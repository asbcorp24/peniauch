#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include "pins.h"

namespace {
constexpr char WIFI_SSID[] = "VocalLab";
constexpr char WIFI_PASS[] = "vocal1234";

SPIClass spi(FSPI);
WebServer server(80);

bool codecReady = false;
bool sdReady = false;
File playbackFile;
bool playing = false;
String playbackError;
uint32_t playbackBytes = 0;
File uploadFile;
bool uploadFailed = false;
size_t uploadBytes = 0;
constexpr size_t MAX_UPLOAD_BYTES = 12 * 1024 * 1024;
uint16_t statusRegister = 0xFFFF;
uint8_t codecVersion = 0;
String codecError;

void deselectAll() {
  digitalWrite(PIN_XCS, HIGH);
  digitalWrite(PIN_XDCS, HIGH);
  digitalWrite(PIN_SDCS, HIGH);
}

// Wait on DREQ before SCI operations, bounded to avoid a lockup.
bool waitDreq(uint32_t timeoutMs = 500) {
  const uint32_t start = millis();
  while (!digitalRead(PIN_DREQ)) {
    if (millis() - start >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

bool readSci(uint8_t address, uint16_t &value) {
  if (!waitDreq()) return false;
  deselectAll();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_XCS, LOW);
  spi.transfer(0x03); // SCI read
  spi.transfer(address);
  const uint16_t hi = spi.transfer(0xFF);
  const uint16_t lo = spi.transfer(0xFF);
  digitalWrite(PIN_XCS, HIGH);
  spi.endTransaction();
  value = (hi << 8) | lo;
  return waitDreq();
}

void testCodec() {
  digitalWrite(PIN_XRST, LOW);
  delay(10);
  digitalWrite(PIN_XRST, HIGH);
  delay(100);
  if (!waitDreq(1000)) {
    codecError = "DREQ timeout";
    return;
  }
  if (!readSci(0x01, statusRegister)) {
    codecError = "SCI_STATUS read timeout";
    return;
  }
  codecVersion = (statusRegister >> 4) & 0x0F;
  // VS1053 reports SS_VER=4. This is stronger than accepting any nonzero data.
  codecReady = (codecVersion == 4);
  if (!codecReady) codecError = "Unexpected SCI_STATUS SS_VER";
}

bool writeSci(uint8_t address, uint16_t value) {
  if (!waitDreq()) return false;
  deselectAll();
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_XCS, LOW);
  spi.transfer(0x02);
  spi.transfer(address);
  spi.transfer(value >> 8);
  spi.transfer(value & 0xFF);
  digitalWrite(PIN_XCS, HIGH);
  spi.endTransaction();
  return waitDreq();
}

void stopPlayback() {
  if (playbackFile) playbackFile.close();
  playing = false;
}

void servicePlayback() {
  if (!playing) return;
  // Small, bounded SDI chunks keep HTTP responsive.
  for (int batch = 0; batch < 4 && playing; ++batch) {
    if (!digitalRead(PIN_DREQ)) break;
    uint8_t buf[32];
    int n = playbackFile.read(buf, sizeof(buf));
    if (n <= 0) { stopPlayback(); break; }
    deselectAll();
    spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_XDCS, LOW);
    spi.writeBytes(buf, n);
    digitalWrite(PIN_XDCS, HIGH);
    spi.endTransaction();
    playbackBytes += n;
  }
}

void startPlayback() {
  if (!codecReady || !sdReady) {
    server.send(503, "text/plain; charset=utf-8", "VS1053 или SD недоступны");
    return;
  }
  stopPlayback();
  playbackError = "";
  // Explicit fixed filename avoids unsafe paths from HTTP parameters.
  playbackFile = SD.open("/test.mp3", FILE_READ);
  if (!playbackFile || playbackFile.isDirectory()) {
    if (playbackFile) playbackFile.close();
    playbackError = "Нет файла /test.mp3";
    server.send(404, "text/plain; charset=utf-8", playbackError);
    return;
  }
  if (!writeSci(0x0B, 0x3030)) { // SCI_VOL: moderate volume
    stopPlayback();
    playbackError = "Не удалось установить громкость";
    server.send(500, "text/plain; charset=utf-8", playbackError);
    return;
  }
  playbackBytes = 0;
  playing = true;
  server.send(200, "text/plain; charset=utf-8", "Воспроизведение началось");
}

void handleUploadChunk() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    stopPlayback();
    uploadFailed = !sdReady;
    uploadBytes = 0;
    if (uploadFile) uploadFile.close();
    if (!uploadFailed) {
      SD.remove("/reference.wav");
      uploadFile = SD.open("/reference.wav", FILE_WRITE);
      uploadFailed = !uploadFile;
    }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!uploadFailed) {
      uploadBytes += u.currentSize;
      if (uploadBytes > MAX_UPLOAD_BYTES ||
          uploadFile.write(u.buf, u.currentSize) != u.currentSize) {
        uploadFailed = true;
      }
    }
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (u.status == UPLOAD_FILE_ABORTED || uploadFailed) {
      SD.remove("/reference.wav");
      uploadFailed = true;
    }
  }
}

