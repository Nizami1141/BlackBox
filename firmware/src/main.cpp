// ===========================================================================
//  ESP32-S3: терминал пограничного поста
//
//  Плата — тонкий клиент. Она не считает эмбеддинги и не хранит базу лиц:
//  и то, и другое живёт на сервере (почему — docs/ARCHITECTURE.md §2).
//
//  ---------------------------------------------------------------------
//  Два транспорта, ровно один включён (config.h)
//  ---------------------------------------------------------------------
//
//  ENABLE_BLE 1 — стендовый режим. Ноутбук рядом, инициатива у сервера:
//      он шлёт "VERIFY", "SCAN ON", "SCAN OFF", а плата исполняет.
//      Так отлаживали всё до появления Ethernet.
//
//  ENABLE_ETH 1 — боевой режим границы. Ни Bluetooth, ни Wi-Fi там не
//      работают. Инициатива у платы: нажали кнопку — проснулась, сняла
//      кадр, сходила к серверу двумя HTTP-запросами, показала вердикт
//      на экране и уснула обратно. Весь сценарий — в checkpoint.cpp.
//
//  Главная гарантия системы в обоих режимах одна и та же: сканер физически
//  не может прочитать штрихкод, пока сервер не опознал лицо и не выдал
//  одноразовый токен. В Host Mode луч включается только командой, а запись
//  в журнал сервер делает исключительно по действующему токену.
// ===========================================================================

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdarg.h>

#include "button.h"
#include "camera.h"
#include "checkpoint.h"
#include "config.h"
#include "gm67.h"
#include "interboard_link.h"
#include "lcd.h"
#include "power.h"

#if ENABLE_BLE
#include "ble_photo.h"
#endif

#if ENABLE_WIFI
#include <WiFi.h>
#include "stream.h"
#include "wifi_setup.h"
#endif

// ===========================================================================
//  Лог в ОБА последовательных интерфейса сразу
//
//  У ESP32-S3 два пути наружу: аппаратный UART0 (пины 43/44, обычно идёт на
//  чип-переходник USB-UART) и нативный USB самого чипа. Какой из них станет
//  вашим COM-портом — зависит от платы и от того, в какой разъём воткнут
//  кабель. Флаг ARDUINO_USB_CDC_ON_BOOT переключает, куда смотрит «Serial»:
//
//      CDC_ON_BOOT=0 :  Serial = UART0,      нативный USB = USBSerial
//      CDC_ON_BOOT=1 :  Serial = нативный USB, UART0      = Serial0
//
//  Чтобы не гадать, на какой порт вы смотрите, важные строки печатаются
//  сразу в оба. Лишний вывод в неподключённый интерфейс просто теряется.
// ===========================================================================
#if ARDUINO_USB_CDC_ON_BOOT
  #define ALT_SERIAL Serial0
#elif ARDUINO_USB_MODE
  extern HWCDC USBSerial;
  #define ALT_SERIAL USBSerial
#endif

static void altBegin() {
#ifdef ALT_SERIAL
  #if ARDUINO_USB_CDC_ON_BOOT
    ALT_SERIAL.begin(SERIAL_BAUD, SERIAL_8N1, 44, 43);   // UART0 на своих пинах
  #else
    ALT_SERIAL.begin();                                   // нативный USB
  #endif
#endif
}

static void logf(const char *fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
#ifdef ALT_SERIAL
    ALT_SERIAL.print(buf);
#endif
}

