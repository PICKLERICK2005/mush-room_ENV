#include <Arduino.h>
#include <Wire.h>
#include <M5UnitENV.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include "secrets.h"

constexpr int SDA_PIN = 2;
constexpr int SCL_PIN = 1;
constexpr uint32_t SENSOR_INTERVAL_MS = 5000UL;
constexpr uint32_t LOG_INTERVAL_MS = 300000UL;
constexpr uint32_t WIFI_RETRY_MS = 30000UL;
constexpr size_t LOG_CHUNK_TARGET_BYTES = 64 * 1024;
constexpr float LOG_FS_BUDGET_RATIO = 0.80f;
constexpr time_t VALID_EPOCH_MIN = 1700000000;

SHT3X sht30;
QMP6988 qmp6988;
WebServer server(80);
Preferences prefs;

struct LiveReading {
  bool valid = false;
  float tempC = NAN;
  float rh = NAN;
  float pressureHpa = NAN;
  float dewPointC = NAN;
};
LiveReading latest;

struct Aggregate {
  uint32_t count = 0;
  double sumTemp = 0, sumRh = 0, sumPressure = 0, sumDew = 0;
  float tempMin = INFINITY, tempMax = -INFINITY;
  float rhMin = INFINITY, rhMax = -INFINITY;

  void reset() {
    count = 0;
    sumTemp = sumRh = sumPressure = sumDew = 0;
    tempMin = rhMin = INFINITY;
    tempMax = rhMax = -INFINITY;
  }

  void add(const LiveReading& r) {
    if (!r.valid) return;
    ++count;
    sumTemp += r.tempC;
    sumRh += r.rh;
    sumPressure += r.pressureHpa;
    sumDew += r.dewPointC;
    tempMin = min(tempMin, r.tempC);
    tempMax = max(tempMax, r.tempC);
    rhMin = min(rhMin, r.rh);
    rhMax = max(rhMax, r.rh);
  }
};
Aggregate agg;

uint32_t sessionId = 0;
uint32_t recordSeq = 0;
uint32_t lastSensorMs = 0, lastLogMs = 0, lastWifiRetryMs = 0, lastValidSampleMs = 0;
bool firstRecordPending = true, filesystemOk = false, ntpConfigured = false, mdnsStarted = false;

uint32_t currentChunkId = 0, oldestChunkId = 0;
size_t currentChunkSize = 0, logBytes = 0, logFileCount = 0, logBudgetBytes = 0;

float dewPointC(float tempC, float rh) {
  if (rh <= 0.0f || rh > 100.0f || !isfinite(tempC)) return NAN;
  constexpr float a = 17.62f, b = 243.12f;
  float gamma = logf(rh / 100.0f) + (a * tempC) / (b + tempC);
  return (b * gamma) / (a - gamma);
}

String isoUtc(time_t t) {
  if (t < VALID_EPOCH_MIN) return "";
  struct tm tmUtc {};
  gmtime_r(&t, &tmUtc);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
  return String(buf);
}

String chunkPath(uint32_t id) {
  char path[32];
  snprintf(path, sizeof(path), "/logs/log_%08lu.csv", (unsigned long)id);
  return String(path);
}

bool parseChunkId(const char* name, uint32_t& out) {
  if (!name) return false;
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  unsigned long id = 0;
  if (sscanf(base, "log_%08lu.csv", &id) == 1) {
    out = (uint32_t)id;
    return true;
  }
  return false;
}

String csvHeader() {
  return "session,seq,epoch_utc,iso_utc,uptime_s,temp_avg_c,rh_avg_pct,"
         "pressure_avg_hpa,dewpoint_avg_c,temp_min_c,temp_max_c,rh_min_pct,"
         "rh_max_pct,samples\n";
}

