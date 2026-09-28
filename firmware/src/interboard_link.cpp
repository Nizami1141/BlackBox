#include "interboard_link.h"

#include "config.h"

#if ENABLE_INTERBOARD

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace {

constexpr uint32_t MAGIC = 0x32425043;  // "CPB2"
constexpr uint8_t VERSION = 1;
constexpr size_t PAYLOAD_SIZE = 200;
constexpr uint16_t ACK_BEGIN = 0xFFFF;
constexpr uint16_t ACK_END = 0xFFFE;
constexpr uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum class Type : uint8_t {
    HELLO = 1,
    HELLO_ACK,
    CAPTURE_REQUEST,
    FRAME_BEGIN,
    FRAME_DATA,
    ACK,
    FRAME_END,
    ERROR_MESSAGE,
};

struct __attribute__((packed)) Packet {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t session;
    uint16_t seq;
    uint16_t payloadLen;
    uint32_t totalLen;
    uint32_t crc32;
    uint16_t width;
    uint16_t height;
    uint8_t payload[PAYLOAD_SIZE];
};

static_assert(sizeof(Packet) <= 250, "ESP-NOW packet exceeds 250-byte limit");

interboard::Role g_role = interboard::Role::CAMERA_NODE;
uint8_t g_peer[6] = {};
volatile bool g_paired = false;
uint32_t g_lastHello = 0;

volatile bool g_capturePending = false;
volatile uint16_t g_captureSession = 0;
volatile bool g_txBusy = false;

volatile uint16_t g_ackSession = 0;
volatile uint16_t g_ackSeq = 0;

uint8_t *g_rxBuffer = nullptr;
size_t g_rxCapacity = 0;
volatile size_t g_rxWritten = 0;
volatile uint16_t g_rxExpectedSeq = 0;
volatile uint16_t g_rxSession = 0;
volatile uint32_t g_rxExpectedCrc = 0;
volatile uint16_t g_rxWidth = 0;
volatile uint16_t g_rxHeight = 0;
volatile bool g_rxActive = false;
volatile bool g_rxComplete = false;
volatile bool g_rxFailed = false;
volatile uint16_t g_lastCompletedSession = 0;
char g_rxError[96] = {};
uint16_t g_nextSession = 1;

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

String macText(const uint8_t *mac) {
    char out[18];
    snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(out);
}

bool sameMac(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

bool ensurePeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = INTERBOARD_WIFI_CHANNEL;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
}

bool sendPacket(const uint8_t *to, Type type, uint16_t session,
                uint16_t seq = 0, const uint8_t *payload = nullptr,
                uint16_t payloadLen = 0, uint32_t totalLen = 0,
                uint32_t crc = 0, uint16_t width = 0, uint16_t height = 0) {
    if (payloadLen > PAYLOAD_SIZE) return false;
    Packet p = {};
    p.magic = MAGIC;
    p.version = VERSION;
    p.type = static_cast<uint8_t>(type);
    p.session = session;
    p.seq = seq;
    p.payloadLen = payloadLen;
    p.totalLen = totalLen;
    p.crc32 = crc;
    p.width = width;
    p.height = height;
    if (payload && payloadLen) memcpy(p.payload, payload, payloadLen);
    return esp_now_send(to, reinterpret_cast<const uint8_t *>(&p), sizeof(p)) == ESP_OK;
}

void setRxError(const char *message) {
    strncpy(g_rxError, message, sizeof(g_rxError) - 1);
    g_rxError[sizeof(g_rxError) - 1] = '\0';
    g_rxFailed = true;
    g_rxActive = false;
}

void learnPeer(const uint8_t *mac) {
    if (!ensurePeer(mac)) return;
    if (!g_paired || !sameMac(g_peer, mac)) {
        memcpy(g_peer, mac, 6);
        g_paired = true;
        Serial.printf("[board-link] найдена вторая плата: %s\n", macText(mac).c_str());
    }
}

void sendAck(const uint8_t *to, uint16_t session, uint16_t seq) {
    sendPacket(to, Type::ACK, session, seq);
}

void onReceive(const uint8_t *mac, const uint8_t *raw, int len) {
    if (len != static_cast<int>(sizeof(Packet))) return;
    const Packet &p = *reinterpret_cast<const Packet *>(raw);
    if (p.magic != MAGIC || p.version != VERSION || p.payloadLen > PAYLOAD_SIZE) return;

    Type type = static_cast<Type>(p.type);
    if (type == Type::HELLO) {
        learnPeer(mac);
        sendPacket(mac, Type::HELLO_ACK, 0);
        return;
    }
    if (type == Type::HELLO_ACK) {
        learnPeer(mac);
        return;
    }

    if (!g_paired || !sameMac(mac, g_peer)) return;

    if (type == Type::ACK) {
        g_ackSession = p.session;
        g_ackSeq = p.seq;
        return;
    }

    if (g_role == interboard::Role::CAMERA_NODE) {
        // Повтор того же запроса приходит, пока камера просыпается и снимает.
        // Не ставим его в очередь второй раз: одно нажатие = ровно один кадр.
        if (type == Type::CAPTURE_REQUEST && !g_txBusy &&
            !g_capturePending && p.session != g_captureSession) {
            g_captureSession = p.session;
            g_capturePending = true;
        }
        return;
    }

    if (type == Type::ERROR_MESSAGE) {
        size_t n = p.payloadLen;
        if (n >= sizeof(g_rxError)) n = sizeof(g_rxError) - 1;
        memcpy(g_rxError, p.payload, n);
        g_rxError[n] = '\0';
        if (n == 0) strcpy(g_rxError, "ошибка камеры ESP32-S3");
        g_rxFailed = true;
        g_rxActive = false;
        return;
    }

    // FRAME_END мог дойти, а его ACK — нет. Отправитель законно повторит
    // конец кадра; отвечаем снова, не трогая уже проверенный буфер.
    if (type == Type::FRAME_END && p.session == g_lastCompletedSession) {
        sendAck(mac, p.session, ACK_END);
        return;
    }

    if (type == Type::FRAME_BEGIN) {
        if (p.totalLen == 0 || p.totalLen > INTERBOARD_MAX_JPEG) {
            setRxError("размер JPEG превышает лимит WROOM");
            sendPacket(mac, Type::ERROR_MESSAGE, p.session);
            return;
        }
        if (g_rxBuffer) {
            free(g_rxBuffer);
            g_rxBuffer = nullptr;
        }
        g_rxBuffer = static_cast<uint8_t *>(malloc(p.totalLen));
        if (!g_rxBuffer) {
            setRxError("WROOM не смогла выделить память для JPEG");
            sendPacket(mac, Type::ERROR_MESSAGE, p.session);
            return;
        }
        g_rxCapacity = p.totalLen;
        g_rxWritten = 0;
        g_rxExpectedSeq = 0;
        g_rxSession = p.session;
        g_rxExpectedCrc = p.crc32;
        g_rxWidth = p.width;
        g_rxHeight = p.height;
        g_rxFailed = false;
        g_rxComplete = false;
        g_rxActive = true;
        sendAck(mac, p.session, ACK_BEGIN);
        return;
    }

    if (!g_rxActive || p.session != g_rxSession) return;

    if (type == Type::FRAME_DATA) {
        if (p.seq == g_rxExpectedSeq) {
            if (g_rxWritten + p.payloadLen > g_rxCapacity) {
                setRxError("переполнение буфера JPEG");
                return;
            }
            memcpy(g_rxBuffer + g_rxWritten, p.payload, p.payloadLen);
            g_rxWritten += p.payloadLen;
            g_rxExpectedSeq++;
        }
        // Повторный пакет подтверждаем снова: отправитель мог не получить ACK.
        if (p.seq < g_rxExpectedSeq) sendAck(mac, p.session, p.seq);
        return;
    }

    if (type == Type::FRAME_END) {
        if (g_rxWritten != g_rxCapacity) {
            setRxError("кадр пришёл не полностью");
            return;
        }
        if (crc32(g_rxBuffer, g_rxCapacity) != g_rxExpectedCrc) {
            setRxError("CRC32 кадра не совпала");
            return;
        }
        g_rxActive = false;
        g_rxComplete = true;
        g_lastCompletedSession = p.session;
        sendAck(mac, p.session, ACK_END);
    }
}

bool sendWithAck(Type type, uint16_t session, uint16_t seq,
                 const uint8_t *payload = nullptr, uint16_t payloadLen = 0,
                 uint32_t totalLen = 0, uint32_t crc = 0,
                 uint16_t width = 0, uint16_t height = 0,
                 uint16_t wantedAck = 0) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        g_ackSession = 0;
        g_ackSeq = static_cast<uint16_t>(wantedAck ^ 0x8000U);
        if (!sendPacket(g_peer, type, session, seq, payload, payloadLen,
                        totalLen, crc, width, height)) {
            delay(20);
            continue;
        }
        uint32_t started = millis();
        while (millis() - started < 350) {
            if (g_ackSession == session && g_ackSeq == wantedAck) return true;
            delay(1);
        }
    }
    return false;
}

}  // namespace

