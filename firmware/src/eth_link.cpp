#include "eth_link.h"
#include "config.h"

#if ENABLE_ETH

#include <ArduinoJson.h>
#include <Ethernet.h>
#include <Dns.h>
#if ETH_SERVER_TLS
#include "w5500_tls_client.h"
#endif
#include <SPI.h>
#include <stdarg.h>
#include <utility/w5100.h>
#include "http_response.h"

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
static const char *API_SCHEME = ETH_SERVER_TLS ? "https" : "http";

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

String safeDiagnostic(String text, size_t limit = 768) {
    // An upstream error can echo request headers. Never print the API key.
    const String key = ETH_API_KEY;
    if (key.length()) text.replace(key, "[REDACTED]");
    for (size_t i = 0; i < text.length(); ++i) {
        const uint8_t ch = static_cast<uint8_t>(text[i]);
        if (ch < 0x20 || ch == 0x7f) text.setCharAt(i, ' ');
    }
    if (text.length() > limit) text = text.substring(0, limit) + " [truncated]";
    return text;
}

String responseReason(const String &body) {
    JsonDocument problem;
    if (deserializeJson(problem, body)) return "";
    const char *fields[] = {"detail", "message", "reason", "error"};
    for (const char *field : fields) {
        JsonVariantConst value = problem[field];
        if (value.is<const char *>()) return safeDiagnostic(value.as<const char *>(), 240);
        if (value["message"].is<const char *>())
            return safeDiagnostic(value["message"].as<const char *>(), 240);
    }
    return "";
}

String httpError(int code, const String &body) {
    if (code >= 300 && code < 400)
        return "HTTP " + String(code) + ": перенаправление; проверьте HTTPS и адрес AKS";
    const String reason = responseReason(body);
    return "HTTP " + String(code) +
           (reason.length() ? ": " + reason : ": сервер отклонил запрос; см. [eth] HTTP body");
}

void recognitionError(eth::VerifyResult &result, const String &body, const String &reason) {
    result.error = reason;
    logf_("[eth] HTTP POST /api/recognize -> %d; validation=%s\n",
          result.httpCode, safeDiagnostic(reason, 240).c_str());
    Serial.print("[eth] HTTP body: ");
    Serial.println(body.length() ? safeDiagnostic(body) : "[empty]");
}

// Own the TCP transport for at least as long as its TLS wrapper. Local laptop
// calls still use plain Ethernet; only the AKS connection is wrapped in TLS.
struct ApiConnection {
    EthernetClient tcp;
#if ETH_SERVER_TLS
    W5500TlsClient tls{tcp};
    Client &client = tls;
#else
    Client &client = tcp;
#endif
    String error;

    ApiConnection() {
        tcp.setConnectionTimeout(5000);
    }
    ~ApiConnection() { client.stop(); }
    ApiConnection(const ApiConnection &) = delete;
    ApiConnection &operator=(const ApiConnection &) = delete;

    bool connect() {
        if (!eth::hardwarePresent() || !eth::linkUp() || !addrSane()) {
            error = "Ethernet недоступен: проверьте W5500, кабель и IP";
            return false;
        }
        IPAddress resolved;
        if (!resolved.fromString(ETH_SERVER_HOST)) {
            DNSClient dns;
            dns.begin(Ethernet.dnsServerIP());
            if (dns.getHostByName(ETH_SERVER_HOST, resolved, 3000) != 1) {
                error = "DNS не разрешает " + String(ETH_SERVER_HOST) +
                        "; проверьте ETH_DNS (" + Ethernet.dnsServerIP().toString() + ")";
                logf_("[eth] %s\n", error.c_str());
                return false;
            }
        }
        logf_("[eth] соединение %s://%s:%d, IP=%s\n", API_SCHEME,
              ETH_SERVER_HOST, ETH_SERVER_PORT, resolved.toString().c_str());
        // Use the hostname for SNI even when DNS has already been checked.
        const bool connected =
#if ETH_SERVER_TLS
            tls.connect(ETH_SERVER_HOST, resolved, ETH_SERVER_PORT);
#else
            tcp.connect(resolved, ETH_SERVER_PORT);
#endif
        if (!connected) {
            error = String(ETH_SERVER_TLS ? "не удалось установить TCP/TLS с " :
                                          "не удалось установить TCP с ") +
                    ETH_SERVER_HOST + ":" + String(ETH_SERVER_PORT);
#if ETH_SERVER_TLS
            error += "; mbedTLS error=" + String(tls.lastError()) + " " + tls.errorText();
#endif
            logf_("[eth] %s\n", error.c_str());
            client.stop();
            return false;
        }
#if ETH_SERVER_TLS
        Serial.println("[eth] TLS engine: ESP32 mbedTLS, TLS 1.2");
        // Never send a key or frame before the TLS handshake succeeds.
        if (!tls.isSecure()) {
            error = "TLS не включён для выбранного порта; AKS требует 443";
            client.stop();
            return false;
        }
#endif
        return true;
    }
};