void scanLogs() {
  logBytes = logFileCount = 0;
  currentChunkId = oldestChunkId = 0;
  currentChunkSize = 0;

  if (!filesystemOk) return;
  if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");

  File dir = LittleFS.open("/logs");
  if (!dir || !dir.isDirectory()) return;

  File file = dir.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      uint32_t id = 0;
      if (parseChunkId(file.name(), id)) {
        size_t sz = file.size();
        logBytes += sz;
        ++logFileCount;
        if (oldestChunkId == 0 || id < oldestChunkId) oldestChunkId = id;
        if (id > currentChunkId) {
          currentChunkId = id;
          currentChunkSize = sz;
        }
      }
    }
    file = dir.openNextFile();
  }
  dir.close();
}

bool createChunk(uint32_t id) {
  String path = chunkPath(id);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return false;
  String header = csvHeader();
  size_t n = f.print(header);
  f.close();

  currentChunkId = id;
  if (oldestChunkId == 0) oldestChunkId = id;
  currentChunkSize = n;
  logBytes += n;
  ++logFileCount;
  return n == header.length();
}

uint32_t nextExistingChunk(uint32_t startId) {
  if (currentChunkId == 0) return 0;
  for (uint32_t id = startId; id <= currentChunkId; ++id)
    if (LittleFS.exists(chunkPath(id))) return id;
  return 0;
}

void enforceLogBudget() {
  while (filesystemOk && logBytes > logBudgetBytes &&
         oldestChunkId != 0 && oldestChunkId < currentChunkId) {
    String path = chunkPath(oldestChunkId);
    File f = LittleFS.open(path, FILE_READ);
    size_t bytes = f ? f.size() : 0;
    if (f) f.close();

    if (LittleFS.remove(path)) {
      logBytes = bytes <= logBytes ? logBytes - bytes : 0;
      if (logFileCount) --logFileCount;
    }

    oldestChunkId = nextExistingChunk(oldestChunkId + 1);
    if (oldestChunkId == 0 && currentChunkId) oldestChunkId = currentChunkId;
  }
}

bool ensureWritableChunk(size_t incoming) {
  if (!filesystemOk) return false;
  if (currentChunkId == 0) return createChunk(1);
  if (currentChunkSize + incoming >= LOG_CHUNK_TARGET_BYTES)
    return createChunk(currentChunkId + 1);
  return true;
}

void clearLogs() {
  if (!filesystemOk) return;
  if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");

  File dir = LittleFS.open("/logs");
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile();
    while (file) {
      String path = file.name();
      bool isDir = file.isDirectory();
      file.close();
      if (!isDir) LittleFS.remove(path);
      file = dir.openNextFile();
    }
  }
  if (dir) dir.close();

  currentChunkId = oldestChunkId = 0;
  currentChunkSize = logBytes = logFileCount = 0;
  recordSeq = 0;
  createChunk(1);
}

bool appendAggregateRecord() {
  if (!filesystemOk || agg.count == 0) return false;

  float t = agg.sumTemp / agg.count;
  float rh = agg.sumRh / agg.count;
  float p = agg.sumPressure / agg.count;
  float dp = agg.sumDew / agg.count;

  time_t now = time(nullptr);
  bool validTime = now >= VALID_EPOCH_MIN;

  String line;
  line.reserve(220);
  line += String(sessionId) + ",";
  line += String(recordSeq++) + ",";
  line += (validTime ? String((unsigned long)now) : "0") + ",";
  line += (validTime ? isoUtc(now) : "") + ",";
  line += String(millis() / 1000UL) + ",";
  line += String(t, 2) + ",";
  line += String(rh, 2) + ",";
  line += String(p, 2) + ",";
  line += String(dp, 2) + ",";
  line += String(agg.tempMin, 2) + ",";
  line += String(agg.tempMax, 2) + ",";
  line += String(agg.rhMin, 2) + ",";
  line += String(agg.rhMax, 2) + ",";
  line += String(agg.count) + "\n";

  if (!ensureWritableChunk(line.length())) return false;

  File f = LittleFS.open(chunkPath(currentChunkId), FILE_APPEND);
  if (!f) return false;
  size_t written = f.print(line);
  f.close();

  currentChunkSize += written;
  logBytes += written;
  enforceLogBudget();
  return written == line.length();
}

void maybeStartMdnsAndNtp() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin("mushroom");
    if (mdnsStarted) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS: http://mushroom.local/");
    }
  }

  if (!ntpConfigured) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
    ntpConfigured = true;
    Serial.println("NTP sync requested.");
  }
}

