#include "ble_photo.h"

#include "config.h"          // ENABLE_BLE объявлен здесь — только после этого #if

#if ENABLE_BLE

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "protocol.h"

namespace {

BLEServer         *g_server   = nullptr;
BLECharacteristic *g_chrData  = nullptr;
BLECharacteristic *g_chrCtrl  = nullptr;
BLECharacteristic *g_chrStat  = nullptr;

volatile bool     g_connected     = false;
volatile bool     g_photoEnabled  = PHOTO_ENABLED_ON_BOOT;
volatile bool     g_shotRequested = false;
volatile uint32_t g_interval      = PHOTO_INTERVAL_MS;
volatile uint32_t g_frameId       = 0;
volatile uint32_t g_barcodeSeq    = 0;
volatile int      g_pendingQual   = 0;
volatile bool     g_scannerWanted = false;
uint16_t          g_connId        = 0;

// Экран: заполняется из BLE-колбэка, читается из обычной задачи.
String            g_lcd1, g_lcd2;
volatile uint32_t g_lcdHold    = 0;
volatile bool     g_lcdPending = false;

// ------------------------------------------------- обработчики подключения
class ServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *s, esp_ble_gatts_cb_param_t *param) override {
        g_connected = true;
        g_connId = param->connect.conn_id;
        Serial.println("[ble] клиент подключился");
        // Просим минимальный интервал соединения — от него напрямую зависит
        // скорость передачи фото. 6 * 1.25 мс = 7.5 мс, это предел BLE.
        s->updateConnParams(param->connect.remote_bda, 6, 12, 0, 400);
    }
    void onDisconnect(BLEServer *s, esp_ble_gatts_cb_param_t *) override {
        g_connected = false;
        // Обрыв связи = сценарий проверки прерван. Луч сканера гасим,
        // иначе он останется гореть и груз можно будет отсканировать
        // уже без всякого сервера.
        g_scannerWanted = false;
        Serial.println("[ble] клиент отключился, снова рекламируюсь");
        // Без этого плата станет невидимой после первого отключения
        s->startAdvertising();
    }
};

// ------------------------------------------------------- разбор команд GUI
class CtrlCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        String raw = String(c->getValue().c_str());
        raw.trim();

        // Текст для экрана разбираем ДО перевода в верхний регистр:
        // иначе "Əliyev Rəşad" превратился бы в кашу, а азербайджанские
        // буквы в UTF-8 двухбайтные — toUpperCase() их просто портит.
        if (raw.startsWith("LCD1 ") || raw.equals("LCD1")) {
            g_lcd1 = raw.length() > 5 ? raw.substring(5) : "";
            g_lcdPending = true;
            return;
        }
        if (raw.startsWith("LCD2 ") || raw.equals("LCD2")) {
            g_lcd2 = raw.length() > 5 ? raw.substring(5) : "";
            g_lcdPending = true;
            return;
        }
        if (raw.startsWith("LCDHOLD ")) {
            g_lcdHold = (uint32_t)raw.substring(8).toInt();
            return;
        }

        String cmd = raw;
        cmd.toUpperCase();
        Serial.printf("[ble] команда: %s\n", cmd.c_str());

        if (cmd == "PHOTO ON") {
            g_photoEnabled = true;
        } else if (cmd == "PHOTO OFF") {
            g_photoEnabled = false;
        } else if (cmd == "SHOT" || cmd == "VERIFY") {
            // VERIFY — то же самое действие, что SHOT, но по смыслу это
            // «снимок на опознание». Отдельное слово оставлено, чтобы в логе
            // платы было видно, кто просил кадр: человек кнопкой или сценарий КПП.
            g_shotRequested = true;
        } else if (cmd == "SCAN ON") {
            g_scannerWanted = true;
        } else if (cmd == "SCAN OFF") {
            g_scannerWanted = false;
        } else if (cmd.startsWith("INTERVAL ")) {
            long v = cmd.substring(9).toInt();
            if (v >= 200 && v <= 600000) g_interval = (uint32_t)v;
        } else if (cmd.startsWith("QUALITY ")) {
            int v = cmd.substring(8).toInt();
            if (v >= 10 && v <= 30) g_pendingQual = v;
        }
    }
};

// Сколько байт полезной нагрузки влезает в одно уведомление.
// MTU согласуется уже ПОСЛЕ установления соединения, поэтому спрашиваем
// его непосредственно перед отправкой, а не один раз при подключении.
uint16_t payloadSize() {
    uint16_t mtu = 23;
    if (g_server) {
        uint16_t m = g_server->getPeerMTU(g_connId);
        if (m >= 23) mtu = m;
    }
    uint16_t usable = mtu - 3;          // 3 байта служебных на ATT-уведомление
    uint16_t chunk  = usable - 3;       // минус наш заголовок [тип][seq u16]
    if (chunk > BLE_MAX_CHUNK) chunk = BLE_MAX_CHUNK;
    if (chunk < 16) chunk = 16;
    return chunk;
}

bool subscribed() {
    if (!g_chrData) return false;
    BLE2902 *d = (BLE2902 *)g_chrData->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));
    return d && d->getNotifications();
}

}  // namespace

