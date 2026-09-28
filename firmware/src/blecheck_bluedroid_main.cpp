// BLE-only диагностика на штатном Bluedroid Arduino-ESP32.
// Используется вместо NimBLE для совместимости GATT с Windows/WinRT.
#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

namespace {

constexpr char DEVICE_NAME[] = "S3-COMM-TEST";
constexpr char SERVICE_UUID[] = "7cf0a100-7f6b-4da8-a5f7-1c3db7400001";
constexpr char RX_UUID[] = "7cf0a100-7f6b-4da8-a5f7-1c3db7400002";
constexpr char TX_UUID[] = "7cf0a100-7f6b-4da8-a5f7-1c3db7400003";

BLECharacteristic *g_tx = nullptr;
volatile bool g_connected = false;
volatile uint32_t g_pingCount = 0;
uint32_t g_lastHeartbeat = 0;

class ServerCallbacks final : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        g_connected = true;
        Serial.println("[PASS] BLE подключение       : Windows/телефон подключён");
    }

    void onDisconnect(BLEServer *server) override {
        g_connected = false;
        Serial.println("[ble] клиент отключился — реклама запущена снова");
        delay(100);
        server->startAdvertising();
    }
};

class RxCallbacks final : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        std::string raw = characteristic->getValue();
        String value;
        for (char ch : raw) value += ch;
        value.trim();
        Serial.printf("[ble] RX: \"%s\" (%u байт)\n", value.c_str(),
                      static_cast<unsigned>(raw.size()));

        String reply;
        if (value.equalsIgnoreCase("PING")) {
            reply = "PONG " + String(++g_pingCount);
        } else {
            reply = "ECHO: " + value;
        }
        g_tx->setValue(reply.c_str());
        g_tx->notify();
        Serial.printf("[PASS] BLE обмен данными    : %s -> %s\n",
                      value.c_str(), reply.c_str());
    }
};

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1200);
    Serial.println();
    Serial.println("============================================================");
    Serial.println(" ESP32-S3 BLE CHECK — Bluedroid/Windows compatible");
    Serial.println("============================================================");

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setMTU(23);
    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    BLEService *service = server->createService(SERVICE_UUID);
    BLECharacteristic *rx = service->createCharacteristic(
        RX_UUID, BLECharacteristic::PROPERTY_WRITE |
                     BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCallbacks());

    g_tx = service->createCharacteristic(
        TX_UUID, BLECharacteristic::PROPERTY_READ |
                     BLECharacteristic::PROPERTY_NOTIFY);
    g_tx->setValue("READY");
    // Явный Client Characteristic Configuration Descriptor. Windows пишет
    // именно его при start_notify; отсутствие/ошибка CCCD даёт Unreachable.
    g_tx->addDescriptor(new BLE2902());

    service->start();
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x000C);
    advertising->setMaxPreferred(0x0018);
    BLEDevice::startAdvertising();

    Serial.printf("[PASS] BLE стек запущен      : имя %s\n", DEVICE_NAME);
    Serial.println("       GATT service + RX/write + TX/read/notify готовы");
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
    }
    delay(20);
}
