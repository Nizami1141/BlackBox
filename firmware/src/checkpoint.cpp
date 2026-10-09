#include "checkpoint.h"

#include "config.h"

#if ENABLE_ETH

#include "button.h"
#include "camera.h"
#include "eth_link.h"
#include "gm67.h"
#include "interboard_link.h"
#include "lcd.h"
#include "power.h"

namespace {

GM67 *g_scanner = nullptr;
uint32_t g_checks = 0;
uint32_t g_records = 0;
uint32_t g_lastLinkCheck = 0;
bool     g_lastLinkUp = true;
uint32_t g_lastCheckEnd = 0;      // когда закончилась последняя проверка
uint32_t g_lastHeartbeat = 0;
uint32_t g_lastPhysicalPress = 0;
constexpr uint32_t HEARTBEAT_MS = 3000;
constexpr uint32_t DOUBLE_PRESS_MS = 2000;
bool     g_backlightOn = true;

void checkDoublePress() {
#if ENABLE_BUTTON
    uint32_t pressedAt = 0;
    while (btn::pressedAt(pressedAt)) {
        if (g_lastPhysicalPress &&
            pressedAt - g_lastPhysicalPress <= DOUBLE_PRESS_MS) {
            Serial.println("[cp] двойное нажатие — перезагрузка ESP");
            Serial.flush();
            ESP.restart();
        }
        g_lastPhysicalPress = pressedAt;
    }
#endif
}

void heartbeatIfDue() {
    const uint32_t now = millis();
    if (now - g_lastHeartbeat < HEARTBEAT_MS) return;
    g_lastHeartbeat = now;
    if (!eth::asleep()) eth::heartbeat();
}

// ------------------------------------------------------------- подсветка ---
// Экран — единственный узел, который человек видит до того, как система
// что-то сделает. Поэтому он зажигается ПЕРВЫМ и гаснет ПОСЛЕДНИМ, уже
// после того как вердикт дочитан.
void backlight(bool on) {
#if ENABLE_LCD
    if (on == g_backlightOn) return;
    lcd::backlight(on);
    g_backlightOn = on;
#endif
}

void show(const String &l1, const String &l2) {
#if ENABLE_LCD
    lcd::show(l1, l2);
#endif
    Serial.printf("[lcd] %s | %s\n", l1.c_str(), l2.c_str());
}

void showFor(const String &l1, const String &l2, uint32_t ms) {
#if ENABLE_LCD
    lcd::showFor(l1, l2, ms);
#endif
    Serial.printf("[lcd] %s | %s\n", l1.c_str(), l2.c_str());
}

void showIdle() {
    show(LCD_IDLE_1, LCD_IDLE_2);
}

// Строки экрана берём от сервера, если он их прислал: тексты живут
// в server/config.yaml, и поправить формулировку можно, не перепрошивая
// посты. Если сервер молчит — показываем местные из config.h.
String pick(const String &fromServer, const char *fallback) {
    return fromServer.length() ? fromServer : String(fallback);
}

// ------------------------------------------------------- ожидание штрихкода
// Возвращает прочитанный код или пустую строку по таймауту.
//
// ПРО ПЕРЕВЗВОД — без него это место молча не работает.
//
// В Host Mode команда START_DECODE запускает ОДИН сеанс чтения, а не
// включает луч навсегда. Сеанс кончается, когда сканер прочитал код либо
// когда истекла длительность одиночного скана (PX_SINGLE_SCAN, у GM67 это
// 5 секунд). А ждём мы штрихкод до GM67_SCAN_WINDOW_MS — заметно дольше.
//
// Без перевзвода выходило так: сотрудника опознали, на экране «Kodu skan
// edin», луч честно загорелся — и через пять секунд тихо погас. Оставшееся
// время плата ждала того, чего уже не могло произойти, и проверка доходила
// до «Vaxt bitdi» при полностью исправных сканере и связи. В BLE-режиме это
// чинится в main.cpp (rearmScanner), а здесь такого не было.
//
// Перевзводим, только когда в приёмном тракте тихо: startDecode() чистит
// буфер, и вызов посреди приходящего кода потерял бы этот код.
String waitBarcode(uint32_t windowMs) {
    if (!g_scanner) return "";

#if GM67_PASSIVE_UART_MODE
    // Не трогаем питание, режим и луч сканера. Приём разрешён только после
    // опознания лица: старые байты, накопившиеся при съёмке, выбрасываем.
    // Остаёмся на рабочей частоте CPU, чтобы не добавлять изменение UART-
    // делителя к диагностике сканера.
    g_scanner->flushInput();
    const uint32_t rxAtStart = g_scanner->rxBytes();
    const uint32_t droppedAtStart = g_scanner->droppedBytes();
    Serial.printf("[gm67] пассивный UART: жду НОВЫЙ штрихкод %lu с, "
                  "команды сканеру не отправляю\n",
                  (unsigned long)(windowMs / 1000));
#else
#if POWER_MODE
    // ------------------------------------------------------ частота вниз ---
    // Это самая тяжёлая по току фаза всей проверки: горит луч сканера, а он
    // ест 180 мА — больше, чем любой другой узел поста. Держать при этом
    // процессор на 240 МГц незачем: всё, что он здесь делает, — опрашивает
    // UART на 9600 бод и ждёт человека. Лишние ~40 мА именно там, где запас
    // по питанию меньше всего.
    //
    // Ограничение «частоту менять только при выключенной камере» здесь не
    // действует: к этому моменту snapshotOnce() уже усыпил её, XCLK снят,
    // портить нечего.
    power::idle();

    // Делители UART считаются от шины APB, а она уехала вслед за частотой
    // процессора. Пересчитываем скорость явно — иначе сканер получит команду
    // на втрое неверной скорости, промолчит, и это будет выглядеть как
    // «сканер сломался».
    if (g_scanner->port()) g_scanner->port()->updateBaudRate(GM67_BAUD);
#endif

    g_scanner->wake();

    // Если настройка при загрузке не удалась, сканер сейчас в неизвестном
    // режиме — скорее всего в заводском Continuous, где startDecode() не
    // действует вовсе. Повторяем настройку здесь: к этому моменту он давно
    // загрузился, и вторая попытка почти всегда проходит. Без неё пост
    // оставался бы без сканера до перезагрузки из-за одной ранней проверки.
    if (!g_scanner->configured()) {
        Serial.println("[gm67] режим не подтверждён при загрузке — настраиваю сейчас");
        if (g_scanner->configure(GM67::Mode::HOST, false, false)) {
            Serial.println("[gm67] сканер настроен со второй попытки");
        } else {
            Serial.println("[gm67] сканер по-прежнему не отвечает — луч может не зажечься");
        }
    }

    g_scanner->startDecode();
#endif

    String code;
    uint32_t t0 = millis();
#if GM67_PASSIVE_UART_MODE
    uint32_t lastStatusMs = t0;
#else
    uint32_t rearmMs = millis();
#endif
    while (millis() - t0 < windowMs) {
        checkDoublePress();
        heartbeatIfDue();
        String s;
        if (g_scanner->poll(s)) {
            s.trim();
            if (s.length()) {
                code = s;
                break;
            }
#if !GM67_PASSIVE_UART_MODE
            // Строка пришла, но не прошла отбор (обрывок служебного кадра).
            // Сеанс чтения уже потрачен — взводим не откладывая, иначе
            // единственная попытка сотрудника пропала бы впустую.
            rearmMs = 0;
#endif
        }
        // Долгое нажатие во время ожидания = отмена. Сотрудник передумал
        // или поднёс не тот груз — он не должен ждать полминуты таймаута.
#if ENABLE_BUTTON
        if (btn::longPressed()) {
            Serial.println("[cp] отмена длинным нажатием");
            break;
        }
#endif
#if !GM67_PASSIVE_UART_MODE
        if (millis() - rearmMs >= (uint32_t)GM67_REARM_MS && g_scanner->idle()) {
            rearmMs = millis();
            g_scanner->startDecode();
        }
#elif ENABLE_LCD
        if (millis() - lastStatusMs >= 10000) {
            lastStatusMs = millis();
            Serial.printf("[gm67] жду код: UART RX=%lu байт, LCD=%s, "
                          "I2C ошибок=%lu\n",
                          (unsigned long)(g_scanner->rxBytes() - rxAtStart),
                          lcd::ok() ? "OK" : "FAIL",
                          (unsigned long)lcd::errors());
        }
#else
        if (millis() - lastStatusMs >= 10000) {
            lastStatusMs = millis();
            Serial.printf("[gm67] жду код: UART RX=%lu байт\n",
                          (unsigned long)(g_scanner->rxBytes() - rxAtStart));
        }
#endif
        delay(20);
    }

#if GM67_PASSIVE_UART_MODE
    Serial.printf("[gm67] пассивный UART: принято %lu байт, отброшено %lu, "
                  "код %s\n",
                  (unsigned long)(g_scanner->rxBytes() - rxAtStart),
                  (unsigned long)(g_scanner->droppedBytes() - droppedAtStart),
                  code.length() ? "получен" : "НЕ получен");
    g_scanner->flushInput();
#else
    g_scanner->stopDecode();
    g_scanner->flushInput();
#if POWER_MODE
    // Сканер — самый прожорливый узел поста: 180 мА круглосуточно, если
    // его не усыплять. Между проверками он спит.
    // Усыпляем ЕЩЁ на 80 МГц: скорость UART сейчас пересчитана под эту
    // частоту, и команда уйдёт правильно.
    g_scanner->sleep();

    // Дальше вызывающий пойдёт отправлять штрихкод на сервер — там нужна
    // скорость. Поднимаем частоту обратно и снова чиним скорость UART.
    power::active();
    if (g_scanner->port()) g_scanner->port()->updateBaudRate(GM67_BAUD);
#endif
#endif
    return code;
}

// -------------------------------------------------------------- в покой ---
// Общий выход из проверки, каким бы ни был исход. Гасим всё в обратном
// порядке: сначала то, что включили последним.
//
// Подсветку здесь НЕ трогаем: на экране висит вердикт, и человек его ещё
// читает. Её погасит loop() через LCD_BACKLIGHT_OFF_MS.
void toIdle() {
    checkDoublePress();
    heartbeatIfDue();
#if CAM_OFF_AFTER_CHECK && !INTERBOARD_GATEWAY
    cam::off();
#endif
#if ETH_SLEEP_IDLE
    eth::sleep();
#endif
    power::idle();
    g_lastCheckEnd = millis();
}

// ------------------------------------------------------------ один проход
void runCheck() {
    g_checks++;
    eth::registryLog("info", "Verification started");
    Serial.printf("\n[cp] === проверка #%lu ===\n", (unsigned long)g_checks);

    // === ЭТАП 1: экран ======================================================
    // Первым делом отклик человеку. Всё остальное занимает секунды, и без
    // этой строки пост выглядит так, будто кнопка не сработала.
    // Частоту поднимаем здесь же: менять её при работающей камере нельзя,
    // а камера пока выключена.
    power::active();
    backlight(true);
#if ENABLE_LCD
    // Переинициализируем панель перед каждой проверкой. Рассинхрон
    // 4-битного режима не даёт ошибок на шине — расширитель подтверждает
    // байты как ни в чём не бывало, — и без этого экран остаётся с мусором
    // до перезагрузки. Двадцать миллисекунд на фоне пятисекундной проверки.
    lcd::resync();
#endif
    show(LCD_WAIT_1, LCD_WAIT_2);

    // === ЭТАП 2: сеть =======================================================
    // Будим ДО камеры: автопереговоры занимают секунду-три, и пусть они
    // идут, пока человек только устраивается перед объективом. Разбудить
    // после съёмки значило бы держать кадр в PSRAM и ждать впустую.
#if ETH_SLEEP_IDLE
    if (!eth::wake()) {
        showFor(LCD_NOLINK_1, LCD_NOLINK_2, LCD_HOLD_MS);
        toIdle();
        return;
    }
#endif

    // === ЭТАП 3: камера =====================================================
    size_t len = 0;
    uint16_t w = 0, h = 0;
    uint8_t *jpg = nullptr;
#if INTERBOARD_GATEWAY
    String cameraError;
    if (!interboard::requestFrame(&jpg, &len, &w, &h, cameraError)) {
        Serial.printf("[cp] кадр от ESP32-S3 не получен: %s\n", cameraError.c_str());
    }
#else
    jpg = cam::snapshotOnce(&len, &w, &h);
#endif
#if CAM_OFF_AFTER_CHECK && !INTERBOARD_GATEWAY
    // Выключаем немедленно, не дожидаясь таймера простоя: кадр уже в PSRAM,
    // сенсор больше не нужен, а только полное выключение драйвера
    // гарантированно снимает XCLK.
    cam::off();
#endif
    if (!jpg || len == 0) {
        Serial.println("[cp] не удалось получить кадр");
        showFor(LCD_ERR_1,
#if INTERBOARD_GATEWAY
                "ESP32-S3 yoxdur",
#else
                "Kamera",
#endif
                LCD_HOLD_MS);
        if (jpg) free(jpg);
        toIdle();
        return;
    }
    Serial.printf("[cp] кадр %ux%u, %u байт\n", w, h, (unsigned)len);

    // === ЭТАП 4: отправка кадра и ожидание ответа ===========================
    show(LCD_SEARCH_1, LCD_SEARCH_2);
    eth::VerifyResult v = eth::verify(jpg, len);

    // Одна повторная попытка при СЕТЕВОМ сбое. Зачем: на стенде линк может
    // моргнуть ровно в момент передачи (дребезг питания макетки), и без
    // повтора человек получает отказ, хотя через две секунды всё живо.
    // Повторяем только сетевые ошибки (v.error): ответ сервера «не узнан» —
    // это решение, а не сбой, и его повторять нельзя.
    if (!v.ok && v.error.length() && v.httpCode == 0) {
        Serial.printf("[cp] сбой связи (%s) — жду линк и повторяю один раз\n",
                      v.error.c_str());
        uint32_t t0 = millis();
        while (millis() - t0 < 4000 && !eth::linkUp()) delay(100);
        if (eth::linkUp()) v = eth::verify(jpg, len);
    }
    free(jpg);                      // кадр больше не нужен, PSRAM освобождаем сразу

    if (!v.ok) {
        eth::registryLog("error", "Recognition request failed");
        Serial.printf("[cp] проверка не завершена (HTTP %d): %s\n",
                      v.httpCode, v.error.c_str());
        showFor(pick(v.lcd1, LCD_ERR_1), pick(v.lcd2, "Sistem xetasi"), LCD_HOLD_MS);
        toIdle();
        return;
    }

    if (!v.matched) {
        eth::registryLog("info", "Face not recognized");
        Serial.printf("[cp] отказ: %s (схожесть %.3f)\n",
                      v.reason.c_str(), v.score);
        // На экране — только вердикт. Ни оценки схожести, ни имён похожих:
        // сотрудник у поста этого видеть не должен, всё это уходит
        // оператору в журнал.
        showFor(pick(v.lcd1, LCD_DENY_1), pick(v.lcd2, LCD_DENY_2), LCD_HOLD_MS);
        toIdle();
        return;
    }

    Serial.printf("[cp] опознан: %s (схожесть %.3f, токен живёт %d с)\n",
                  v.name.c_str(), v.score, v.expiresIn);

    // === ЭТАП 5: экран ПЕРЕД сканером =======================================
    // Порядок здесь не косметика. Человек должен прочитать «поднесите код»
    // раньше, чем увидит луч, иначе загоревшийся сканер выглядит как
    // случайность. Луч включается ТОЛЬКО после того, как сервер выдал токен.
    show(pick(v.lcd1, v.name.c_str()), pick(v.lcd2, LCD_OK_2));

    // Ждать дольше, чем живёт токен, бессмысленно: сервер всё равно откажет.
    uint32_t window = GM67_SCAN_WINDOW_MS;
    if (v.expiresIn > 1) {
        uint32_t byToken = (uint32_t)(v.expiresIn - 1) * 1000UL;
        if (byToken < window) window = byToken;
    }

    // === ЭТАП 6: сканер =====================================================
    // waitBarcode сам будит сканер, ждёт код и усыпляет его обратно.
    String code = waitBarcode(window);
    if (code.length() == 0) {
        eth::registryLog("warn", "Barcode missing or canceled");
        Serial.println("[cp] штрихкод не поднесли");
        showFor(LCD_ERR_1, "Vaxt bitdi", LCD_HOLD_MS);
        toIdle();
        return;
    }
    Serial.printf("[cp] штрихкод: %s\n", code.c_str());

    // === ЭТАП 7: отправка штрихкода =========================================
    eth::ScanResult s = eth::scan(v.token, code);

    // У центрального API не документирована идемпотентность записи груза:
    // потерянный ответ мог означать уже выполненную запись. Автоматический
    // повтор оставляем только для локального сервера с защитой сессии.
#if !ETH_DIRECT_AKS
    // Тот же повтор при сетевом сбое, что и у verify — штрихкод уже прочитан,
    // человек своё дело сделал, и терять его скан из-за моргнувшего линка
    // обидно вдвойне. Повторять безопасно: если сервер УСПЕЛ записать, а
    // потерялся только ответ, повтор упрётся в одноразовость токена и вернёт
    // внятный отказ, но НЕ создаст вторую запись.
    if (!s.ok && s.error.length() && s.httpCode == 0) {
        Serial.printf("[cp] сбой связи при отправке кода (%s) — повторяю\n",
                      s.error.c_str());
        uint32_t t0 = millis();
        while (millis() - t0 < 4000 && !eth::linkUp()) delay(100);
        if (eth::linkUp()) s = eth::scan(v.token, code);
    }
#endif

    if (!s.ok) {
        eth::registryLog("error", "Barcode recording failed");
        Serial.printf("[cp] не записано: %s %s\n",
                      s.reason.c_str(), s.error.c_str());
        showFor(pick(s.lcd1, LCD_ERR_1), pick(s.lcd2, s.reason.c_str()),
                LCD_HOLD_MS);
        toIdle();
        return;
    }

    g_records++;
    eth::registryLog("info", "Barcode record saved");
    Serial.printf("[cp] запись №%d\n", s.recordId);
    showFor(pick(s.lcd1, LCD_SAVED_1), pick(s.lcd2, code.c_str()), LCD_HOLD_MS);
    toIdle();
}

}  // namespace