// Печатает состояние памяти. Строка осталась со времён, когда плата падала
// на BLEDevice::init(): Bluedroid требует около 70 КБ ВНУТРЕННЕЙ DRAM,
// и если её мало, падение происходит без внятного сообщения.
// В режиме Ethernet Bluedroid не поднимается вовсе — отсюда сразу
// освобождается около 70 КБ.
// ===========================================================================
//  Почему плата перезагрузилась в прошлый раз
//
//  Строка нужна ровно для одного: чтобы не искать в программе то, что лежит
//  в питании. При просадке напряжения ESP32 сбрасывает себя сам, и в мониторе
//  это выглядит как обычная загрузка — плата просто «начинает сначала»
//  посреди отправки кадра. Отличить это от аварии в коде на глаз невозможно,
//  а чип помнит причину и готов её назвать.
// ===========================================================================
static void printResetReason() {
    esp_reset_reason_t r = esp_reset_reason();
    const char *s;
    switch (r) {
        case ESP_RST_POWERON:   s = "подано питание";                       break;
        case ESP_RST_EXT:       s = "внешний сброс (кнопка RST)";           break;
        case ESP_RST_SW:        s = "программная перезагрузка";             break;
        case ESP_RST_PANIC:     s = "АВАРИЯ в программе (ищите backtrace)"; break;
        case ESP_RST_INT_WDT:   s = "сторожевой таймер прерываний";         break;
        case ESP_RST_TASK_WDT:  s = "сторожевой таймер задачи";             break;
        case ESP_RST_WDT:       s = "сторожевой таймер";                    break;
        case ESP_RST_BROWNOUT:  s = "ПРОСАДКА ПИТАНИЯ (brownout)";          break;
        case ESP_RST_DEEPSLEEP: s = "выход из глубокого сна";               break;
        default:                s = "неизвестно";                           break;
    }
    logf("[boot] причина прошлого сброса: %s\n", s);

    if (r == ESP_RST_BROWNOUT) {
        logf("       Это ПИТАНИЕ, а не программа. Напряжение просело ниже\n");
        logf("       порога, и чип сбросил себя сам — скорее всего в момент\n");
        logf("       отправки кадра, когда ток максимален.\n");
        logf("       Порядок действий, от бесплатного к платному:\n");
        logf("         1) короткий толстый USB-кабель, без удлинителей и хабов\n");
        logf("         2) порт USB 3.0 (синий) — он даёт 900 мА вместо 500\n");
        logf("         3) электролит 100-470 мкФ по питанию у W5500\n");
        logf("         4) отдельный БП 5 В на периферию (docs/ETHERNET_BRINGUP.md)\n");
    }
}

static void printMem(const char *stage) {
    logf("[mem] %-16s heap=%6u  внутр.DRAM=%6u  PSRAM=%7u/%7u\n",
         stage,
         (unsigned)ESP.getFreeHeap(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)ESP.getFreePsram(),
         (unsigned)ESP.getPsramSize());
}

// ===========================================================================
//  Сканер
// ===========================================================================
#if ENABLE_GM67
static GM67 g_scanner;

#if GM67_SELFTEST
// Проверка «сам себе»: шлём строку в TX и пытаемся принять её же на RX.
// Требует перемычки между GM67_TX_PIN и GM67_RX_PIN (сканер отключить).
// Единственный способ разделить «не работает плата» и «не работает сканер»
// не имея осциллографа.
static void uartSelfTest() {
    HardwareSerial *s = g_scanner.port();
    if (!s) return;
    while (s->available()) s->read();

    const char *probe = "LOOPBACK-OK";
    s->print(probe);
    s->flush();

    String got;
    uint32_t t0 = millis();
    while (millis() - t0 < 400) {
        while (s->available()) got += (char)s->read();
        delay(5);
    }

    logf("[test] отправлено \"%s\", принято \"%s\"\n", probe, got.c_str());
    if (got.indexOf(probe) >= 0) {
        logf("[test] САМОПРОВЕРКА ПРОЙДЕНА: пины %d/%d и UART%d исправны.\n",
             GM67_RX_PIN, GM67_TX_PIN, GM67_UART_NUM);
        logf("       Значит дело в сканере или в проводке до него.\n");
    } else {
        logf("[test] САМОПРОВЕРКА ПРОВАЛЕНА.\n");
        logf("       Либо не поставлена перемычка между GPIO %d и GPIO %d,\n",
             GM67_RX_PIN, GM67_TX_PIN);
        logf("       либо эти пины заняты чем-то ещё — возьмите другие.\n");
    }
}
#endif  // GM67_SELFTEST
#endif  // ENABLE_GM67