void handleUploadComplete() {
  if (uploadFile) uploadFile.close();
  if (uploadFailed || uploadBytes == 0) {
    SD.remove("/reference.wav");
    server.send(400, "text/plain; charset=utf-8", "Ошибка записи или размер превышает 12 МБ");
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "Эталон сохранён: " + String(uploadBytes) + " байт");
}

void handleReferenceDownload() {
  if (!sdReady || !SD.exists("/reference.wav")) {
    server.send(404, "text/plain; charset=utf-8", "Эталон отсутствует");
    return;
  }
  stopPlayback();
  File file = SD.open("/reference.wav", FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "SD read error");
    return;
  }
  server.streamFile(file, "audio/wav");
  file.close();
}

String jsonStatus() {
  String out = "{";
  out += "\"codec_ok\":" + String(codecReady ? "true" : "false");
  out += ",\"sd_ok\":" + String(sdReady ? "true" : "false");
  out += ",\"dreq\":" + String(digitalRead(PIN_DREQ) ? "true" : "false");
  out += ",\"sci_status\":\"" + String(statusRegister, HEX) + "\"";
  out += ",\"ss_ver\":" + String(codecVersion);
  out += ",\"error\":\"" + codecError + "\"";
  out += ",\"sd_mb\":" + String(sdReady ? SD.cardSize() / (1024ULL * 1024ULL) : 0);
  out += ",\"psram_mb\":" + String(ESP.getPsramSize() / (1024ULL * 1024ULL));
  out += ",\"psram_free_kb\":" + String(ESP.getFreePsram() / 1024);
  out += ",\"playing\":" + String(playing ? "true" : "false");
  out += ",\"playback_bytes\":" + String(playbackBytes);
  out += "}";
  return out;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VocalLab</title>
<style>
body{background:#101827;color:#eef3fc;font:16px system-ui,Arial,sans-serif;margin:0;padding:24px}
main{max-width:640px;margin:auto}.card{background:#1e293b;border-radius:14px;padding:20px;margin:14px 0}
h1{margin-bottom:4px}.muted{color:#a9b5ca}.ok{color:#4ade80}.bad{color:#fb7185}
button{background:#4b78e8;border:0;border-radius:10px;padding:12px 18px;color:white;font-size:16px}
</style></head><body><main>
<h1>VocalLab</h1><p class="muted">ESP32-S3 · HW-261 · автономный тренажёр</p>
<div class="card"><h2>Диагностика оборудования</h2>
<p>VS1053B: <b id="codec">Проверка...</b></p>
<p>microSD: <b id="sd">Проверка...</b></p>
<p>DREQ: <span id="dreq">—</span></p>
<p>SCI_STATUS: <span id="status">—</span></p>
<p>SS_VER: <span id="version">—</span></p>
<p>Карта: <span id="size">—</span></p><p>PSRAM: <span id="psram">—</span></p>
<p class="muted" id="error"></p></div>
<div class="card"><h2>Проверка аудиовыхода</h2>
<p class="muted">Поместите файл test.mp3 в корень microSD. Подключите наушники.</p>
<button onclick="audio('/api/play')">Воспроизвести test.mp3</button>
<button onclick="audio('/api/stop')">Остановить</button>
<p id="playback">—</p></div>
<div class="card"><h2>Запись вокала</h2>
<p class="muted">Появится после проверки аппаратного подключения и микрофонного тракта.</p></div>
<button onclick="refresh()">Обновить</button>
<script>
async function uploadWav(){
  const f=document.getElementById('wav').files[0];
  const msg=document.getElementById('uploadStatus');
  if(!f){msg.textContent='Выберите WAV';return;}
  if(f.size>12*1024*1024){msg.textContent='Файл больше 12 МБ';return;}
  const fd=new FormData();fd.append('file',f);
  msg.textContent='Загрузка...';
  try {
    const r=await fetch('/api/reference',{method:'POST',body:fd});
    msg.textContent=await r.text();
    if(r.ok) await drawWav(f);
  }catch(e){msg.textContent=String(e);}
}
async function drawWav(file){
  try {
    const ctx=new (window.AudioContext||window.webkitAudioContext)();
    const buffer=await ctx.decodeAudioData(await file.arrayBuffer());
    const data=buffer.getChannelData(0);
    const canvas=document.getElementById('wave'),g=canvas.getContext('2d');
    g.clearRect(0,0,canvas.width,canvas.height);
    g.strokeStyle='#60a5fa';g.lineWidth=2;g.beginPath();
    for(let x=0;x<canvas.width;x++){
      const a=Math.floor(x*data.length/canvas.width);
      const b=Math.max(a+1,Math.floor((x+1)*data.length/canvas.width));
      let sum=0;
      for(let i=a;i<b;i++)sum+=data[i]*data[i];
      const rms=Math.sqrt(sum/(b-a));
      const y=canvas.height- Math.min(1,rms*3)*canvas.height;
      if(x===0)g.moveTo(x,y);else g.lineTo(x,y);
    }
    g.stroke();await ctx.close();
  }catch(e){document.getElementById('uploadStatus').textContent='Анализ WAV: '+e;}
}
async function audio(path){
  try { const r=await fetch(path,{method:'POST'}); document.getElementById('playback').textContent=await r.text(); }
  catch(e){document.getElementById('playback').textContent=String(e);}
  refresh();
}
async function refresh(){
  try {
    const r=await fetch('/api/status',{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const s=await r.json();
    for(const [id,yes] of [['codec',s.codec_ok],['sd',s.sd_ok]]) {
      const el=document.getElementById(id);
      el.textContent=yes?'OK':'ОШИБКА';el.className=yes?'ok':'bad';
    }
    document.getElementById('dreq').textContent=s.dreq?'HIGH':'LOW';
    document.getElementById('status').textContent='0x'+s.sci_status;
    document.getElementById('version').textContent=s.ss_ver;
    document.getElementById('size').textContent=s.sd_mb+' МБ';
    document.getElementById('psram').textContent=s.psram_mb+' МБ, свободно '+s.psram_free_kb+' КБ';
    if(s.playing) document.getElementById('playback').textContent='Воспроизведение: '+s.playback_bytes+' байт';
    document.getElementById('error').textContent=s.error||'';
  } catch(e) {document.getElementById('error').textContent=String(e);}
}
refresh();setInterval(refresh,3000);
</script></main></body></html>
)HTML");
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  pinMode(PIN_XCS, OUTPUT);
  pinMode(PIN_XDCS, OUTPUT);
  pinMode(PIN_SDCS, OUTPUT);
  pinMode(PIN_XRST, OUTPUT);
  pinMode(PIN_DREQ, INPUT);
  deselectAll();
  digitalWrite(PIN_XRST, HIGH);

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  testCodec();
  deselectAll();
  sdReady = SD.begin(PIN_SDCS, spi, 1000000);
  Serial.printf("VS1053: %s SCI_STATUS=0x%04X VER=%u %s\n",
                codecReady ? "OK" : "ERROR", statusRegister, codecVersion,
                codecError.c_str());
  Serial.printf("SD: %s\n", sdReady ? "OK" : "ERROR");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.printf("Wi-Fi AP: %s http://%s\n",
                WIFI_SSID, WiFi.softAPIP().toString().c_str());
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, [](){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",jsonStatus());
  });
  server.on("/api/play", HTTP_POST, startPlayback);
  server.on("/api/stop", HTTP_POST, [](){
    stopPlayback();
    server.send(200, "text/plain; charset=utf-8", "Остановлено");
  });
  server.on("/api/reference", HTTP_POST, handleUploadComplete, handleUploadChunk);
  server.on("/api/reference", HTTP_GET, handleReferenceDownload);
  server.begin();
}

void loop() {
  server.handleClient();
  servicePlayback();
  delay(1);
}
