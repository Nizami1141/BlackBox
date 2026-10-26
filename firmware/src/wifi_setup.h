// ===========================================================================
//  Подключение к Wi-Fi: скрытая сеть + WPA3-Personal
//
//  ------------------------------------------------------------------
//  Почему сеть DGK не подключалась с первого раза
//  ------------------------------------------------------------------
//  netsh показал, что точка работает в режиме WPA3-Personal
//  (плюс WPA2-Personal как запасной вариант). У WPA3 два отличия,
//  из-за которых обычный WiFi.begin() спотыкается:
//
//   1. WPA3 использует рукопожатие SAE вместо PSK. ESP32-S3 его умеет,
//      но точка при этом ТРЕБУЕТ защищённые управляющие кадры (PMF,
//      стандарт 802.11w). Arduino по умолчанию ставит pmf.capable = true,
//      pmf.required = false — то есть «умею, но не настаиваю». Точке в
//      режиме WPA3-only этого мало, и авторизация отваливается по
//      таймауту. Именно это мы и видели: причина 2, AUTH_EXPIRE.
//
//   2. Шифры GCMP и GCMP-256, которые точка предлагает первыми,
//      ESP32 не поддерживает — он умеет только CCMP (AES). В смешанном
//      режиме точка согласится на CCMP, и всё заработает. Если же на
//      2.4 ГГц включён строго GCMP — подключиться не выйдет никак,
//      это ограничение железа, а не кода.
//
//  Решение: перебираем несколько профилей авторизации подряд и печатаем,
//  какой сработал. Так не нужно угадывать настройки роутера заранее.
//
//  ------------------------------------------------------------------
//  Про скрытую сеть
//  ------------------------------------------------------------------
//  Скрытая точка не кладёт имя в beacon, но отвечает на направленный
//  probe request с точным SSID — его ESP32 и шлёт. Имя должно совпадать
//  символ в символ. В WiFi.scanNetworks() такая сеть не появится, это
//  нормально.
//
//  ------------------------------------------------------------------
//  Про обработчик событий (здесь была ошибка, исправлено)
//  ------------------------------------------------------------------
//  Колбэк WiFi.onEvent() выполняется в системной задаче событий с
//  небольшим стеком. Печатать оттуда через Serial.printf() нельзя:
//  vsnprintf переполняет стек, и плата уходит в abort(). Поэтому колбэк
//  только записывает код причины, а печатает уже обычный код.
// ===========================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config.h"

namespace wifis {

// Пишется из колбэка, читается из обычного кода.
// Именно static, а не inline-переменная: ядро собирает C++ как gnu++11.
static volatile uint8_t  g_lastReason  = 0;
static volatile uint32_t g_reasonCount = 0;

inline const char *reasonText(uint8_t r) {
    switch (r) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE - точка найдена, но авторизация не прошла "
                         "(типично для WPA3 без PMF)";
        case 4:   return "ASSOC_EXPIRE";
        case 8:   return "ASSOC_LEAVE - точка разорвала связь";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT - почти всегда неверный пароль";
        case 200: return "BEACON_TIMEOUT - сигнал пропал";
        case 201: return "NO_AP_FOUND - сеть не найдена на этом канале";
        case 202: return "AUTH_FAIL - точка отклонила авторизацию";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "см. wifi_err_reason_t в esp-idf";
    }
}

inline void onEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        g_lastReason  = info.wifi_sta_disconnected.reason;
        g_reasonCount = g_reasonCount + 1;
    }
}

inline void drainReasons() {
    static uint32_t seen = 0;
    uint32_t now = g_reasonCount;
    if (now == seen) return;
    seen = now;
    uint8_t r = g_lastReason;
    Serial.printf("\n[wifi] причина %u: %s\n", r, reasonText(r));
}

inline bool bssidGiven(const uint8_t *b) {
    for (int i = 0; i < 6; i++)
        if (b[i] != 0) return true;
    return false;
}

// --- профили авторизации, которые перебираем ------------------------------
struct AuthProfile {
    bool        pmfRequired;   // true = обязательные защищённые кадры (WPA3/SAE)
    bool        allChannels;   // true = игнорировать WIFI_CHANNEL, искать везде
    const char *name;
};

static const AuthProfile kProfiles[] = {
    {false, false, "WPA2/WPA3 совместимый, PMF желателен"},
    {true,  false, "WPA3-SAE, PMF обязателен"},
    {false, true,  "WPA2/WPA3 совместимый, перебор всех каналов"},
    {true,  true,  "WPA3-SAE, перебор всех каналов"},
};
static const int kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