bool readResponse(Client &c, int &code, String &body,
                  uint32_t timeoutMs = HTTP_TIMEOUT_MS, const char *path = nullptr) {
    struct Reader {
        Client &client;
        uint32_t started;
        uint32_t timeout;
        int read() {
            while (millis() - started < timeout) {
                if (client.available()) return client.read();
                if (!client.connected()) return -1;
                delay(2);
            }
            return -2;
        }
    } reader{c, millis(), timeoutMs};
    struct Sink {
        String &text;
        bool append(char value) { return text.concat(value); }
    } sink{body};

    body = "";
    code = 0;
    http_response::Info info;
    const bool complete = http_response::read(reader, sink, info);
    code = info.code;
    if (path && (!complete || code >= 300)) {
        logf_("[eth] HTTP POST %s -> %d; source=%s; destination=%s:%d\n",
              path, code, eth::localIP().c_str(), ETH_SERVER_HOST, ETH_SERVER_PORT);
        logf_("[eth] HTTP Server: %s; Content-Type: %s\n",
              safeDiagnostic(info.server, 80).c_str(),
              safeDiagnostic(info.contentType, 80).c_str());
        Serial.print("[eth] HTTP body: ");
        Serial.println(body.length() ? safeDiagnostic(body) : "[empty]");
        if (!complete) logf_("[eth] HTTP receive error: %s\n", info.error ? info.error : "unknown");
    }
    return complete;
}

bool writeAll(Client &c, const uint8_t *data, size_t len) {
    const uint32_t started = millis();
    size_t sent = 0;
    while (sent < len && millis() - started < HTTP_TIMEOUT_MS) {
        const size_t remaining = len - sent;
        const size_t count = remaining > TX_CHUNK ? TX_CHUNK : remaining;
        const size_t written = c.write(data + sent, count);
        if (!written) return false;
        sent += written;
    }
    return sent == len;
}

bool writeAll(Client &c, const String &text) {
    return writeAll(c, reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
}

bool sendHeaders(Client &c, const char *path, const char *contentType, size_t contentLength) {
    String h;
    h.reserve(320);
    h += "POST ";  h += path;  h += " HTTP/1.1\r\n";
    h += "Host: ";  h += ETH_SERVER_HOST;  h += ":";  h += String(ETH_SERVER_PORT); h += "\r\n";
    h += "X-API-Key: ";     h += ETH_API_KEY;            h += "\r\n";
    h += "Content-Type: ";  h += contentType;            h += "\r\n";
    h += "Content-Length: "; h += String(contentLength); h += "\r\n";
    h += "Connection: close\r\n\r\n";
    return writeAll(c, h);
}

}  // namespace

