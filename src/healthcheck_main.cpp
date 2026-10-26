// ===========================================================================
//  Автономная диагностика ГОЛОЙ ESP32-S3-WROOM
//
//  Никакая внешняя периферия не используется: камера, W5500, LCD, GM67,
//  кнопка и GPIO не проверяются. Среда сборки: s3_healthcheck.
// ===========================================================================

#if defined(FW_S3_HEALTHCHECK)

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#ifndef HEALTHCHECK_RADIO
  #define HEALTHCHECK_RADIO 1
#endif
#if HEALTHCHECK_RADIO
  #include <WiFi.h>
#include <esp_wifi.h>
#endif

namespace {

uint16_t g_passed = 0;
uint16_t g_failed = 0;
uint32_t g_lastHeartbeat = 0;

void result(const char *name, bool ok, const String &details = "") {
    Serial.printf("[%s] %-22s", ok ? "PASS" : "FAIL", name);
    if (details.length()) Serial.printf(" : %s", details.c_str());
    Serial.println();
    if (ok) g_passed++;
    else g_failed++;
}

const char *resetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "подача питания";
        case ESP_RST_EXT:       return "кнопка EN/RST";
        case ESP_RST_SW:        return "программный сброс";
        case ESP_RST_PANIC:     return "авария программы";
        case ESP_RST_INT_WDT:   return "watchdog прерываний";
        case ESP_RST_TASK_WDT:  return "watchdog задачи";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "выход из deep sleep";
        case ESP_RST_BROWNOUT:  return "просадка питания (brownout)";
        default:                return "другая причина";
    }
}

// Пишем во всю выбранную область псевдослучайный, но воспроизводимый
// шаблон, затем читаем его обратно. Это выявляет не только невозможность
// выделить память, но и битые/нестабильные линии памяти.
bool memoryPatternTest(uint32_t caps, size_t wanted, String &details) {
    uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(wanted, caps));
    if (!buffer) {
        details = "не удалось выделить " + String(wanted / 1024) + " КБ";
        return false;
    }

    uint32_t state = 0x53A3C96DU;
    for (size_t i = 0; i < wanted; ++i) {
        state = state * 1664525U + 1013904223U;
        buffer[i] = static_cast<uint8_t>(state >> 24);
    }

    bool ok = true;
    state = 0x53A3C96DU;
    size_t badAt = 0;
    for (size_t i = 0; i < wanted; ++i) {
        state = state * 1664525U + 1013904223U;
        if (buffer[i] != static_cast<uint8_t>(state >> 24)) {
            ok = false;
            badAt = i;
            break;
        }
    }
    heap_caps_free(buffer);

    details = String(wanted / 1024) + " КБ проверено";
    if (!ok) details += ", ошибка по смещению " + String(badAt);
    return ok;
}

bool nvsTest(String &details) {
    Preferences prefs;
    if (!prefs.begin("s3-health", false)) {
        details = "Preferences.begin не открыл NVS";
        return false;
    }
    const uint32_t probe = 0xA55A39C3U;
    size_t written = prefs.putUInt("probe", probe);
    uint32_t readBack = prefs.getUInt("probe", 0);
    prefs.remove("probe");
    prefs.end();
    details = "запись/чтение служебного значения";
    return written == sizeof(uint32_t) && readBack == probe;
}

#if HEALTHCHECK_RADIO
bool radioTest(String &details) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    delay(80);

    uint8_t mac[6] = {};
    esp_err_t macRc = esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_err_t channelRc = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    bool nonZero = false;
    for (uint8_t b : mac) nonZero |= b != 0;

    char text[18];
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    details = "Wi-Fi MAC ";
    details += text;

    bool ok = macRc == ESP_OK && channelRc == ESP_OK && nonZero;
    WiFi.mode(WIFI_OFF);
    return ok;
}
#endif

void runHealthcheck() {
    g_passed = 0;
    g_failed = 0;

    Serial.println();
    Serial.println("============================================================");
    Serial.println(" ESP32-S3-WROOM SELF TEST — только плата, без модулей");
    Serial.println("============================================================");
    Serial.printf("Сборка: %s %s\n", __DATE__, __TIME__);
    Serial.printf("Причина сброса: %s\n", resetReasonText(esp_reset_reason()));

    String model = ESP.getChipModel();
    result("Модель чипа", model.indexOf("ESP32-S3") >= 0,
           model + ", rev " + String(ESP.getChipRevision()) +
           ", ядер " + String(ESP.getChipCores()));

    const uint32_t flashSize = ESP.getFlashChipSize();
    result("Flash обнаружена", flashSize >= 4U * 1024U * 1024U,
           String(flashSize / (1024U * 1024U)) + " МБ, " +
           String(ESP.getFlashChipSpeed() / 1000000U) + " МГц");

    const bool psramPresent = psramFound() && ESP.getPsramSize() > 0;
    result("PSRAM обнаружена", psramPresent,
           String(ESP.getPsramSize() / (1024U * 1024U)) + " МБ");

    String details;
    result("Внутренняя RAM", memoryPatternTest(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                               32U * 1024U, details), details);

    if (psramPresent) {
        details = "";
        result("PSRAM запись/чтение",
               memoryPatternTest(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 512U * 1024U, details), details);
    } else {
        result("PSRAM запись/чтение", false,
               "пропущено: PSRAM не инициализирована");
    }

    details = "";
    result("NVS во Flash", nvsTest(details), details);

#if HEALTHCHECK_RADIO
    details = "";
    result("Радиоблок Wi-Fi", radioTest(details), details);
#else
    Serial.println("[SKIP] Радиоблок Wi-Fi       : исключён из облегчённой прошивки");
#endif

    bool heapOk = heap_caps_check_integrity_all(true);
    result("Целостность heap", heapOk,
           "свободно " + String(ESP.getFreeHeap() / 1024U) + " КБ");

    float temp = temperatureRead();
    result("Датчик температуры", temp > -20.0f && temp < 125.0f,
           String(temp, 1) + " °C");

    Serial.println("------------------------------------------------------------");
    Serial.printf("ИТОГ: PASS=%u, FAIL=%u -> %s\n",
                  g_passed, g_failed,
                  g_failed == 0 ? "ПЛАТА ПРОШЛА БАЗОВУЮ ПРОВЕРКУ"
                                : "ЕСТЬ ОШИБКИ — СМОТРИТЕ FAIL ВЫШЕ");
    Serial.println("Команда R + Enter — повторить тест.");
    Serial.println("============================================================");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1200);
    runHealthcheck();
    g_lastHeartbeat = millis();
}

void loop() {
    while (Serial.available()) {
        char c = static_cast<char>(Serial.read());
        if (c == 'r' || c == 'R') runHealthcheck();
    }

    if (millis() - g_lastHeartbeat >= 5000) {
        g_lastHeartbeat = millis();
        Serial.printf("[alive] uptime=%lu c, heap=%u КБ, PSRAM=%u КБ, temp=%.1f C\n",
                      static_cast<unsigned long>(millis() / 1000U),
                      static_cast<unsigned>(ESP.getFreeHeap() / 1024U),
                      static_cast<unsigned>(ESP.getFreePsram() / 1024U),
                      temperatureRead());
    }
    delay(20);
}

#endif  // FW_S3_HEALTHCHECK