// ===========================================================================
//  РЕЖИМ BLE: сервер командует, плата исполняет
//  (в режиме Ethernet весь этот блок не компилируется)
// ===========================================================================
#if ENABLE_BLE

static TaskHandle_t g_photoTask = nullptr;

#if ENABLE_GM67
static String   g_lastCode;
static uint32_t g_lastCodeMs = 0;
static bool     g_scanActive  = false;
static uint32_t g_scanStartMs = 0;
static uint32_t g_rearmMs     = 0;    // когда в последний раз взводили чтение
static uint32_t g_codesSent   = 0;

// Луч включается ТОЛЬКО по команде "SCAN ON" с сервера, то есть после того,
// как сервер узнал лицо и выдал session token.
static void applyScannerState() {
    bool want = blep::scannerWanted();

    // Страховка от «сервер отвалился и не погасил луч».
    if (want && g_scanActive &&
        (millis() - g_scanStartMs) > (uint32_t)GM67_SCAN_WINDOW_MS) {
        Serial.println("[gm67] окно сканирования истекло — гашу луч сам");
        blep::setScannerWanted(false);
        want = false;
    }
    if (want == g_scanActive) return;

    if (want) {
        g_scanner.wake();
        g_scanner.startDecode();
        g_scanStartMs = millis();
        g_rearmMs     = millis();
        g_lastCode = "";                 // новая проверка — забываем прошлый код
        Serial.println("[gm67] луч ВКЛЮЧЁН, жду штрихкод груза");
    } else {
        g_scanner.stopDecode();
        g_scanner.flushInput();          // выбрасываем недочитанный хвост
#if POWER_MODE
        g_scanner.sleep();
#endif
        Serial.println("[gm67] луч выключен");
    }
    g_scanActive = want;
}

static void pollScanner() {
    String code;
    if (!g_scanner.poll(code)) return;
    code.trim();
    if (code.length() == 0) return;

    uint32_t now = millis();
    if (code == g_lastCode &&
        (now - g_lastCodeMs) < (uint32_t)GM67_SAME_CODE_IGNORE_S * 1000) {
        return;
    }
    g_lastCode   = code;
    g_lastCodeMs = now;

    Serial.printf("[gm67] прочитан код: \"%s\"\n", code.c_str());
    if (blep::sendBarcode(code)) {
        g_codesSent++;
    } else {
        Serial.println("[gm67] компьютер не подключён — код показан только "
                       "в логе и НИКУДА не записан");
    }
}

// ---------------------------------------------------------------- перевзвод
//
// Зачем это вообще нужно. В Host Mode команда START_DECODE запускает ОДИН
// сеанс чтения, а не включает луч навсегда. Сеанс заканчивается в двух
// случаях: сканер прочитал код, либо истекла длительность одиночного скана
// (параметр PX_SINGLE_SCAN, у нас 5 секунд).
//
// А ждём мы штрихкод до 45 секунд. То есть через пять секунд после команды
// SCAN ON сканер тихо переставал читать, и оставшиеся сорок секунд плата
// ждала того, чего уже не могло произойти. Проверка доходила до таймаута,
// и в журнале не появлялось ничего — при том, что и сканер, и связь были
// полностью исправны.
//
// Тот же механизм чинит и второй случай: если прочитанная строка не прошла
// отбор (обрывок служебного кадра), сеанс чтения уже потрачен. Без
// перевзвода единственная попытка сотрудника пропала бы впустую.
//
// Перевзводим только когда в приёмном тракте пусто: startDecode() чистит
// буфер, и посреди приходящего кода это его бы и потеряло.
static void rearmScanner() {
    if (!g_scanActive) return;
    if (millis() - g_rearmMs < (uint32_t)GM67_REARM_MS) return;
    if (!g_scanner.idle()) return;      // код прямо сейчас приходит — не мешаем

    g_rearmMs = millis();
    g_scanner.startDecode();
}
#endif  // ENABLE_GM67

