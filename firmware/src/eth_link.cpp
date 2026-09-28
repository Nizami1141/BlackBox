#include "eth_link.h"
#include "config.h"

#if ENABLE_ETH

#include <ArduinoJson.h>
#include <Ethernet.h>
#include <SPI.h>
#include <stdarg.h>
#include <utility/w5100.h>

namespace {

byte g_mac[] = ETH_MAC;
bool g_up = false;
bool g_asleep = false;

const uint8_t PHY_PWDN = 0x40 | (0x6 << 3);
const uint8_t PHY_AUTO = 0x40 | (0x7 << 3);

const size_t TX_CHUNK = 1024;
// Central AKS can take longer than the local model. A short timeout made an
// otherwise successful recognition look like a missing face on the device.
const uint32_t HTTP_TIMEOUT_MS = 45000;
const uint32_t LINK_WAIT_MS = 3000;

static const char *BOUNDARY = "----ESP32BoundaryAKS7MA4Y";

void phyApply(uint8_t cfg) {
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    W5100.writePHYCFGR_W5500((uint8_t)(cfg & ~0x80));
    SPI.endTransaction();
    delay(2);

    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    W5100.writePHYCFGR_W5500((uint8_t)(cfg | 0x80));
    SPI.endTransaction();
    delay(2);
}

bool addrSane() {
    IPAddress ip = Ethernet.localIP();
    bool allZero = (ip[0] | ip[1] | ip[2] | ip[3]) == 0x00;
    bool allOnes = (ip[0] & ip[1] & ip[2] & ip[3]) == 0xFF;
    return !allZero && !allOnes;
}

void logf_(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

bool readResponse(EthernetClient &c, int &code, String &body,
                  uint32_t timeoutMs = HTTP_TIMEOUT_MS) {
    uint32_t t0 = millis();
    String statusLine;

    while (millis() - t0 < timeoutMs) {
        if (c.available()) {
            statusLine = c.readStringUntil('\n');
            break;
        }
        if (!c.connected() && !c.available()) return false;
        delay(5);
    }
    if (statusLine.length() == 0) return false;

    int sp = statusLine.indexOf(' ');
    code = (sp > 0) ? statusLine.substring(sp + 1, sp + 4).toInt() : 0;

    long contentLength = -1;
    while (millis() - t0 < timeoutMs) {
        if (!c.available()) {
            if (!c.connected()) break;
            delay(2);
            continue;
        }
        String line = c.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
        String low = line;
        low.toLowerCase();
        if (low.startsWith("content-length:")) {
            contentLength = line.substring(15).toInt();
        }
    }

    body = "";
    while (millis() - t0 < timeoutMs) {
        while (c.available()) {
            body += (char)c.read();
            if (contentLength > 0 && (long)body.length() >= contentLength) return true;
        }
        if (!c.connected()) break;
        delay(2);
    }
    return body.length() > 0;
}

bool sendHeaders(EthernetClient &c, const char *path, const char *contentType, size_t contentLength) {
    String h;
    h.reserve(320);
    h += "POST ";  h += path;  h += " HTTP/1.1\r\n";
    h += "Host: ";  h += ETH_SERVER_HOST;  h += ":";  h += String(ETH_SERVER_PORT); h += "\r\n";
    h += "X-API-Key: ";     h += ETH_API_KEY;            h += "\r\n";
    h += "Content-Type: ";  h += contentType;            h += "\r\n";
    h += "Content-Length: "; h += String(contentLength); h += "\r\n";
    h += "Connection: close\r\n\r\n";
    return c.print(h) == h.length();
}

}  // namespace

namespace eth {

bool begin() {
    logf_("[eth] поднимаю W5500...\n");
#if ETH_RST_PIN >= 0
    pinMode(ETH_RST_PIN, OUTPUT);
    digitalWrite(ETH_RST_PIN, LOW);
    delay(20);
    digitalWrite(ETH_RST_PIN, HIGH);
    delay(200);
#endif

    SPI.begin(ETH_SCK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
    Ethernet.init(ETH_CS_PIN);
    Ethernet.setRetransmissionTimeout(400);
    Ethernet.setRetransmissionCount(3);

#if ETH_USE_STATIC
    IPAddress ip ETH_STATIC_IP;
    IPAddress gw ETH_GATEWAY;
    IPAddress sn ETH_SUBNET;
    Ethernet.begin(g_mac, ip, gw, gw, sn);

    if (!hardwarePresent()) {
        logf_("[eth] W5500 не найдена на SPI.\n");
        g_up = false;
        return false;
    }
    logf_("[eth] статический адрес %s\n", localIP().c_str());

    uint32_t t0 = millis();
    bool up = false;
    while (millis() - t0 < LINK_WAIT_MS) {
        if (linkUp()) { up = true; break; }
        delay(50);
    }
    if (!up) {
        logf_("[eth] Линк не поднялся за %lu мс.\n", (unsigned long)LINK_WAIT_MS);
    }
#else
    if (Ethernet.begin(g_mac, 8000) == 0) {
        logf_("[eth] DHCP не ответил.\n");
        g_up = false;
        return false;
    }
    logf_("[eth] получен адрес %s\n", localIP().c_str());
#endif

    g_up = true;
    return true;
}

void reapply() {
#if ETH_USE_STATIC
    IPAddress ip ETH_STATIC_IP;
    IPAddress gw ETH_GATEWAY;
    IPAddress sn ETH_SUBNET;
    Ethernet.begin(g_mac, ip, gw, gw, sn);
#else
    Ethernet.maintain();
#endif
}

void sleep() {
    if (g_asleep) return;
    phyApply(PHY_PWDN);
    g_asleep = true;
}

bool wake() {
    if (!g_asleep) return linkUp();
    uint32_t t0 = millis();
    phyApply(PHY_AUTO);
    g_asleep = false;
    while (millis() - t0 < LINK_WAIT_MS) {
        if (linkUp()) {
            reapply();
            return true;
        }
        delay(50);
    }
    return false;
}

bool asleep() { return g_asleep; }
bool hardwarePresent() { return Ethernet.hardwareStatus() != EthernetNoHardware; }
bool linkUp() { return Ethernet.linkStatus() == LinkON; }
String localIP() {
    IPAddress ip = Ethernet.localIP();
    return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

// ---------------------------------------------------------------- verify ---
VerifyResult verify(const uint8_t *jpeg, size_t len) {
    VerifyResult r;

    if (!hardwarePresent()) {
        r.error = "W5500 не отвечает";
        return r;
    }
    if (!linkUp()) {
        r.error = "кабель не подключён";
        return r;
    }

    EthernetClient c;
    uint32_t t0 = millis();
    if (!c.connect(ETH_SERVER_HOST, ETH_SERVER_PORT)) {
        r.error = "сервер не отвечает";
        logf_("[eth] не удалось подключиться к %s:%d\n", ETH_SERVER_HOST, ETH_SERVER_PORT);
        return r;
    }

    // Собираем multipart/form-data
    String head = String("--") + BOUNDARY + "\r\n" +
                  "Content-Disposition: form-data; name=\"file\"; filename=\"face.jpg\"\r\n" +
                  "Content-Type: image/jpeg\r\n\r\n";
    String tail = String("\r\n--") + BOUNDARY + "--\r\n";

    size_t totalLen = head.length() + len + tail.length();
    String ct = String("multipart/form-data; boundary=") + BOUNDARY;

    if (!sendHeaders(c, "/api/recognize", ct.c_str(), totalLen)) {
        c.stop();
        r.error = "обрыв при отправке заголовков";
        return r;
    }

    // Передаем тело без дублирования буфера в RAM
    c.print(head);

    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > TX_CHUNK) n = TX_CHUNK;
        size_t w = c.write(jpeg + sent, n);
        if (w == 0) {
            c.stop();
            r.error = "обрыв при отправке кадра";
            return r;
        }
        sent += w;
    }

    c.print(tail);
    c.flush();

    String body;
    if (!readResponse(c, r.httpCode, body)) {
        c.stop();
        r.error = "сервер не ответил вовремя";
        return r;
    }
    c.stop();

    if (r.httpCode == 401) {
        r.error = "Ошибка 401 (Unauthorized): неверный X-API-Key";
        r.lcd1 = "Xeta: 401";
        r.lcd2 = "API Key sehvdir";
        return r;
    }
    if (r.httpCode == 403) {
        r.error = "Ошибка 403 (Forbidden): IP не в белом списке";
        r.lcd1 = "Xeta: 403";
        r.lcd2 = "IP qadagandir";
        return r;
    }

    if (r.httpCode != 200) {
        JsonDocument problem;
        String reason;
        if (!deserializeJson(problem, body)) {
            reason = String((const char *)(problem["reason"] | ""));
        }
        r.error = "HTTP " + String(r.httpCode) +
                  (reason.length() ? " " + reason : " от сервера");
        return r;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        r.error = "непонятный ответ сервера (ошибка JSON)";
        return r;
    }

    if (strcmp((const char *)(doc["status"] | ""), "success") != 0) {
        r.error = "сервер не подтвердил распознавание";
        return r;
    }
    r.ok = true;
    JsonArray faces = doc["faces"].as<JsonArray>();
    if (faces.size() > 0) {
        JsonObject f0 = faces[0];
        r.matched = f0["match"] | false;
        r.logId   = f0["log_id"] | 0;
        r.token   = String(r.logId);
        r.expiresIn = f0["expires_in"] | 90;
        r.score = f0["score"] | 0.0f;

        if (r.matched) {
            String fn = f0["user"]["first_name"] | "";
            String ln = f0["user"]["last_name"] | "";
            r.name = fn + " " + ln;
            r.name.trim();

            r.lcd1 = r.name;
            r.lcd2 = LCD_OK_2;
        } else {
            r.reason = String((const char *)(f0["message"] | "Saxta uz!"));
            r.lcd1 = LCD_DENY_1;
            r.lcd2 = r.reason;
        }
    } else {
        r.matched = false;
        r.reason = String((const char *)(doc["message"] | "Uz tapilmadi"));
        r.lcd1 = LCD_DENY_1;
        r.lcd2 = r.reason;
    }

    logf_("[eth] recognize: %s, log_id=%d, %u байт за %lu мс, HTTP %d\n",
          r.matched ? "опознан" : r.reason.c_str(),
          r.logId, (unsigned)len, (unsigned long)(millis() - t0), r.httpCode);
    return r;
}

// ------------------------------------------------------------------ scan ---
ScanResult scan(const String &token, const String &barcode) {
    ScanResult r;

    if (!linkUp()) {
        r.error = "кабель не подключён";
        return r;
    }

    String payload = "";
    payload += "--"; payload += BOUNDARY; payload += "\r\n";
    payload += "Content-Disposition: form-data; name=\"log_id\"\r\n\r\n";
    payload += token; payload += "\r\n";

    payload += "--"; payload += BOUNDARY; payload += "\r\n";
    payload += "Content-Disposition: form-data; name=\"cargo_barcode\"\r\n\r\n";
    payload += barcode; payload += "\r\n";

    payload += "--"; payload += BOUNDARY; payload += "--\r\n";

    String ct = String("multipart/form-data; boundary=") + BOUNDARY;

    EthernetClient c;
    if (!c.connect(ETH_SERVER_HOST, ETH_SERVER_PORT)) {
        r.error = "сервер не отвечает";
        return r;
    }
    if (!sendHeaders(c, "/api/log_cargo", ct.c_str(), payload.length())) {
        c.stop();
        r.error = "обрыв при отправке";
        return r;
    }
    c.print(payload);
    c.flush();

    String body;
    if (!readResponse(c, r.httpCode, body)) {
        c.stop();
        r.error = "сервер не ответил вовремя";
        return r;
    }
    c.stop();

    if (r.httpCode == 401 || r.httpCode == 403) {
        r.error = "Ошибка авторизации HTTP " + String(r.httpCode);
        r.lcd1 = "Xeta: " + String(r.httpCode);
        r.lcd2 = "Girish qadagan";
        return r;
    }

    if (r.httpCode != 200) {
        JsonDocument problem;
        String reason;
        if (!deserializeJson(problem, body)) {
            reason = String((const char *)(problem["reason"] | ""));
        }
        r.error = "HTTP " + String(r.httpCode) +
                  (reason.length() ? " " + reason : " от сервера");
        return r;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        r.error = "непонятный ответ сервера (ошибка JSON)";
        return r;
    }

    const char *st = doc["status"] | "";
    r.ok = (strcmp(st, "success") == 0);
    r.recordId = doc["record_id"] | token.toInt();
    r.reason = String((const char *)(doc["message"] | ""));

    if (r.ok) {
        r.lcd1 = LCD_SAVED_1;
        r.lcd2 = barcode;
    } else {
        r.lcd1 = LCD_ERR_1;
        r.lcd2 = r.reason.length() ? r.reason : "Qeyd olunmadi";
    }

    logf_("[eth] log_cargo: %s, HTTP %d\n", r.ok ? "успешно" : r.reason.c_str(), r.httpCode);
    return r;
}

// ---------------------------------------------------------------- health ---
bool health(String &out) {
    EthernetClient c;
    if (!c.connect(ETH_SERVER_HOST, ETH_SERVER_PORT)) {
        out = "сервер не отвечает (порт " + String(ETH_SERVER_PORT) + " закрыт или хост недоступен)";
        return false;
    }
    c.stop();
    out = "хост " + String(ETH_SERVER_HOST) + ":" + String(ETH_SERVER_PORT) + " доступен (TCP OK)";
    return true;
}

bool takeRemoteTrigger() {
    if (!g_up || g_asleep || !linkUp()) return false;

    EthernetClient c;
    c.setTimeout(1000);
    if (!c.connect(ETH_SERVER_HOST, ETH_SERVER_PORT)) return false;
    String request = "GET /api/device/trigger HTTP/1.1\r\nHost: ";
    request += ETH_SERVER_HOST;
    request += ":";
    request += String(ETH_SERVER_PORT);
    request += "\r\nX-API-Key: ";
    request += ETH_API_KEY;
    request += "\r\nConnection: close\r\n\r\n";
    if (c.print(request) != request.length()) {
        c.stop();
        return false;
    }
    int status = 0;
    String body;
    bool received = readResponse(c, status, body, 2000);
    c.stop();
    if (!received || status != 200) return false;
    JsonDocument payload;
    if (deserializeJson(payload, body)) return false;
    return payload["check"] == true;
}

#if REGISTRY_ENABLED
static bool g_registryRegistered = false;
struct RegistryEvent {
    const char *level;
    const char *message;
    uint32_t uptimeMs;
};
static RegistryEvent g_registryEvents[8];
static uint8_t g_registryEventCount = 0;

void registryLog(const char *level, const char *message) {
    if (g_registryEventCount == 8) {
        for (uint8_t i = 1; i < 8; ++i) g_registryEvents[i - 1] = g_registryEvents[i];
        --g_registryEventCount;
    }
    g_registryEvents[g_registryEventCount++] = {level, message, millis()};
}

static int registryPost(const String &path, const String &json) {
    EthernetClient c;
    if (!c.connect(REGISTRY_HOST, REGISTRY_PORT)) return 0;
    String headers = "POST " + path + " HTTP/1.1\r\nHost: " REGISTRY_HOST;
    headers += ":" + String(REGISTRY_PORT) + "\r\nX-API-Key: " REGISTRY_API_KEY;
    headers += "\r\nContent-Type: application/json\r\nContent-Length: ";
    headers += String(json.length()) + "\r\nConnection: close\r\n\r\n";
    if (c.print(headers) != headers.length() || c.print(json) != json.length()) {
        c.stop();
        return 0;
    }
    int code = 0;
    String body;
    const bool got = readResponse(c, code, body, 1000);
    c.stop();
    return got ? code : 0;
}

static void registryHeartbeat() {
    if (!g_registryRegistered) {
        JsonDocument registration;
        registration["device_id"] = CHECKPOINT_DEVICE_ID;
        registration["region"] = REGISTRY_REGION;
        registration["name"] = REGISTRY_NAME;
        String json;
        serializeJson(registration, json);
        g_registryRegistered = registryPost("/api/v1/devices/register", json) == 200;
        if (!g_registryRegistered) return;
        registryLog("info", "ESP32-S3 registered after boot");
    }

    JsonDocument heartbeatBody;
    heartbeatBody["ip"] = localIP();
    heartbeatBody["fw_version"] = REGISTRY_FW_VERSION;
    String json;
    serializeJson(heartbeatBody, json);
    const int code = registryPost(String("/api/v1/devices/") +
                                  CHECKPOINT_DEVICE_ID + "/heartbeat", json);
    if (code == 404) g_registryRegistered = false;
    if (code != 200 || g_registryEventCount == 0) return;

    JsonDocument logs;
    JsonArray events = logs["logs"].to<JsonArray>();
    for (uint8_t i = 0; i < g_registryEventCount; ++i) {
        JsonObject event = events.add<JsonObject>();
        event["level"] = g_registryEvents[i].level;
        event["message"] = g_registryEvents[i].message;
        event["uptime_ms"] = g_registryEvents[i].uptimeMs;
    }
    json = "";
    serializeJson(logs, json);
    if (registryPost(String("/api/v1/devices/") + CHECKPOINT_DEVICE_ID +
                     "/logs", json) == 200) {
        g_registryEventCount = 0;
    }
}
#else
void registryLog(const char *, const char *) {}
#endif

bool heartbeat() {
    if (!linkUp()) return false;
    EthernetClient c;
    bool mainOk = false;
    if (c.connect(ETH_SERVER_HOST, ETH_SERVER_PORT)) {
        if (sendHeaders(c, "/api/esp/heartbeat", "application/json", 0)) {
            int code = 0;
            String body;
            const bool got = readResponse(c, code, body, 1000);
            mainOk = got && code == 200;
        }
        c.stop();
    }
#if REGISTRY_ENABLED
    static uint32_t lastRegistryMs = 0;
    if (!lastRegistryMs || millis() - lastRegistryMs >= 30000) {
        lastRegistryMs = millis();
        registryHeartbeat();
    }
#endif
    return mainOk;
}

}  // namespace eth

#endif  // ENABLE_ETH