void startNetworking() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname("mushroom-env");

  if (WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.printf("Fallback AP: %s\n", AP_SSID);
    Serial.printf("AP dashboard: http://%s/\n", WiFi.softAPIP().toString().c_str());
  }

  if (strlen(WIFI_SSID) > 0) {
    Serial.printf("Trying Wi-Fi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    Serial.println("House Wi-Fi not configured; AP-only mode.");
  }
}

void maintainWifi() {
  static wl_status_t previous = WL_NO_SHIELD;
  wl_status_t status = WiFi.status();

  if (status != previous) {
    previous = status;
    if (status == WL_CONNECTED) {
      Serial.printf("STA connected: http://%s/\n", WiFi.localIP().toString().c_str());
      maybeStartMdnsAndNtp();
    }
  }

  if (strlen(WIFI_SSID) == 0 || status == WL_CONNECTED) return;

  uint32_t nowMs = millis();
  if (nowMs - lastWifiRetryMs >= WIFI_RETRY_MS) {
    lastWifiRetryMs = nowMs;
    Serial.println("Retrying house Wi-Fi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setClockFromEpoch(time_t epoch) {
  if (epoch < VALID_EPOCH_MIN) return;
  struct timeval tv {};
  tv.tv_sec = epoch;
  settimeofday(&tv, nullptr);
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mushroom ENV Logger</title>
<style>
:root{color-scheme:dark;font-family:system-ui,-apple-system,Segoe UI,sans-serif}
body{margin:0;background:#111418;color:#eef2f5}main{width:min(920px,calc(100% - 28px));margin:28px auto}
h1{margin:0 0 4px}.sub{opacity:.7;margin-bottom:22px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.card{background:#1c2127;border:1px solid #2b333c;border-radius:14px;padding:16px}.label{opacity:.65;font-size:.85rem}
.value{font-size:1.8rem;font-weight:700;margin-top:5px}.small{font-size:1rem;line-height:1.5}.actions{margin-top:16px;display:flex;flex-wrap:wrap;gap:10px}
button,a.button{border:0;border-radius:10px;padding:11px 14px;background:#2b78e4;color:white;text-decoration:none;font:inherit;cursor:pointer}
button.danger{background:#a83434}code{word-break:break-all}</style></head>
<body><main><h1>Mushroom ENV Logger</h1>
<div class="sub">NanoC6 + ENV III · offline-first environmental recorder</div>
<section class="grid">
<div class="card"><div class="label">Temperature</div><div class="value" id="temp">--</div></div>
<div class="card"><div class="label">Humidity</div><div class="value" id="rh">--</div></div>
<div class="card"><div class="label">Dew point</div><div class="value" id="dew">--</div></div>
<div class="card"><div class="label">Pressure</div><div class="value" id="pressure">--</div></div>
</section>
<section class="grid" style="margin-top:12px">
<div class="card small"><strong>Logger</strong><br>Session: <span id="session">--</span><br>Uptime: <span id="uptime">--</span><br>Clock: <span id="clock">--</span><br>Sample age: <span id="age">--</span></div>
<div class="card small"><strong>Storage</strong><br>Logs: <span id="logbytes">--</span><br>Files: <span id="files">--</span><br>Filesystem: <span id="fs">--</span></div>
<div class="card small"><strong>Network</strong><br>STA: <span id="sta">--</span><br>RSSI: <span id="rssi">--</span><br>Direct AP: <code>MUSHROOM-ENV</code><br>AP IP: <code>192.168.4.1</code></div>
</section>
<div class="actions"><a class="button" href="/download.csv">Download all CSV</a>
<button id="sync">Sync clock from this device</button><button class="danger" id="clear">Clear logs</button></div>
</main>
<script>
const $=id=>document.getElementById(id);
function bytes(n){if(n<1024)return n+" B";if(n<1048576)return(n/1024).toFixed(1)+" KiB";return(n/1048576).toFixed(2)+" MiB"}
function uptime(sec){const d=Math.floor(sec/86400);sec%=86400;const h=Math.floor(sec/3600);sec%=3600;const m=Math.floor(sec/60);const s=sec%60;return`${d}d ${h}h ${m}m ${s}s`}
async function syncClock(){const e=Math.floor(Date.now()/1000);await fetch(`/api/time?epoch=${e}`,{method:"POST"})}
async function refresh(){try{const j=await(await fetch("/api/live",{cache:"no-store"})).json();
$("temp").textContent=j.valid?j.temp_c.toFixed(2)+" °C":"--";$("rh").textContent=j.valid?j.rh_pct.toFixed(2)+" %":"--";
$("dew").textContent=j.valid?j.dewpoint_c.toFixed(2)+" °C":"--";$("pressure").textContent=j.valid?j.pressure_hpa.toFixed(2)+" hPa":"--";
$("session").textContent=j.session;$("uptime").textContent=uptime(j.uptime_s);$("clock").textContent=j.time_valid?new Date(j.epoch*1000).toLocaleString():"unsynchronised";
$("age").textContent=j.valid?(j.sample_age_ms/1000).toFixed(1)+" s":"--";$("logbytes").textContent=`${bytes(j.log_bytes)} / ${bytes(j.log_budget_bytes)}`;
$("files").textContent=j.log_files;$("fs").textContent=`${bytes(j.fs_used_bytes)} / ${bytes(j.fs_total_bytes)}`;
$("sta").textContent=j.sta_connected?j.sta_ip:"offline";$("rssi").textContent=j.sta_connected?`${j.rssi_dbm} dBm`:"--"}catch(e){console.log(e)}}
$("sync").onclick=async()=>{await syncClock();await refresh()};
$("clear").onclick=async()=>{if(!confirm("Delete all stored environmental logs?"))return;await fetch("/clear",{method:"POST"});await refresh()};
syncClock().then(refresh);setInterval(refresh,2000);
</script></body></html>
)HTML";

void sendLiveJson() {
  String json;
  json.reserve(650);
  time_t now = time(nullptr);
  bool tv = now >= VALID_EPOCH_MIN;
  bool sta = WiFi.status() == WL_CONNECTED;

  json += "{";
  json += "\"valid\":" + String(latest.valid ? "true" : "false");
  json += ",\"temp_c\":" + String(latest.valid ? latest.tempC : 0, 3);
  json += ",\"rh_pct\":" + String(latest.valid ? latest.rh : 0, 3);
  json += ",\"pressure_hpa\":" + String(latest.valid ? latest.pressureHpa : 0, 3);
  json += ",\"dewpoint_c\":" + String(latest.valid ? latest.dewPointC : 0, 3);
  json += ",\"sample_age_ms\":" + String(latest.valid ? millis() - lastValidSampleMs : 0);
  json += ",\"session\":" + String(sessionId);
  json += ",\"uptime_s\":" + String(millis() / 1000UL);
  json += ",\"time_valid\":" + String(tv ? "true" : "false");
  json += ",\"epoch\":" + String(tv ? (unsigned long)now : 0);
  json += ",\"log_bytes\":" + String(logBytes);
  json += ",\"log_budget_bytes\":" + String(logBudgetBytes);
  json += ",\"log_files\":" + String(logFileCount);
  json += ",\"fs_used_bytes\":" + String(filesystemOk ? LittleFS.usedBytes() : 0);
  json += ",\"fs_total_bytes\":" + String(filesystemOk ? LittleFS.totalBytes() : 0);
  json += ",\"sta_connected\":" + String(sta ? "true" : "false");
  json += ",\"sta_ip\":\"" + String(sta ? WiFi.localIP().toString() : "") + "\"";
  json += ",\"rssi_dbm\":" + String(sta ? WiFi.RSSI() : 0);
  json += "}";

  server.send(200, "application/json", json);
}

void downloadCsv() {
  if (!filesystemOk || currentChunkId == 0) {
    server.send(503, "text/plain", "Filesystem/logs unavailable.");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"mushroom_env_log.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");

  bool headerSent = false;
  for (uint32_t id = oldestChunkId; id != 0 && id <= currentChunkId; ++id) {
    String path = chunkPath(id);
    if (!LittleFS.exists(path)) continue;
    File f = LittleFS.open(path, FILE_READ);
    if (!f) continue;

    bool firstLine = true;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line += "\n";
      if (firstLine) {
        firstLine = false;
        if (headerSent) continue;
        headerSent = true;
      }
      server.sendContent(line);
      yield();
    }
    f.close();
  }
  server.sendContent("");
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](){ server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/api/live", HTTP_GET, [](){ sendLiveJson(); });

  server.on("/api/time", HTTP_POST, [](){
    if (!server.hasArg("epoch")) { server.send(400, "text/plain", "Missing epoch."); return; }
    time_t epoch = (time_t)strtoull(server.arg("epoch").c_str(), nullptr, 10);
    if (epoch < VALID_EPOCH_MIN) { server.send(400, "text/plain", "Invalid epoch."); return; }
    setClockFromEpoch(epoch);
    server.send(204);
  });

  server.on("/download.csv", HTTP_GET, [](){ downloadCsv(); });
  server.on("/clear", HTTP_POST, [](){ clearLogs(); server.send(200, "text/plain", "Logs cleared."); });
  server.onNotFound([](){ server.send(404, "text/plain", "Not found."); });
  server.begin();
}

bool initSensors() {
  if (!qmp6988.begin(&Wire, QMP6988_SLAVE_ADDRESS_L, SDA_PIN, SCL_PIN, 400000U)) return false;
  if (!sht30.begin(&Wire, SHT3X_I2C_ADDR, SDA_PIN, SCL_PIN, 400000U)) return false;
  return true;
}

void sampleSensors() {
  if (!sht30.update() || !qmp6988.update()) {
    Serial.println("WARNING: sensor read failed.");
    return;
  }

  latest.valid = true;
  latest.tempC = sht30.cTemp;
  latest.rh = sht30.humidity;
  latest.pressureHpa = qmp6988.pressure / 100.0f;
  latest.dewPointC = dewPointC(latest.tempC, latest.rh);
  lastValidSampleMs = millis();
  agg.add(latest);

  Serial.printf("LIVE T=%.2fC RH=%.2f%% P=%.2fhPa DP=%.2fC\n",
                latest.tempC, latest.rh, latest.pressureHpa, latest.dewPointC);
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println("\nMUSHROOM ENV LOGGER v0.1");

  prefs.begin("mushlog", false);
  sessionId = prefs.getULong("session", 0) + 1;
  prefs.putULong("session", sessionId);

  filesystemOk = LittleFS.begin(true);
  if (filesystemOk) {
    if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");
    logBudgetBytes = (size_t)(LittleFS.totalBytes() * LOG_FS_BUDGET_RATIO);
    scanLogs();
    enforceLogBudget();
    Serial.printf("LittleFS: %u/%u bytes; log budget=%u\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
                  (unsigned)logBudgetBytes);
  } else {
    Serial.println("ERROR: LittleFS mount failed; live monitoring only.");
  }

  if (initSensors()) Serial.println("ENV III detected.");
  else Serial.println("ERROR: ENV III init failed.");

  agg.reset();
  startNetworking();
  setupWebServer();

  Serial.printf("Direct dashboard: http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.println("LAN dashboard when connected: http://mushroom.local/");

  lastSensorMs = millis() - SENSOR_INTERVAL_MS;
  lastLogMs = millis();
}

void loop() {
  server.handleClient();
  maintainWifi();

  uint32_t nowMs = millis();

  if (nowMs - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = nowMs;
    sampleSensors();

    if (firstRecordPending && agg.count > 0) {
      Serial.println(appendAggregateRecord() ? "LOG first record written." : "ERROR first record write failed.");
      agg.reset();
      firstRecordPending = false;
      lastLogMs = nowMs;
    }
  }

  if (!firstRecordPending && nowMs - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = nowMs;
    if (agg.count > 0) {
      Serial.println(appendAggregateRecord() ? "LOG 5-minute record written." : "ERROR log write failed.");
      agg.reset();
    }
  }

  delay(2);
}
