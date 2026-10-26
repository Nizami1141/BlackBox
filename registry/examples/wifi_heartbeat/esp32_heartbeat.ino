#include <WiFi.h>
#include <HTTPClient.h>
#include <stdarg.h>
#include "config.h"

unsigned long lastHeartbeat = 0;
unsigned long lastFlush = 0;
bool registered = false;

// ---------- буфер логов ----------
struct LogItem {
  const char *level;
  unsigned long uptime;
  String msg;
};
LogItem logBuf[LOG_BUFFER_SIZE];
int logCount = 0;

// Пишет в Serial и складывает в буфер для отправки на сервер.
// Использование: logf("info", "температура: %.1f", t);
void logf(const char *level, const char *fmt, ...) {
  char tmp[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  Serial.printf("[%s] %s\n", level, tmp);

  if (logCount == LOG_BUFFER_SIZE) {           // буфер полон — выбрасываем самый старый
    for (int i = 1; i < LOG_BUFFER_SIZE; i++) logBuf[i - 1] = logBuf[i];
    logCount--;
  }
  logBuf[logCount++] = { level, millis(), String(tmp) };
}

String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((uint8_t)c < 0x20) { /* пропускаем управляющие */ }
    else o += c;
  }
  return o;
}

// ---------- сеть ----------
void connectWifi() {
  Serial.printf("[wifi] подключаюсь к %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] подключено, IP: %s\n", WiFi.localIP().toString().c_str());
}

String serverUrl(const String &path) {
  return String("http://") + SERVER_HOST + ":" + String(SERVER_PORT) + path;
}

int postJson(const String &path, const String &body) {
  HTTPClient http;
  http.begin(serverUrl(path));
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_KEY) > 0) http.addHeader("X-API-Key", API_KEY);
  int code = http.POST(body);
  http.end();
  return code;
}

bool registerDevice() {
  String body = String("{\"device_id\":\"") + DEVICE_ID +
                "\",\"region\":\"" + DEVICE_REGION +
                "\",\"name\":\"" + jsonEscape(DEVICE_NAME) + "\"}";
  int code = postJson("/api/v1/devices/register", body);
  Serial.printf("[register] код ответа: %d\n", code);
  return code == 200;
}

bool sendHeartbeat() {
  String body = String("{\"ip\":\"") + WiFi.localIP().toString() +
                "\",\"fw_version\":\"" + FW_VERSION + "\"}";
  return postJson(String("/api/v1/devices/") + DEVICE_ID + "/heartbeat", body) == 200;
}

// Отправляет все накопленные логи одним запросом. При ошибке логи остаются в буфере.
bool flushLogs() {
  if (logCount == 0) return true;
  int n = logCount;
  String body = "{\"logs\":[";
  for (int i = 0; i < n; i++) {
    if (i) body += ",";
    body += String("{\"level\":\"") + logBuf[i].level + "\",\"uptime_ms\":" + String(logBuf[i].uptime) +
            ",\"message\":\"" + jsonEscape(logBuf[i].msg) + "\"}";
  }
  body += "]}";

  int code = postJson(String("/api/v1/devices/") + DEVICE_ID + "/logs", body);
  if (code == 200) {
    // убираем отправленные (за время запроса могли добавиться новые — они сдвигаются в начало)
    for (int i = n; i < logCount; i++) logBuf[i - n] = logBuf[i];
    logCount -= n;
    return true;
  }
  if (code == 404) registered = false;  // сервер не знает устройство — перерегистрируемся
  Serial.printf("[logs] ошибка отправки, код: %d\n", code);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("=== %s (регион: %s) ===\n", DEVICE_ID, DEVICE_REGION);
  connectWifi();
  registered = registerDevice();
  logf("info", "boot: fw %s, IP %s", FW_VERSION, WiFi.localIP().toString().c_str());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    logf("warn", "Wi-Fi потерян, переподключаюсь");
    connectWifi();
    logf("info", "Wi-Fi восстановлен");
  }

  if (!registered) {
    registered = registerDevice();
    delay(2000);
    return;
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    bool ok = sendHeartbeat();
    Serial.printf("[heartbeat] %s\n", ok ? "ok" : "ошибка");
    if (!ok) logf("warn", "heartbeat не прошёл");
  }

  // ==== ваш код: вызывайте logf("info"/"warn"/"error", ...) где нужно ====
  // пример: logf("info", "температура %.1f C, влажность %d%%", temp, hum);

  if (millis() - lastFlush >= LOG_FLUSH_INTERVAL_MS) {
    lastFlush = millis();
    flushLogs();
  }
}