// В режиме КПП читаем сканер только когда луч включён: байты, пришедшие
// при выключенном луче, — хвост прошлого скана, и относить их к новой
// проверке нельзя.
static void scannerStep() {
#if ENABLE_GM67
    applyScannerState();
    if (!g_scanActive) return;
    pollScanner();
    rearmScanner();
#endif
}

static void lcdStep() {
#if ENABLE_LCD
    // Строки от сервера кладутся в буфер из BLE-колбэка; рисуем их здесь,
    // в обычной задаче — I2C из колбэка Bluedroid трогать нельзя.
    String l1, l2;
    uint32_t hold = 0;
    if (blep::takeLcdUpdate(l1, l2, hold)) {
        if (hold) lcd::showFor(l1, l2, hold);
        else      lcd::show(l1, l2);
    }
    lcd::tick(LCD_IDLE_1, LCD_IDLE_2);
#endif
}

static void buttonStep() {
#if ENABLE_BUTTON
    if (btn::longPressed()) {
        Serial.println("[btn] долгое нажатие — отмена");
        blep::setScannerWanted(false);
        blep::sendEvent("CANCEL");
#if ENABLE_LCD
        lcd::showFor(LCD_CANCEL_1, "", 1500);
#endif
        return;
    }
    if (!btn::pressed()) return;

    Serial.println("[btn] нажатие — прошу сервер начать проверку");
    if (blep::sendEvent("BUTTON")) {
#if ENABLE_LCD
        lcd::show(LCD_WAIT_1, LCD_WAIT_2);
#endif
    } else {
        Serial.println("[btn] сервер не подключён");
#if ENABLE_LCD
        lcd::showFor(LCD_NOLINK_1, LCD_NOLINK_2, LCD_HOLD_MS);
#endif
    }
#endif
}