namespace interboard {

bool begin(Role role) {
    g_role = role;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    delay(50);
    if (esp_wifi_set_channel(INTERBOARD_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.println("[board-link] не удалось установить радиоканал ESP-NOW");
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        Serial.println("[board-link] esp_now_init завершился ошибкой");
        return false;
    }
    esp_now_register_recv_cb(onReceive);
    if (!ensurePeer(BROADCAST)) {
        Serial.println("[board-link] не удалось добавить broadcast peer");
        return false;
    }
    Serial.printf("[board-link] ESP-NOW готов, роль=%s, канал=%d, MAC=%s\n",
                  role == Role::CAMERA_NODE ? "CAMERA" : "GATEWAY",
                  INTERBOARD_WIFI_CHANNEL, WiFi.macAddress().c_str());
    return true;
}

void loop() {
    if (!g_paired && millis() - g_lastHello >= 1000) {
        g_lastHello = millis();
        sendPacket(BROADCAST, Type::HELLO, 0);
    }
}

bool paired() { return g_paired; }

String peerMac() { return g_paired ? macText(g_peer) : String("--"); }

bool takeCaptureRequest(uint16_t &session) {
    if (!g_capturePending || g_txBusy) return false;
    session = g_captureSession;
    g_capturePending = false;
    return true;
}

bool sendFrame(uint16_t session, const uint8_t *jpeg, size_t len,
               uint16_t width, uint16_t height) {
    if (!g_paired || !jpeg || len == 0 || len > INTERBOARD_MAX_JPEG) return false;
    g_txBusy = true;
    const uint32_t crc = crc32(jpeg, len);

    if (!sendWithAck(Type::FRAME_BEGIN, session, 0, nullptr, 0,
                     len, crc, width, height, ACK_BEGIN)) {
        Serial.println("[board-link] шлюз не подтвердил начало кадра");
        g_txBusy = false;
        return false;
    }

    size_t offset = 0;
    uint16_t seq = 0;
    while (offset < len) {
        uint16_t count = static_cast<uint16_t>(len - offset);
        if (count > PAYLOAD_SIZE) count = PAYLOAD_SIZE;
        if (!sendWithAck(Type::FRAME_DATA, session, seq, jpeg + offset,
                         count, 0, 0, 0, 0, seq)) {
            Serial.printf("[board-link] пакет %u не подтверждён\n", seq);
            g_txBusy = false;
            return false;
        }
        offset += count;
        seq++;
    }

    bool ok = sendWithAck(Type::FRAME_END, session, seq, nullptr, 0,
                          len, crc, width, height, ACK_END);
    g_txBusy = false;
    Serial.printf("[board-link] кадр %u байт: %s (%u пакетов)\n",
                  static_cast<unsigned>(len), ok ? "передан" : "нет финального ACK", seq);
    return ok;
}

void sendCaptureError(uint16_t session, const String &message) {
    if (!g_paired) return;
    uint16_t n = static_cast<uint16_t>(message.length());
    if (n > PAYLOAD_SIZE) n = PAYLOAD_SIZE;
    sendPacket(g_peer, Type::ERROR_MESSAGE, session, 0,
               reinterpret_cast<const uint8_t *>(message.c_str()), n);
}

bool requestFrame(uint8_t **jpeg, size_t *len, uint16_t *width,
                  uint16_t *height, String &error) {
    if (!jpeg || !len || !width || !height) return false;
    *jpeg = nullptr;
    *len = 0;
    *width = *height = 0;

    uint32_t pairStarted = millis();
    while (!g_paired && millis() - pairStarted < 5000) {
        loop();
        delay(10);
    }
    if (!g_paired) {
        error = "вторая ESP32 не найдена по ESP-NOW";
        return false;
    }

    if (g_rxBuffer) {
        free(g_rxBuffer);
        g_rxBuffer = nullptr;
    }
    g_rxCapacity = 0;
    g_rxWritten = 0;
    g_rxActive = false;
    g_rxComplete = false;
    g_rxFailed = false;
    g_rxError[0] = '\0';

    uint16_t session = ++g_nextSession;
    if (session == 0) session = ++g_nextSession;
    uint32_t started = millis();
    uint32_t lastRequest = 0;

    while (millis() - started < INTERBOARD_FRAME_TIMEOUT_MS) {
        if (!g_rxActive && !g_rxComplete && !g_rxFailed &&
            millis() - lastRequest >= 900) {
            lastRequest = millis();
            sendPacket(g_peer, Type::CAPTURE_REQUEST, session);
        }
        if (g_rxFailed) {
            error = g_rxError;
            if (g_rxBuffer) { free(g_rxBuffer); g_rxBuffer = nullptr; }
            return false;
        }
        if (g_rxComplete) {
            *jpeg = g_rxBuffer;
            *len = g_rxCapacity;
            *width = g_rxWidth;
            *height = g_rxHeight;
            g_rxBuffer = nullptr;
            g_rxCapacity = 0;
            g_rxComplete = false;
            return true;
        }
        delay(2);
    }

    if (g_rxBuffer) { free(g_rxBuffer); g_rxBuffer = nullptr; }
    g_rxActive = false;
    error = "таймаут ожидания кадра от ESP32-S3";
    return false;
}

}  // namespace interboard

#else

namespace interboard {
bool begin(Role) { return false; }
void loop() {}
bool paired() { return false; }
String peerMac() { return "--"; }
bool takeCaptureRequest(uint16_t &) { return false; }
bool sendFrame(uint16_t, const uint8_t *, size_t, uint16_t, uint16_t) { return false; }
void sendCaptureError(uint16_t, const String &) {}
bool requestFrame(uint8_t **, size_t *, uint16_t *, uint16_t *, String &) { return false; }
}  // namespace interboard

#endif
