// ===========================================================================
//  ESP32-S3: проверка встроенных беспроводных интерфейсов без модулей
//
//  1. Wi-Fi: запуск STA, чтение MAC, сканирование эфира 2.4 ГГц.
//  2. BLE: реклама, подключение центрального устройства, обмен PING/PONG.
//
//  ESP32-S3 НЕ поддерживает Bluetooth Classic — проверяется только BLE.
//  Среда PlatformIO: s3_commscheck.
// ===========================================================================

#if defined(FW_S3_COMMSCHECK)

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#ifndef COMMSCHECK_WIFI
  #define COMMSCHECK_WIFI 1
#endif
#if COMMSCHECK_WIFI
  #include <WiFi.h>
#endif

namespace {

constexpr char DEVICE_NAME[] = "S3-COMM-TEST";
constexpr char SERVICE_UUID[] = "7cf0a100-7f6b-4da8-a5f7-1c3db7400001";
constexpr char RX_UUID[]      = "7cf0a100-7f6b-4da8-a5f7-1c3db7400002";
constexpr char TX_UUID[]      = "7cf0a100-7f6b-4da8-a5f7-1c3db7400003";

NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_tx = nullptr;
volatile bool g_connected = false;
volatile bool g_gotPing = false;
volatile uint32_t g_pingCount = 0;
uint32_t g_lastHeartbeat = 0;

class ServerCallbacks final : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer *) override {
        g_connected = true;
        Serial.println("[PASS] BLE подключение       : центральное устройство подключено");
    }

    void onDisconnect(NimBLEServer *) override {
        g_connected = false;
        // Сервер сам возобновляет рекламу через advertiseOnDisconnect().
        // Повторный ручной startAdvertising() создавал гонку и предупреждение
        // "Advertising already active" при быстром Windows reconnect.
        Serial.println("[ble] устройство отключилось — реклама возобновится автоматически");
    }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic *characteristic) override {
        std::string raw = characteristic->getValue();
        String value;
        for (char c : raw) value += c;
        value.trim();

        Serial.printf("[ble] RX: \"%s\" (%u байт)\n",
                      value.c_str(), static_cast<unsigned>(raw.size()));

        if (value.equalsIgnoreCase("PING")) {
            uint32_t number = ++g_pingCount;
            String reply = "PONG " + String(number);
            g_tx->setValue(reply.c_str());
            g_tx->notify();
            g_gotPing = true;
            Serial.printf("[PASS] BLE обмен данными    : PING -> %s\n", reply.c_str());
        } else {
            String reply = "ECHO: " + value;
            g_tx->setValue(reply.c_str());
            g_tx->notify();
            Serial.printf("[ble] TX: \"%s\"\n", reply.c_str());
        }
    }
};

#if COMMSCHECK_WIFI
void testWiFi() {
    Serial.println();
    Serial.println("[wifi] запускаю радиоблок и сканирую сети 2.4 ГГц...");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    delay(100);

    String mac = WiFi.macAddress();
    bool macOk = mac != "00:00:00:00:00:00" && mac.length() == 17;
    Serial.printf("[%s] Wi-Fi инициализация    : MAC %s\n",
                  macOk ? "PASS" : "FAIL", mac.c_str());

    int count = WiFi.scanNetworks(false, true, false, 300);
    if (count < 0) {
        Serial.printf("[FAIL] Wi-Fi сканирование    : код %d\n", count);
    } else if (count == 0) {
        Serial.println("[WARN] Wi-Fi сканирование    : сетей 2.4 ГГц не найдено");
        Serial.println("       Это не доказывает поломку: рядом может не быть такой сети.");
    } else {
        Serial.printf("[PASS] Wi-Fi приём RF        : найдено сетей: %d\n", count);
        int shown = count > 5 ? 5 : count;
        for (int i = 0; i < shown; ++i) {
            Serial.printf("       %2d) ch=%2d RSSI=%4d dBm  %s\n",
                          i + 1, WiFi.channel(i), WiFi.RSSI(i),
                          WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "<hidden>");
        }
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    delay(150);
}
#endif

void startBle() {
    Serial.println();
    Serial.println("[ble] запускаю BLE GATT сервер...");
    NimBLEDevice::init(DEVICE_NAME);
    // Оставляем стандартный MTU 23. Для PING/PONG его достаточно, а ранний
    // обмен MTU=247 на некоторых Windows/WinRT адаптерах обрывает discovery.
    // Для стендового теста pairing не нужен: важно проверить именно
    // advertising/connect/GATT. В рабочей прошивке безопасность задаётся
    // отдельно согласно модели угроз поста.
    NimBLEDevice::setSecurityAuth(false, false, false);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());
    g_server->advertiseOnDisconnect(true);
    NimBLEService *service = g_server->createService(SERVICE_UUID);

    NimBLECharacteristic *rx = service->createCharacteristic(
        RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallbacks());

    g_tx = service->createCharacteristic(
        TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_tx->setValue("READY");

    service->start();
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    // 15–30 мс — безопасный диапазон для Windows BLE и большинства телефонов.
    advertising->setMinPreferred(0x000C);
    advertising->setMaxPreferred(0x0018);
    NimBLEDevice::startAdvertising();

    Serial.printf("[PASS] BLE стек запущен      : имя %s\n", DEVICE_NAME);
    Serial.printf("       Service: %s\n", SERVICE_UUID);
    Serial.printf("       RX/write: %s\n", RX_UUID);
    Serial.printf("       TX/read+notify: %s\n", TX_UUID);
    Serial.println("       Подключитесь и запишите ASCII PING в RX.");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1200);

    Serial.println();
    Serial.println("============================================================");
    Serial.println(" ESP32-S3 COMMUNICATION TEST — Wi-Fi + BLE");
    Serial.println(" Без камеры, W5500, LCD и GM67");
    Serial.println("============================================================");
    Serial.printf("Chip: %s rev %u, heap=%u КБ, PSRAM=%u КБ\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  static_cast<unsigned>(ESP.getFreeHeap() / 1024U),
                  static_cast<unsigned>(ESP.getFreePsram() / 1024U));

#if COMMSCHECK_WIFI
    testWiFi();
#else
    Serial.println("[SKIP] Wi-Fi исключён из компактной BLE-only прошивки");
#endif
    startBle();
    g_lastHeartbeat = millis();
}

void loop() {
    if (millis() - g_lastHeartbeat >= 5000) {
        g_lastHeartbeat = millis();
        Serial.printf("[alive] BLE=%s, PING=%lu, heap=%u КБ, temp=%.1f C\n",
                      g_connected ? "CONNECTED" : "ADVERTISING",
                      static_cast<unsigned long>(g_pingCount),
                      static_cast<unsigned>(ESP.getFreeHeap() / 1024U),
                      temperatureRead());
        if (!g_connected && !g_gotPing) {
            Serial.println("        Жду телефон/ноутбук: имя S3-COMM-TEST");
        }
    }
    delay(20);
}

#endif  // FW_S3_COMMSCHECK