// Одна попытка по конкретному профилю.
inline bool attempt(int n, int total, const AuthProfile &p) {
    static const uint8_t kBssid[6] = WIFI_BSSID;

    // Мягкий сброс: рвём соединение, но НЕ гасим радиомодуль. Полное
    // выключение между попытками заставляет заново поднимать сетевой стек
    // и само по себе иногда роняет плату.
    WiFi.disconnect(false, true);
    delay(200);

    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#if WIFI_ALLOW_WPA1
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
#endif

    const bool    useBssid = bssidGiven(kBssid);
    const int32_t ch = p.allChannels ? 0 : (int32_t)WIFI_CHANNEL;

    Serial.printf("[wifi] попытка %d/%d: \"%s\"%s, канал %d — %s\n",
                  n, total, WIFI_SSID,
                  WIFI_HIDDEN ? " (скрытая)" : "",
                  (int)ch, p.name);

    // Ключевой момент: настраиваем БЕЗ подключения (последний аргумент false),
    // затем правим PMF в конфигурации и только потом подключаемся вручную.
    // Прямого способа задать PMF через Arduino API в этой версии ядра нет.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, ch,
               useBssid ? kBssid : nullptr, false);

    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        cfg.sta.pmf_cfg.capable  = true;
        cfg.sta.pmf_cfg.required = p.pmfRequired;
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    esp_wifi_connect();

    uint32_t t0 = millis();
    while (millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) return true;
        drainReasons();
        delay(300);
        Serial.print(".");
    }
    Serial.println();
    return false;
}

// ---------------------------------------------------------------------------
//  Поднять собственную точку доступа.
//  Адрес платы в своей сети всегда 192.168.4.1 — это значение по умолчанию
//  в esp-idf, менять его для стенда незачем.
// ---------------------------------------------------------------------------
inline void startAP() {
    WiFi.mode(WIFI_AP);
    delay(200);

    bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL,
                          false /* не скрывать SSID */, AP_MAX_CLIENTS);

    Serial.println();
    Serial.println("==================================================");
    if (!ok) {
        Serial.println(" НЕ УДАЛОСЬ поднять точку доступа");
        Serial.println("==================================================");
        return;
    }
    Serial.println(" ПЛАТА РАЗДАЁТ СВОЮ СЕТЬ");
    Serial.printf ("   имя сети : %s\n", AP_SSID);
    Serial.printf ("   пароль   : %s\n", AP_PASSWORD);
    Serial.printf ("   канал    : %d\n", AP_CHANNEL);
    Serial.println("--------------------------------------------------");
    Serial.printf ("   1. Подключитесь к сети \"%s\" с ноутбука\n", AP_SSID);
    Serial.printf ("   2. Откройте в браузере: http://%s/\n",
                   WiFi.softAPIP().toString().c_str());
    Serial.println("==================================================");
    Serial.println();
}

inline void begin() {
    WiFi.persistent(false);           // не насиловать флеш записью настроек

#if WIFI_AP_ONLY
    // Режим стенда: к чужим сетям не подключаемся вообще, сразу раздаём свою.
    // Так проверка камеры не зависит ни от роутера, ни от WPA3, ни от того,
    // пустят ли устройство в корпоративную сеть.
    Serial.println("[wifi] режим: собственная точка доступа (WIFI_AP_ONLY = 1)");
    startAP();
    return;
#else

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);             // иначе задержки скачут на сотни мс
    WiFi.onEvent(onEvent);

    const int total = kProfileCount * WIFI_MAX_ATTEMPTS;
    int n = 0;

    for (int pass = 0; pass < WIFI_MAX_ATTEMPTS; pass++) {
        for (int i = 0; i < kProfileCount; i++) {
            n++;
            if (attempt(n, total, kProfiles[i])) {
                Serial.printf("\n[wifi] ПОДКЛЮЧЕНО профилем: %s\n", kProfiles[i].name);
                Serial.printf("[wifi] IP=%s, RSSI=%d dBm, канал %d\n",
                              WiFi.localIP().toString().c_str(),
                              WiFi.RSSI(), WiFi.channel());
                Serial.printf("[wifi] поток: http://%s/\n",
                              WiFi.localIP().toString().c_str());
                if (WIFI_CHANNEL == 0)
                    Serial.printf("[wifi] совет: впишите WIFI_CHANNEL %d в config.h "
                                  "- подключение станет быстрее\n", WiFi.channel());
                return;
            }
            drainReasons();
        }
    }

    Serial.println("\n[wifi] ни один профиль не подошёл.");
    Serial.println("       Сеть DGK работает в режиме WPA3-Personal с шифрами");
    Serial.println("       GCMP-256 / GCMP / CCMP. ESP32 умеет только CCMP.");
    Serial.println("       Если на 2.4 ГГц включён строго GCMP - подключиться");
    Serial.println("       нельзя в принципе, это ограничение железа.");
    Serial.println("       Варианты: включить на роутере режим WPA2/WPA3 mixed,");
    Serial.println("       либо поставить WIFI_AP_ONLY = 1 в config.h.");
    Serial.println("[wifi] поднимаю собственную точку доступа");

    startAP();
#endif  // WIFI_AP_ONLY
}

}  // namespace wifis