namespace blep {

void begin(const char *deviceName) {
    BLEDevice::init(deviceName);
    // Просим большой MTU. Реальное значение решает клиент (Windows обычно
    // соглашается на 517, Linux/BlueZ — как настроен).
    BLEDevice::setMTU(517);
    BLEDevice::setPower(ESP_PWR_LVL_P9);

    g_server = BLEDevice::createServer();
    g_server->setCallbacks(new ServerCB());

    BLEService *svc = g_server->createService(BLEUUID(SVC_UUID), 20, 0);

    g_chrData = svc->createCharacteristic(CHR_DATA, BLECharacteristic::PROPERTY_NOTIFY);
    g_chrData->addDescriptor(new BLE2902());

    g_chrStat = svc->createCharacteristic(
        CHR_STATUS, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
    g_chrStat->addDescriptor(new BLE2902());

    g_chrCtrl = svc->createCharacteristic(
        CHR_CTRL, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    g_chrCtrl->setCallbacks(new CtrlCB());

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLEUUID(SVC_UUID));
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();

    Serial.printf("[ble] сервер поднят, имя \"%s\"\n", deviceName);
}

bool isReady() { return g_connected && subscribed(); }

bool sendBarcode(const String &text) {
    if (!isReady() || text.length() == 0) return false;

    const uint16_t chunk = payloadSize();       // сколько влезает в уведомление
    const uint32_t seq = ++g_barcodeSeq;

    uint8_t buf[300];
    size_t maxPayload = sizeof(buf) - PKT_BARCODE_OVERHEAD;
    if (maxPayload > chunk) maxPayload = chunk;

    size_t n = buildBarcode(buf, seq, text.c_str(), text.length(), maxPayload);
    g_chrData->setValue(buf, n);
    g_chrData->notify();

    Serial.printf("[gm67] код #%lu отправлен: \"%s\" (%u байт)\n",
                  (unsigned long)seq, text.c_str(), (unsigned)text.length());
    return true;
}

uint32_t barcodesSent() { return g_barcodeSeq; }

bool scannerWanted() { return g_scannerWanted; }
void setScannerWanted(bool on) { g_scannerWanted = on; }

bool sendEvent(const char *name) {
    if (!isReady() || !name) return false;
    uint8_t buf[64];
    size_t len = strlen(name);
    size_t n = buildEvent(buf, name, len, sizeof(buf) - PKT_EVENT_OVERHEAD);
    g_chrData->setValue(buf, n);
    g_chrData->notify();
    Serial.printf("[ble] событие -> серверу: %s\n", name);
    return true;
}

bool takeLcdUpdate(String &line1, String &line2, uint32_t &holdMs) {
    if (!g_lcdPending) return false;
    g_lcdPending = false;
    line1 = g_lcd1;
    line2 = g_lcd2;
    holdMs = g_lcdHold;
    g_lcdHold = 0;
    return true;
}

void sendStatus(const String &json) {
    if (!g_connected || !g_chrStat) return;
    g_chrStat->setValue((uint8_t *)json.c_str(), json.length());
    g_chrStat->notify();
}

bool sendPhoto(const uint8_t *jpeg, size_t len, uint16_t w, uint16_t h) {
    if (!isReady() || len == 0) return false;

    const uint16_t chunk    = payloadSize();
    const uint32_t nChunks  = (len + chunk - 1) / chunk;
    if (nChunks > 0xFFFF) {
        Serial.println("[ble] кадр слишком большой для 16-битного счётчика чанков");
        return false;
    }
    const uint32_t frameId = ++g_frameId;
    const uint32_t t0 = millis();

    // ---- пакет заголовка ----
    const uint16_t nc = (uint16_t)nChunks;
    uint8_t hdr[PKT_HEADER_LEN];
    size_t hlen = buildHeader(hdr, frameId, (uint32_t)len, chunk, nc, w, h);
    g_chrData->setValue(hdr, hlen);
    g_chrData->notify();

    // ---- пакеты данных ----
    uint8_t *buf = (uint8_t *)malloc(chunk + PKT_CHUNK_OVERHEAD);
    if (!buf) {
        Serial.println("[ble] не хватило памяти на буфер чанка");
        return false;
    }

    size_t off = 0;
    for (uint16_t seq = 0; seq < nc; seq++) {
        if (!g_connected) { free(buf); return false; }

        size_t n = len - off;
        if (n > chunk) n = chunk;
        size_t plen = buildChunk(buf, seq, jpeg + off, n);
        g_chrData->setValue(buf, plen);
        g_chrData->notify();
        off += n;

#if BLE_CHUNK_DELAY_MS > 0
        delay(BLE_CHUNK_DELAY_MS);
#else
        // Даже без паузы нужно периодически отдавать процессор: иначе
        // задача забивает очередь стека BLE и уведомления начинают теряться.
        if ((seq & 0x07) == 0x07) vTaskDelay(1);
#endif
    }
    free(buf);

    // ---- пакет конца ----
    uint8_t end[PKT_END_LEN];
    size_t elen = buildEnd(end, frameId, crc32_calc(jpeg, len));
    g_chrData->setValue(end, elen);
    g_chrData->notify();

    uint32_t dt = millis() - t0;
    Serial.printf("[ble] кадр #%lu отправлен: %u байт, %u чанков по %u, %lu мс (%.1f КБ/с)\n",
                  (unsigned long)frameId, (unsigned)len, nc, chunk,
                  (unsigned long)dt, dt ? (len / 1024.0f) / (dt / 1000.0f) : 0.0f);
    return true;
}

bool     photoEnabled()  { return g_photoEnabled; }
void     setPhotoEnabled(bool on) { g_photoEnabled = on; }
uint32_t photoInterval() { return g_interval; }
uint32_t framesSent()    { return g_frameId; }

bool takeShotRequested() {
    if (!g_shotRequested) return false;
    g_shotRequested = false;
    return true;
}

int pendingQuality() {
    int q = g_pendingQual;
    g_pendingQual = 0;
    return q;
}

}  // namespace blep

#endif  // ENABLE_BLE
