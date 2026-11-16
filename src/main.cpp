#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include "pins.h"
#include "analysis_page.h"
#include "vocal_dsp.h"

namespace {
constexpr char WIFI_SSID[] = "VocalLab";
constexpr char WIFI_PASS[] = "vocal1234";

SPIClass spi(FSPI);
WebServer server(80);

bool codecReady = false;
bool sdReady = false;
File playbackFile;
bool playing = false;
File recordFile;
bool recording = false;
bool stoppingRecord = false;
uint32_t recordBytes = 0;
uint32_t stopRequestedAt = 0;
String recordError;
constexpr char RECORD_PLUGIN[] = "/vs1053b_24k_l.img";
String playbackError;
uint32_t playbackBytes = 0;
File uploadFile;
String uploadTarget = "/reference.wav";
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

// The VLSI .img plugin is a P&H record stream; not a raw .plg patch.
bool readPluginByte(File &f, uint8_t &b) {
  int v = f.read();
  if (v < 0) return false;
  b = uint8_t(v);
  return true;
}
bool readPluginWord(File &f, uint16_t &w) {
  uint8_t hi, lo;
  if (!readPluginByte(f, hi) || !readPluginByte(f, lo)) return false;
  w = (uint16_t(hi) << 8) | lo;
  return true;
}
bool loadRecorderPlugin(uint16_t &entry) {
  File f = SD.open(RECORD_PLUGIN, FILE_READ);
  if (!f) { recordError = "Нет /vs1053b_24k_l.img на SD"; return false; }
  uint8_t b;
  if (!readPluginByte(f,b) || b!='P' || !readPluginByte(f,b) || b!='&' ||
      !readPluginByte(f,b) || b!='H') {
    recordError = "Некорректный заголовок P&H"; f.close(); return false;
  }
  const uint16_t offsets[3] = {0x8000, 0, 0x4000};
  uint32_t wordsTotal = 0;
  while (f.available()) {
    uint8_t type;
    uint16_t len, addr;
    if (!readPluginByte(f,type) || !readPluginWord(f,len) || !readPluginWord(f,addr)) break;
    len &= 0xFFFE;
    if (type == 3) {
      f.close(); entry = addr;
      return wordsTotal > 0 && entry == 0x34;
    }
    if (type > 2 || !len || wordsTotal + len/2 > 100000) break;
    if (!writeSci(0x07, addr + offsets[type])) break; // SCI_WRAMADDR
    for (uint16_t i=0; i<len/2; i++) {
      uint16_t w;
      if (!readPluginWord(f,w) || !writeSci(0x06,w)) {
        recordError = "Ошибка передачи плагина";
        f.close(); return false;
      }
    }
    wordsTotal += len/2;
  }
  recordError = "Ошибка формата плагина";
  f.close();
  return false;
}

void abortRecording(const String &reason) {
  recordError = reason;
  if (recordFile) recordFile.close();
  SD.remove("/recording.part");
  recording = false;
  stoppingRecord = false;
  // Encoder state cannot be safely resumed without a codec reset.
  digitalWrite(PIN_XRST, LOW); delay(10);
  digitalWrite(PIN_XRST, HIGH); delay(100);
  testCodec();
}

void serviceRecording() {
  if (!recording) return;
  uint16_t availableWords=0;
  if (!readSci(0x09,availableWords)) {abortRecording("PCM FIFO timeout");return;}
  uint16_t flags=0;
  if (stoppingRecord) {
    if (!readSci(0x0F,flags)) {abortRecording("PCM stop status timeout");return;}
    if (millis()-stopRequestedAt>10000) {abortRecording("PCM stop timeout");return;}
  }
  bool finished=stoppingRecord && (flags&2);
  if (availableWords<256 && !finished) return;
  uint16_t count=min(uint16_t(256),availableWords);
  uint8_t bytes[512];
  for(uint16_t i=0;i<count;i++){
    uint16_t w;
    if(!readSci(0x08,w)){abortRecording("PCM data timeout");return;}
    bytes[2*i]=uint8_t(w>>8);bytes[2*i+1]=uint8_t(w);
  }
  if(count && recordFile.write(bytes,count*2)!=count*2){
    abortRecording("PCM SD write failed");return;
  }
  recordBytes+=count*2;
  if(!finished || availableWords>count)return;
  recordFile.flush();recordFile.close();
  recording=false;stoppingRecord=false;
  if(recordBytes<44){recordError="WAV too short";SD.remove("/recording.part");return;}
  File wav=SD.open("/recording.part","r+");
  uint8_t header[44];
  if(!wav || wav.read(header,44)!=44 || memcmp(header,"RIFF",4) ||
     memcmp(header+8,"WAVE",4) || memcmp(header+36,"data",4)){
    if(wav)wav.close();
    recordError="Invalid PCM WAV header";SD.remove("/recording.part");return;
  }
  uint8_t sizes[4];
  auto patch=[&](uint32_t offset,uint32_t value){
    for(int i=0;i<4;i++)sizes[i]=uint8_t(value>>(i*8));
    return wav.seek(offset)&&wav.write(sizes,4)==4;
  };
  bool ok=patch(4,recordBytes-8)&&patch(40,recordBytes-44);
  wav.flush();wav.close();
  if(!ok){recordError="WAV header write failed";return;}
  SD.remove("/recording.wav");
  if(!SD.rename("/recording.part","/recording.wav")){
    recordError="WAV rename failed";return;
  }
  digitalWrite(PIN_XRST,LOW);delay(10);
  digitalWrite(PIN_XRST,HIGH);delay(100);
  testCodec();
}

void startRecording() {
  if (recording || playing || !codecReady || !sdReady) {
    server.send(409,"text/plain; charset=utf-8","Кодек занят или недоступен"); return;
  }
  recordError = "";
  recordBytes = 0;
  // Recorder initialization resets decoder and loads application from SD.
  if (!writeSci(0x03,0xC000) || !writeSci(0x02,0) ||
      !writeSci(0x00,0x0804)) {
    server.send(500,"text/plain","Ошибка инициализации VS1053"); return;
  }
  if (!writeSci(0x0A,0) || !writeSci(0x07,0xC01A) ||
      !writeSci(0x06,2)) {
    server.send(500,"text/plain","Ошибка настройки SCI"); return;
  }
  uint16_t entry = 0;
  if (!loadRecorderPlugin(entry)) {
    server.send(500,"text/plain; charset=utf-8",recordError); return;
  }
  SD.remove("/recording.part");
  recordFile = SD.open("/recording.part",FILE_WRITE);
  if (!recordFile) { server.send(500,"text/plain","SD open failed"); return; }
  // Mic input: SM_LINE1=0; SM_ADPCM | SM_SDINEW.
  if (!writeSci(0x00,0x1004) || !writeSci(0x0C,0x8000) ||
      !writeSci(0x0D,1024) || !writeSci(0x0E,0) ||
      !writeSci(0x0F,0) || !writeSci(0x0A,entry)) {
    abortRecording("Не удалось запустить encoder");
    server.send(500,"text/plain; charset=utf-8",recordError); return;
  }
  recording = true;
  stoppingRecord = false;
  server.send(200,"text/plain; charset=utf-8","Запись началась");
}

void requestStopRecording() {
  if (!recording || stoppingRecord) {
    server.send(409,"text/plain; charset=utf-8","Нет активной записи"); return;
  }
  if (!writeSci(0x0F,1)) { abortRecording("Stop command failed"); server.send(500,"text/plain",recordError); return; }
  stoppingRecord = true;
  stopRequestedAt = millis();
  server.send(200,"text/plain; charset=utf-8","Завершение Ogg...");
}

void startPlayback() {
  if (recording || !codecReady || !sdReady) {
    server.send(503, "text/plain; charset=utf-8", "VS1053 или SD недоступны");
    return;
  }
  stopPlayback();
  playbackError = "";
  // Explicit fixed filename avoids unsafe paths from HTTP parameters.
  const bool oggPlayback = server.uri() == "/api/play/ogg";
  playbackFile = SD.open(oggPlayback ? "/reference.ogg" : "/test.mp3", FILE_READ);
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
  uploadTarget = server.uri() == "/api/reference/ogg" ? "/reference.ogg" : "/reference.wav";
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    stopPlayback();
    uploadFailed = !sdReady;
    uploadBytes = 0;
    if (uploadFile) uploadFile.close();
    if (!uploadFailed) {
      SD.remove(uploadTarget.c_str());
      uploadFile = SD.open(uploadTarget.c_str(), FILE_WRITE);
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
  server.send(200, "text/plain; charset=utf-8", "Файл сохранён: " + uploadTarget + " (" + String(uploadBytes) + " байт)");
}

void handleOggDownload() {
  if (!sdReady || !SD.exists("/reference.ogg")) {
    server.send(404, "text/plain; charset=utf-8", "Ogg отсутствует");
    return;
  }
  stopPlayback();
  File file = SD.open("/reference.ogg", FILE_READ);
  if (!file) { server.send(500, "text/plain", "SD read error"); return; }
  server.streamFile(file, "audio/ogg");
  file.close();
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
  out += ",\"recording\":" + String(recording ? "true" : "false");
  out += ",\"record_stopping\":" + String(stoppingRecord ? "true" : "false");
  out += ",\"record_bytes\":" + String(recordBytes);
  out += ",\"record_error\":\"" + recordError + "\"";
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
<p><a href="/analysis" style="color:#93c5fd">Анализ вокала: ноты и громкость →</a></p>
<div class="card"><h2>Анализ на ESP32-S3</h2>
<p class="muted">Обрабатывает reference.wav (моно PCM16, 8–48 кГц) без браузера. Ogg пока требует декодера.</p>
<button onclick="audio('/api/analyze/wav')">Вычислить ноты и громкость на ESP32</button>
<a href="/api/analyze/result" style="color:#93c5fd">Скачать результат JSON</a></div>
<button onclick="refresh()">Обновить</button>
<script>
async function uploadOgg(){
  const f=document.getElementById('ogg').files[0], msg=document.getElementById('oggStatus');
  if(!f){msg.textContent='Выберите Ogg';return;}
  if(f.size>12*1024*1024){msg.textContent='Файл больше 12 МБ';return;}
  const fd=new FormData();fd.append('file',f);
  msg.textContent='Загрузка...';
  try {const r=await fetch('/api/reference/ogg',{method:'POST',body:fd});msg.textContent=await r.text();}
  catch(e){msg.textContent=String(e);}
}
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
    document.getElementById('recStatus').textContent=(s.recording?(s.record_stopping?'Завершение':'Идёт запись'):'Остановлено')+' · '+s.record_bytes+' байт'+(s.record_error?' · '+s.record_error:'');
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
  server.on("/api/analyze/wav",HTTP_POST,[](){
    if(recording || playing || !sdReady){server.send(409,"text/plain","Device busy");return;}
    File input=SD.open("/reference.wav",FILE_READ);
    if(!input){server.send(404,"text/plain","reference.wav missing");return;}
    SD.remove("/reference-analysis.part");
    File output=SD.open("/reference-analysis.part",FILE_WRITE);
    if(!output){input.close();server.send(500,"text/plain","SD write error");return;}
    String error;
    bool ok=VocalDSP::analyzeWav(input,output,error);
    output.close();input.close();
    if(!ok){SD.remove("/reference-analysis.part");server.send(422,"text/plain; charset=utf-8",error);return;}
    SD.remove("/reference-analysis.json");
    if(!SD.rename("/reference-analysis.part","/reference-analysis.json")){
      server.send(500,"text/plain","Rename failed");return;
    }
    server.send(200,"text/plain; charset=utf-8","Анализ сохранён на microSD");
  });
  server.on("/api/analyze/result",HTTP_GET,[](){
    if(!sdReady || !SD.exists("/reference-analysis.json")){
      server.send(404,"text/plain","No analysis");return;
    }
    File f=SD.open("/reference-analysis.json",FILE_READ);
    if(!f){server.send(500,"text/plain","SD read error");return;}
    server.streamFile(f,"application/json");f.close();
  });
  server.on("/analysis", HTTP_GET, [](){
    server.sendHeader("Content-Encoding","gzip");
    server.sendHeader("Cache-Control","no-store");
    server.send_P(200,"text/html; charset=utf-8",
                  reinterpret_cast<const char*>(ANALYSIS_HTML_GZ), ANALYSIS_HTML_GZ_SIZE);
  });
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
  server.on("/api/reference/ogg", HTTP_POST, handleUploadComplete, handleUploadChunk);
  server.on("/api/reference/ogg", HTTP_GET, handleOggDownload);
  server.on("/api/reference", HTTP_GET, handleReferenceDownload);
  server.on("/api/record/start",HTTP_POST,startRecording);
  server.on("/api/play/ogg",HTTP_POST,startPlayback);
  server.on("/api/record/stop",HTTP_POST,requestStopRecording);
  server.on("/api/record/file",HTTP_GET,[](){
    if(recording || !sdReady || !SD.exists("/recording.ogg")){
      server.send(404,"text/plain","No completed recording"); return;
    }
    File f=SD.open("/recording.ogg",FILE_READ);
    if(!f){server.send(500,"text/plain","SD error");return;}
    server.streamFile(f,"audio/ogg");f.close();
  });
  server.begin();
}

void loop() {
  server.handleClient();
  servicePlayback();
  serviceRecording();
  delay(1);
}