namespace eth {

bool begin() {
    logf_("[eth] поднимаю W5500...\n");
#ifdef ETH_SPI_CLOCK_HZ
    logf_("[eth] SPI clock: %u Hz\n", (unsigned)ETH_SPI_CLOCK_HZ);
#endif
    logf_("[eth] режим: %s, API: %s://%s:%d/api\n",
          ETH_DIRECT_AKS ? "прямой AKS" : "сервер ноутбука",
          API_SCHEME, ETH_SERVER_HOST, ETH_SERVER_PORT);
#if ETH_SERVER_TLS
    logf_("[eth] TLS: %s\n", ETH_TLS_INSECURE ?
          "режим разработки, проверка сертификата отключена" : "проверка fingerprint");
#endif
    logf_("[eth] MAC W5500: %02X:%02X:%02X:%02X:%02X:%02X\n",
          g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
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
    IPAddress dns ETH_DNS;
    for (uint8_t i = 0; i < 4; ++i) {
        if ((ip[i] & sn[i]) != (gw[i] & sn[i])) {
            logf_("[eth] ошибка адресации: шлюз %s вне подсети %s / %s\n",
                  gw.toString().c_str(), ip.toString().c_str(), sn.toString().c_str());
            g_up = false;
            return false;
        }
    }
    Ethernet.begin(g_mac, ip, dns, gw, sn);

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
        logf_("[eth] DHCP не выдал адрес. Проверьте сеть или задайте "
              "выданные администратором IP, маску и шлюз в config.h.\n");
        g_up = false;
        return false;
    }
    logf_("[eth] получен адрес %s\n", localIP().c_str());
#endif

    logf_("[eth] маска %s, шлюз %s, DNS %s\n",
          Ethernet.subnetMask().toString().c_str(),
          Ethernet.gatewayIP().toString().c_str(),
          Ethernet.dnsServerIP().toString().c_str());

    g_up = true;
    return true;
}

void reapply() {
#if ETH_USE_STATIC
    IPAddress ip ETH_STATIC_IP;
    IPAddress gw ETH_GATEWAY;
    IPAddress sn ETH_SUBNET;
    IPAddress dns ETH_DNS;
    Ethernet.begin(g_mac, ip, dns, gw, sn);
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

    ApiConnection connection;
    Client &c = connection.client;
    uint32_t t0 = millis();
    if (!connection.connect()) {
        r.error = connection.error;
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
    if (!writeAll(c, head) || !writeAll(c, jpeg, len) || !writeAll(c, tail)) {
        c.stop();
        r.error = "обрыв при отправке multipart кадра";
        return r;
    }
    // The TLS write is synchronous; leave any fast reply in its RX buffer.

    String body;
    if (!readResponse(c, r.httpCode, body, HTTP_TIMEOUT_MS, "/api/recognize")) {
        c.stop();
        r.error = r.httpCode ? "HTTP " + String(r.httpCode) + ": неполный или неверный ответ сервера" :
                              "сервер не ответил вовремя";
        return r;
    }
    c.stop();

    if (r.httpCode == 401) {
        r.error = httpError(r.httpCode, body);
        r.lcd1 = "Xeta: 401";
        r.lcd2 = "API Key sehvdir";
        return r;
    }
    if (r.httpCode == 403) {
        r.error = httpError(r.httpCode, body);
        r.lcd1 = "Xeta: 403";
        r.lcd2 = "Server redd etdi";
        return r;
    }

    if (r.httpCode != 200) {
        r.error = httpError(r.httpCode, body);
        return r;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        recognitionError(r, body, "непонятный ответ сервера (ошибка JSON)");
        return r;
    }

    if (strcmp((const char *)(doc["status"] | ""), "success") != 0) {
        recognitionError(r, body, "сервер не подтвердил распознавание: status != success");
        return r;
    }
    // HTTP 200 can still be a denial. Keep the actual upstream verdict
    // visible instead of diagnosing it from our fallback LCD text.
    Serial.print("[eth] AKS response: ");
    Serial.println(safeDiagnostic(body));
    JsonArray faces = doc["faces"].as<JsonArray>();
    // The live AKS returns HTTP 200 with faces:[] when no face is detected.
    // Accept that denial, but never accept multiple faces or a malformed match.
    if (faces.isNull()) {
        recognitionError(r, body, "неверный ответ распознавания: faces отсутствует или не массив");
        return r;
    }
    if (faces.size() > 1) {
        recognitionError(r, body, "сервер вернул несколько лиц: " + String(faces.size()) +
                         "; для прохода требуется одно лицо");
        return r;
    }
    if (faces.size() == 1 && !faces[0].is<JsonObject>()) {
        recognitionError(r, body, "неверный ответ распознавания: faces[0] не объект");
        return r;
    }
    if (faces.size() == 1 && !faces[0]["match"].is<bool>()) {
        recognitionError(r, body, "неверный ответ распознавания: match отсутствует или не true/false");
        return r;
    }
    if (faces[0]["match"].as<bool>() &&
        (!faces[0]["log_id"].is<int>() || faces[0]["log_id"].as<int>() <= 0)) {
        recognitionError(r, body, "сервер не выдал действительный log_id");
        return r;
    }
    logf_("[eth] AKS faces_count=%u\n", (unsigned)faces.size());
    if (faces[0]["match"].as<bool>()) {
        JsonVariantConst user = faces[0]["user"];
        if (!user["first_name"].is<const char *>() ||
            !user["last_name"].is<const char *>()) {
            recognitionError(r, body, "AKS не вернул имя и фамилию сотрудника");
            return r;
        }
        String firstName = user["first_name"].as<const char *>();
        String lastName = user["last_name"].as<const char *>();
        firstName.trim();
        lastName.trim();
        if (!firstName.length() || !lastName.length()) {
            recognitionError(r, body, "AKS вернул пустое имя или фамилию сотрудника");
            return r;
        }
    }
    r.ok = true;
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

    ApiConnection connection;
    Client &c = connection.client;
    if (!connection.connect()) {
        r.error = connection.error;
        return r;
    }
    if (!sendHeaders(c, "/api/log_cargo", ct.c_str(), payload.length())) {
        c.stop();
        r.error = "обрыв при отправке";
        return r;
    }
    if (!writeAll(c, payload)) {
        c.stop();
        r.error = "обрыв при отправке multipart груза";
        return r;
    }

    String body;
    if (!readResponse(c, r.httpCode, body, HTTP_TIMEOUT_MS, "/api/log_cargo")) {
        c.stop();
        r.error = r.httpCode ? "HTTP " + String(r.httpCode) + ": неполный или неверный ответ сервера" :
                              "сервер не ответил вовремя";
        return r;
    }
    c.stop();

    if (r.httpCode == 401 || r.httpCode == 403) {
        r.error = httpError(r.httpCode, body);
        r.lcd1 = "Xeta: " + String(r.httpCode);
        r.lcd2 = "Girish qadagan";
        return r;
    }

    if (r.httpCode != 200) {
        r.error = httpError(r.httpCode, body);
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
bool health(String &out, bool httpProbe) {
    ApiConnection connection;
    if (!connection.connect()) {
        out = connection.error;
        return false;
    }
    out = "хост " + String(ETH_SERVER_HOST) + ":" + String(ETH_SERVER_PORT) +
          (ETH_SERVER_TLS ? " доступен (TLS OK; авторизация ещё не проверена)" : " доступен (TCP OK)");
    if (httpProbe) {
        // Read-only round trip exercises encrypted writes AND reads. No
        // API key, biometric image, recognition POST or cargo record involved.
        const String request = String("GET /api/recognize HTTP/1.1\r\nHost: ") +
            ETH_SERVER_HOST + "\r\nConnection: close\r\n\r\n";
        if (!writeAll(connection.client, request)) {
            out = "TLS HTTP probe: write failed";
            return false;
        }
        int code = 0;
        String body;
        if (!readResponse(connection.client, code, body, 8000) || code < 100) {
            out = "TLS HTTP probe: invalid or incomplete response";
            return false;
        }
        out += "; read-only probe HTTP " + String(code);
    }
    return true;
}

bool takeRemoteTrigger() {
#if ETH_DIRECT_AKS
    // This laptop-only endpoint is absent from the central AKS contract.
    return false;
#else
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
#endif
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
#if !ETH_USE_STATIC
    if (g_up && !g_asleep) Ethernet.maintain();
#endif
#if ETH_DIRECT_AKS
    // Maintain the DHCP lease without calling undocumented AKS endpoints.
    return g_up && !g_asleep && addrSane();
#else
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
#endif
}

}  // namespace eth

#endif  // ENABLE_ETH