// Отдельная задача, потому что передача одного кадра по BLE занимает
// секунду и больше. В loop() такое держать нельзя — встанет всё остальное.
static void photoTask(void *) {
    uint32_t lastShot = 0;

    for (;;) {
        scannerStep();
        buttonStep();
        lcdStep();

        int q = blep::pendingQuality();
        if (q) {
            cam::setQuality(q);
            Serial.printf("[cam] качество JPEG -> %d\n", q);
        }

        bool byTimer  = blep::photoEnabled() &&
                        (millis() - lastShot >= blep::photoInterval());
        bool byButton = blep::takeShotRequested();

        if ((byTimer || byButton) && blep::isReady()) {
            size_t   len = 0;
            uint16_t w = 0, h = 0;
            // snapshotOnce: разбудить -> снять -> усыпить обратно.
            uint8_t *jpg = cam::snapshotOnce(&len, &w, &h);
            if (jpg) {
                blep::sendPhoto(jpg, len, w, h);
                free(jpg);
            } else {
                Serial.println("[photo] не удалось получить кадр");
            }
            // Кадр отправлен, работа закончена — можно снижать частоту.
            power::idle();
            lastShot = millis();
        } else if (byTimer) {
            lastShot = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif  // ENABLE_BLE

// ===================================================================== setup
void setup() {
    Serial.begin(SERIAL_BAUD);
    altBegin();
    delay(800);              // дать хосту время открыть порт после сброса

#if ENABLE_ETH
    logf("\n\n=== Пост \"%s\": автономный режим, связь по Ethernet ===\n",
         CHECKPOINT_DEVICE_ID);
#elif ENABLE_BLE
    logf("\n\n=== Пост \"%s\": стендовый режим, связь по Bluetooth ===\n",
         CHECKPOINT_DEVICE_ID);
#else
    logf("\n\n=== Пост \"%s\": транспорт не выбран ===\n", CHECKPOINT_DEVICE_ID);
#endif
    logf("[boot] сборка %s %s\n", __DATE__, __TIME__);
#if ARDUINO_USB_CDC_ON_BOOT
    logf("[boot] Serial: встроенный USB; DEBUG_CHECK через USB COM-порт\n");
#else
    logf("[boot] Serial: UART0/CH343; DEBUG_CHECK через USB-UART COM-порт\n");
#endif

    // PSRAM обязательна. Если здесь 0 — почти наверняка неверный тип памяти
    // в platformio.ini (qio_opi для Octal против qio_qspi для Quad).
    logf("[boot] PSRAM: %s, размер %u байт\n",
         psramFound() ? "найдена" : "НЕ НАЙДЕНА", (unsigned)ESP.getPsramSize());
    printResetReason();
    printMem("старт");

    power::begin();

#if ENABLE_LCD
    if (lcd::begin(LCD_SDA_PIN, LCD_SCL_PIN, LCD_I2C_ADDR)) {
        logf("[boot] экран найден на адресе 0x%02X (SDA=%d, SCL=%d)\n",
             lcd::address(), LCD_SDA_PIN, LCD_SCL_PIN);
        lcd::show("Yuklenir...", "");
    } else {
        logf("[boot] ЭКРАН НЕ ОТВЕЧАЕТ по I2C (SDA=%d, SCL=%d).\n",
             LCD_SDA_PIN, LCD_SCL_PIN);
        logf("       Проверьте: 1) не перепутаны ли SDA и SCL\n");
        logf("                  2) подано ли 5 В на адаптер HW-61\n");
        logf("                  3) стоит ли преобразователь уровней\n");
        logf("       Если экран светится, но пустой — крутите подстроечник\n");
        logf("       контраста, это отдельная от I2C проблема.\n");
        logf("       На работу сканера и камеры это НЕ влияет: обращения\n");
        logf("       к ненайденному экрану пропускаются, время не тратится.\n");
    }
#endif

#if ENABLE_BUTTON
    btn::begin(BUTTON_PIN, true);
    logf("[boot] кнопка на GPIO %d (второй вывод на GND)\n", BUTTON_PIN);
    if (btn::isDown()) {
        logf("[boot] ВНИМАНИЕ: кнопка выглядит нажатой прямо сейчас.\n");
        logf("       Скорее всего пин замкнут на землю постоянно.\n");
    }
#endif

#if ENABLE_CAMERA
    if (!cam::begin()) logf("[!] камера не инициализирована\n");
    logf("[boot] план питания камеры: %s\n",
         CAM_POWER_PLAN == 2 ? "сон между кадрами, полное выключение после простоя"
                             : (CAM_POWER_PLAN == 1 ? "сон между кадрами"
                                                    : "включена всегда"));
    logf("[boot] температура кристалла: %.1f C\n", cam::chipTemp());
    printMem("после камеры");
#else
    logf("[boot] камера ОТКЛЮЧЕНА флагом ENABLE_CAMERA\n");
#endif

#if ENABLE_WIFI
    wifis::begin();
    stream::begin();
    printMem("после Wi-Fi");
#endif

#if ENABLE_GM67
    logf("[boot] сканер GM67 на UART%d (RX=%d, TX=%d, %d бод)...\n",
         GM67_UART_NUM, GM67_RX_PIN, GM67_TX_PIN, GM67_BAUD);
    g_scanner.begin(GM67_UART_NUM, GM67_RX_PIN, GM67_TX_PIN, GM67_BAUD);
    g_scanner.setDebug(GM67_DEBUG_RAW);
    g_scanner.setMinLength(GM67_MIN_CODE_LEN);
    g_scanner.setGuardMs(GM67_GUARD_MS);

#if GM67_SELFTEST
    logf("[test] режим самопроверки UART (перемычка GPIO %d <-> GPIO %d)\n",
         GM67_RX_PIN, GM67_TX_PIN);
    uartSelfTest();
#endif

#if ENABLE_ETH && GM67_PASSIVE_UART_MODE
    // Приём без команд: предыдущий тест подтвердил приём штрихкода по GPIO1,
    // но сканер не подтвердил ни одной команды по GPIO2. Не меняем его режим
    // и не отправляем STOP_DECODE при загрузке — иначе можно погасить луч.
    g_scanner.flushInput();
    logf("[boot] GM67: пассивный UART, команд нет; сканер должен читать сам (Continuous mode)\n");
#else
    // Host Mode: луч включается только командой, то есть только после того,
    // как сервер опознал лицо. Ответ сканера на команду важен сам по себе —
    // он означает, что работают ОБА направления UART.
    // Три попытки с паузой. GM67 после подачи питания поднимается около двух
    // секунд, а мы доходим сюда примерно через полторы — одиночная проверка
    // регулярно била мимо и печатала «сканер НЕ ответил» на исправном железе.
    // Один раз это уже вводило в заблуждение: боевое чтение штрихкода прошло
    // сразу после такой жалобы.
    bool cfgOk = false;
    for (int attempt = 1; attempt <= 3 && !cfgOk; attempt++) {
        // Состояние сканера при загрузке неизвестно: плата перезагружается,
        // а он — нет. Будим жёстко перед каждой попыткой.
        g_scanner.wakeHard();
        cfgOk = g_scanner.configure(GM67::Mode::HOST,
                                    GM67_WRITE_CONFIG_TO_FLASH, false);
        if (!cfgOk && attempt < 3) {
            logf("[boot] сканер молчит (попытка %d из 3), жду и повторяю...\n",
                 attempt);
            delay(700);
        }
    }
    g_scanner.stopDecode();
    if (cfgOk) {
        logf("[boot] сканер ответил на команды — связь по UART в обе стороны есть\n");
    } else {
        logf("[boot] ВНИМАНИЕ: сканер НЕ ответил на команды.\n");
        logf("       Проверьте: 1) не перепутаны ли TX и RX (они перекрещиваются)\n");
        logf("                  2) есть ли общая земля GND\n");
        logf("                  3) подано ли на сканер 5 В\n");
        logf("       Если сканер при этом пищит и читает — он, скорее всего,\n");
        logf("       переключён в режим USB и по UART не говорит. Отсканируйте\n");
        logf("       QR-код Factory default setting со стр. 7 его даташита.\n");
    }
#endif
    printMem("после GM67");
#else
    logf("[boot] сканер ОТКЛЮЧЕН флагом ENABLE_GM67\n");
#endif

    // --------------------------------------------------------- транспорт ---
#if ENABLE_INTERBOARD
    if (!interboard::begin(
#if INTERBOARD_CAMERA_NODE
            interboard::Role::CAMERA_NODE
#else
            interboard::Role::GATEWAY
#endif
        )) {
        logf("[!] связь между двумя ESP32 не запустилась\n");
    }
#endif

#if ENABLE_ETH
    checkpoint::begin(
#if ENABLE_GM67
        &g_scanner
#else
        nullptr
#endif
    );
    printMem("после Ethernet");
#elif ENABLE_BLE
    logf("[boot] поднимаю BLE...\n");
    blep::begin(BLE_DEVICE_NAME);
    printMem("после BLE");
    // Задача фото на ядре 0: ядро 1 занято сетью.
    xTaskCreatePinnedToCore(photoTask, "photo", 8192, nullptr, 3, &g_photoTask, 0);
#endif

    logf("[ok] загрузка завершена, процессор на %u МГц\n", power::cpuMhz());
}

// ====================================================================== loop
void loop() {
#if INTERBOARD_CAMERA_NODE
    interboard::loop();

    uint16_t session = 0;
    if (interboard::takeCaptureRequest(session)) {
        logf("[board-link] запрос кадра, сессия %u\n", session);
        power::active();
        size_t len = 0;
        uint16_t w = 0, h = 0;
        uint8_t *jpg = cam::snapshotOnce(&len, &w, &h);
        if (!jpg) {
            logf("[board-link] камера не дала кадр\n");
            interboard::sendCaptureError(session, "камера OV5640 не дала кадр");
        } else {
            interboard::sendFrame(session, jpg, len, w, h);
            free(jpg);
        }
#if CAM_OFF_AFTER_CHECK
        cam::off();
#endif
        power::idle();
    }
    cam::tick();
    delay(2);

#elif ENABLE_ETH
    interboard::loop();
    // Весь сценарий автономного поста — в checkpoint.cpp.
    checkpoint::loop();

#elif ENABLE_BLE
    static uint32_t lastStatus = 0;
    static uint32_t lastHint = 0;

    // Переводит камеру из лёгкого сна в полное выключение после простоя.
    cam::tick();

    if (millis() - lastHint >= 10000) {
        lastHint = millis();
        logf("[ble] %s | кадров: %lu | штрихкодов: %lu\n",
             blep::isReady() ? "компьютер подключён"
                             : "жду подключения компьютера (имя " BLE_DEVICE_NAME ")",
             (unsigned long)blep::framesSent(),
             (unsigned long)blep::barcodesSent());
        // Главная строка для ответа на вопрос «греется или нет». Смотреть
        // надо не на абсолютное число, а на то, растёт оно или стоит:
        // датчик грубый, но динамику показывает честно.
        logf("[cam] %s | снимков: %lu | отбраковано: %lu | последний: %u байт "
             "| камера работала: %lu с | кристалл: %.1f C\n",
             cam::stateName(), (unsigned long)cam::shotsTaken(),
             (unsigned long)cam::rejected(), (unsigned)cam::lastLen(),
             (unsigned long)(cam::activeMsTotal() / 1000), cam::chipTemp());
#if ENABLE_LCD
        // Растущее число ошибок = шина работает на грани. Это не только
        // про экран: пока обмен упирается в таймауты, стоит вся задача,
        // включая опрос сканера. См. src/lcd.cpp, expanderWrite().
        if (lcd::errors()) {
            logf("[lcd] %s | ошибок шины: %lu\n",
                 lcd::ok() ? "работает" : "ОТКЛЮЧЁН из-за сбоев",
                 (unsigned long)lcd::errors());
        }
#endif
#if ENABLE_GM67
        logf("[gm67] байт принято: %lu (из них непечатных: %lu)\n",
             (unsigned long)g_scanner.rxBytes(),
             (unsigned long)g_scanner.droppedBytes());
#endif
    }

    if (millis() - lastStatus >= STATUS_PERIOD_MS) {
        lastStatus = millis();
#if ENABLE_GM67
        bool scanOn = g_scanActive;
#else
        bool scanOn = false;
#endif
        String js = String("{\"device_id\":\"") + CHECKPOINT_DEVICE_ID + "\"" +
                    ",\"mode\":\"ble\"" +
                    ",\"scan\":"   + (scanOn ? "true" : "false") +
                    ",\"photo\":"  + (blep::photoEnabled() ? "true" : "false") +
                    ",\"sent\":"   + String(blep::framesSent()) +
                    ",\"codes\":"  + String(blep::barcodesSent()) +
                    ",\"heap\":"   + String(ESP.getFreeHeap() / 1024) +
                    ",\"cpu\":"    + String(power::cpuMhz()) +
                    ",\"temp\":"   + String(cam::chipTemp(), 1) +
                    ",\"cam\":\""   + cam::stateName() + "\"}";
        blep::sendStatus(js);
    }
    delay(20);

#else
    delay(1000);
#endif
}
