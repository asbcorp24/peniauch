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

String jsonStatus() {
  String out = "{";
  out += "\"codec_ok\":" + String(codecReady ? "true" : "false");
  out += ",\"sd_ok\":" + String(sdReady ? "true" : "false");
  out += ",\"dreq\":" + String(digitalRead(PIN_DREQ) ? "true" : "false");
  out += ",\"sci_status\":\"" + String(statusRegister, HEX) + "\"";
  out += ",\"ss_ver\":" + String(codecVersion);
  out += ",\"error\":\"" + codecError + "\"";
  out += ",\"sd_mb\":" + String(sdReady ? SD.cardSize() / (1024ULL * 1024ULL) : 0);
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
<p>Карта: <span id="size">—</span></p>
<p class="muted" id="error"></p></div>
<div class="card"><h2>Запись вокала</h2>
<p class="muted">Появится после проверки аппаратного подключения и микрофонного тракта.</p></div>
<button onclick="refresh()">Обновить</button>
<script>
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
  server.begin();
}

void loop() {
  server.handleClient();
  delay(2);
}