namespace checkpoint {

void begin(GM67 *scanner) {
    g_scanner = scanner;
    g_lastHeartbeat = millis() - HEARTBEAT_MS;

    if (!eth::begin()) {
        show(LCD_NOLINK_1, LCD_NOLINK_2);
        Serial.println("[cp] сеть не поднялась — проверки работать не будут");
    } else {
        String info;
        if (eth::health(info)) {
            Serial.printf("[cp] сервер отвечает: %s\n", info.c_str());
        } else {
            Serial.printf("[cp] ВНИМАНИЕ: сеть есть, но сервер молчит: %s\n",
                          info.c_str());
            Serial.printf("       Проверьте адрес %s:%d, IP платы, маску, шлюз и DNS.\n",
                          ETH_SERVER_HOST, ETH_SERVER_PORT);
#if ETH_DIRECT_AKS
            Serial.println("       Плата обращается прямо в AKS: нужен маршрут "
                           "из общей сети к этому серверу. app.py не требуется.");
#else
            Serial.println("       На ноутбуке должен быть запущен app.py.");
#endif
        }
    }

#if ENABLE_GM67 && POWER_MODE
    // Сразу усыпляем: до первой проверки сканеру делать нечего,
    // а 180 мА он ест независимо от того, горит луч или нет.
    // Усыплять сканер, который так и не ответил на команды, нельзя: команда
    // сна до него, скорее всего, дойдёт (приёмник работает), а разбудить его
    // мы потом не сможем — и ловушка воспроизведётся при каждой загрузке.
    if (g_scanner && g_scanner->configured()) g_scanner->sleep();
#endif

    showIdle();

    // Опрос сервера при загрузке сделан — дальше пост уходит в покой,
    // и здесь он в него встаёт впервые.
#if CAM_OFF_AFTER_CHECK && !INTERBOARD_GATEWAY
    cam::off();
#endif
#if ETH_SLEEP_IDLE
    eth::sleep();
#endif
    g_lastCheckEnd = millis();
#if GM67_PASSIVE_UART_MODE
    Serial.println("[cp] покой: Ethernet активен, камера выключена; GM67 читает самостоятельно");
#else
    Serial.println("[cp] покой: камера и сканер остановлены");
#endif
}

void loop() {
    static uint32_t lastRemotePoll = 0;
    checkDoublePress();
    // ------------------------------------------------------- подсветка ----
    // Гасим не сразу после проверки, а спустя LCD_BACKLIGHT_OFF_MS: вердикт
    // висит на экране LCD_HOLD_MS, и погасить раньше значило бы не дать
    // человеку дочитать.
#if ENABLE_LCD && !LCD_BACKLIGHT_IDLE
    if (g_backlightOn && g_lastCheckEnd &&
        millis() - g_lastCheckEnd > (uint32_t)LCD_BACKLIGHT_OFF_MS) {
        backlight(false);
    }
#endif

    // ------------------------------------------------------ состояние линка --
    // Пока PHY спит, линк спрашивать нечего: его нет по построению, и опрос
    // печатал бы «кабель отключён» каждые пять секунд. Считаем линк
    // условно живым — реальную проверку сделает eth::wake() по кнопке,
    // и вот она уже скажет правду.
    if (eth::asleep()) {
        g_lastLinkUp = true;
    } else if (millis() - g_lastLinkCheck > 5000) {
        // Пост не спит (ETH_SLEEP_IDLE выключён) — тогда пропажу кабеля надо
        // показать сразу: иначе сотрудник будет жать кнопку и не понимать,
        // почему ничего не происходит.
        g_lastLinkCheck = millis();
        bool up = eth::linkUp();
        if (up != g_lastLinkUp) {
            g_lastLinkUp = up;
            if (up) {
                // Настройки могли не пережить пропажу линка: если модулю
                // моргнуло питание, W5500 сбросился и потерял адрес вместе
                // с маской. Выставляем заново — иначе пост останется
                // с адресом 0.0.0.0 при живом кабеле и рабочем SPI.
                eth::reapply();
                Serial.printf("[eth] линк восстановлен, адрес %s\n",
                              eth::localIP().c_str());
                showIdle();
            } else {
                // «Линка нет» бывает по двум несмежным причинам, и путать их
                // дорого: одна лечится втыканием кабеля, другая — снятием
                // питания с модуля. Различаются они ответом по SPI.
                if (eth::hardwarePresent()) {
                    Serial.println("[eth] кабель отключён (микросхема отвечает,"
                                   " линка нет — смотрите RJ45 и порт ноутбука)");
                } else {
                    Serial.println("[eth] микросхема W5500 не отвечает по SPI.");
                    Serial.println("      Это НЕ кабель. Снимите питание с модуля"
                                   " на 5 секунд — как только он вернётся,");
                    Serial.println("      пост подхватит его сам, без"
                                   " перезагрузки: ждите «линк восстановлен».");
                }
                backlight(true);
                show(LCD_NOLINK_2, "");
            }
        }
    }

#if ENABLE_LCD
    lcd::tick(LCD_IDLE_1, LCD_IDLE_2);
#endif
    heartbeatIfDue();

    bool buttonRequested = false;
#if ENABLE_BUTTON
    buttonRequested = btn::pressed();
#endif
    bool remoteRequested = false;
    if (!buttonRequested && millis() - lastRemotePoll >= 2000) {
        lastRemotePoll = millis();
        remoteRequested = eth::takeRemoteTrigger();
        if (remoteRequested) Serial.println("[cp] команда ПРОВЕРИТЬ получена с сайта");
    }
    if (buttonRequested || remoteRequested) {
        // Линк лежит? Не отказываем сразу: на живом железе он может быть
        // в секундной яме (автопереговоры, дрогнувший контакт), и человек
        // не должен получать «связи нет» из-за мгновения. Ждём до 4 секунд —
        // экран честно показывает «Gözləyin», — и только потом отказ.
        if (!g_lastLinkUp) {
            backlight(true);
            show(LCD_WAIT_2, "");
            uint32_t t0 = millis();
            while (millis() - t0 < 4000 && !eth::linkUp()) delay(100);
            g_lastLinkUp = eth::linkUp();
            if (g_lastLinkUp) {
                Serial.println("[cp] линк вернулся за время ожидания — начинаю");
            }
        }
        if (!g_lastLinkUp) {
            showFor(LCD_NOLINK_1, LCD_NOLINK_2, LCD_HOLD_MS);
        } else {
            runCheck();
        }
        return;
    }

    // Спим до следующего нажатия. В POWER_MODE < 2 это обычная пауза,
    // в режиме 2 — настоящий лёгкий сон с пробуждением по кнопке.
    power::sleepUntilButton(BUTTON_PIN, 2000);
}

}  // namespace checkpoint

#endif  // ENABLE_ETH
