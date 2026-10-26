// ===========================================================================
//  Драйвер сканера GROW GM67 по последовательному интерфейсу (протокол SSI)
//
//  Формат кадра команды (Приложение 6 руководства GM67):
//     [LEN][OPCODE][SOURCE][STATUS][ ...данные... ][CKSUM_HI][CKSUM_LO]
//
//     LEN     — длина кадра БЕЗ двух байт контрольной суммы, включая сам LEN
//     SOURCE  — 0x04 = «хост»
//     STATUS  — 0x00 временно (до перезагрузки), 0x08 записать во флеш
//     CKSUM   — дополнение до двух от суммы всех предыдущих байт:
//                   cksum = 0x10000 - sum(bytes)
//               Проверка на примере из руководства:
//                   CMD_ACK = 04 D0 04 00 FF 28
//                   sum = 0x04+0xD0+0x04+0x00 = 0xD8
//                   0x10000 - 0xD8 = 0xFF28   ✓
//
//  Ключевые факты из руководства, на которых построена логика ниже:
//   * По умолчанию: 9600 8N1, режим Continuous, ACK/NAK разрешён.
//   * «To start decoding and stop decoding the serial port command needs to be
//      valid in host mode» — START_DECODE/STOP_DECODE работают ТОЛЬКО
//      в Host Mode. Поэтому при старте переводим сканер в Host Mode.
//   * «When the scanner is not working, it is in sleep mode. Under the sleep
//      mode, need to wake up first (0x00, 50ms), then send the effective
//      command» — перед каждой командой шлём байт 0x00 и ждём 50 мс.
//   * Вывод данных переключаем в «оригинальный» ASCII (параметр 0xEE = 0x00)
//     и добавляем терминатор CR LF — так парсинг на ESP32 тривиален.
//   * ACK/NAK отключаем (параметр 0x9F = 0x00), иначе сканер ждёт от нас
//     подтверждение на каждый декод.
// ===========================================================================
#pragma once

#include <Arduino.h>

class GM67 {
public:
    // Готовые кадры из Приложения 6 руководства (для справки и самопроверки)
    static const uint8_t CMD_ACK[6];        // 04 D0 04 00 FF 28
    static const uint8_t START_DECODE[6];   // 04 E4 04 00 FF 14
    static const uint8_t STOP_DECODE[6];    // 04 E5 04 00 FF 13
    static const uint8_t SCAN_ENABLE[6];    // 04 E9 04 00 FF 0F
    static const uint8_t SCAN_DISABLE[6];   // 04 EA 04 00 FF 0E
    static const uint8_t LED_ON[7];         // 05 E7 04 00 01 FF 0F
    static const uint8_t LED_OFF[7];        // 05 E8 04 00 01 FF 0E
    static const uint8_t SLEEP_CMD[6];      // 04 EB 04 00 FF 0D
    static const uint8_t RESET_CMD[6];      // 04 FA 04 00 FE FE

    // --- параметры (номер параметра из таблицы 6-2) ---
    static const uint8_t P_TRIGGER_MODE = 0x8A;   // 0x08 = Host Mode
    static const uint8_t P_DECODE_FMT   = 0xEE;   // 0x00 = сырой ASCII
    static const uint8_t P_ACKNAK       = 0x9F;   // 0x00 = запретить ACK/NAK
    static const uint8_t P_BAUD         = 0x9C;   // 0x06 = 9600, 0x0A = 115200
    static const uint8_t P_BUZZ_VOL     = 0x8C;   // 0x00 громко .. 0x02 тихо
    static const uint8_t P_SCAN_DUR     = 0x88;   // длительность одного скана

    // расширенные параметры (двухбайтные, с префиксом 0xF2)
    static const uint8_t PX_PREFIX      = 0xF2;
    static const uint8_t PX_TERMINATOR  = 0x05;   // 0x01 = CR LF
    static const uint8_t PX_SILENT      = 0x0C;   // 0x01 = беззвучно
    static const uint8_t PX_STARTUP_SND = 0x0D;   // 0x00 = без звука при старте
    static const uint8_t PX_SINGLE_SCAN = 0xFA;   // 0x05 = 5 секунд

    void begin(int uartNum, int rxPin, int txPin, uint32_t baud);

    // Режим чтения:
    //   CONTINUOUS — сканер светит и читает сам, без команд. Нужен здесь:
    //                штрихкоды ловятся параллельно со съёмкой фотографий.
    //   HOST       — луч включается только командой startDecode(). Нужен там,
    //                где сканировать можно лишь после какого-то условия
    //                (например, после распознавания лица на КПП).
    enum class Mode { CONTINUOUS, HOST };

    // Разово записать рабочую конфигурацию. persist=true пишет во флеш сканера.
    bool configure(Mode mode, bool persist, bool use115200);

    // Удалась ли последняя настройка. Нужно вызывающему, чтобы отличить
    // «сканер мёртв» от «сканер в неизвестном режиме»: во втором случае
    // настройку имеет смысл повторить перед работой, а не списывать узел
    // до перезагрузки. GM67 после подачи питания поднимается около двух
    // секунд, и проверка при старте платы бывает просто РАНЬШЕ него.
    bool configured() const { return _configOk; }

    void wakeup();                    // 0x00 + 50 мс, см. Приложение 6

    // --- питание сканера ---------------------------------------------------
    // GM67 потребляет около 180 мА и делает это КРУГЛОСУТОЧНО, даже когда
    // луч погашен: спит только по явной команде. На посту, где проверка
    // бывает раз в несколько минут, это самый крупный потребитель во всей
    // схеме — больше, чем ESP32 с камерой вместе взятые.
    //
    // sleep() шлёт SLEEP_CMD, wake() будит байтом 0x00 (Приложение 6
    // руководства: «Under the sleep mode, need to wake up first»).
    void sleep();
    bool wake();
    // Жёсткое пробуждение: три байта 0x00 с паузами. Нужно, когда неизвестно,
    // спит модуль или нет — например, сразу после загрузки платы, которая
    // сканер не сбрасывает (питание у него своё). Подробности в gm67.cpp.
    void wakeHard();
    bool asleep() const { return _asleep; }
    bool startDecode();               // включить луч, начать поиск кода
    bool stopDecode();                // погасить луч
    void flushInput();

    // Забрать декодированную строку, если она пришла. Терминатор CR LF.
    // Возвращает true и заполняет out, когда строка собрана целиком.
    bool poll(String &out, uint32_t interCharTimeoutMs = 60);

    // Минимальная длина строки, которую считаем настоящим штрихкодом.
    // Всё короче — обрывок служебного кадра сканера, см. acceptable().
    void setMinLength(int n) { _minLen = n < 1 ? 1 : n; }

    // Окно тишины после включения луча. 0 = выключено, и это правильное
    // значение по умолчанию: окно съедало настоящие коды у тех, кто поднёс
    // штрихкод заранее. Подробный разбор — в gm67.cpp у acceptable().
    void setGuardMs(uint32_t ms) { _guardMs = ms; }

    // Пусто ли в приёмном тракте. Нужно перед перевзводом чтения:
    // startDecode() чистит буфер и посреди приходящего кода потеряет его.
    bool idle() const;

    // Режим отладки: печатать в Serial КАЖДЫЙ принятый байт в hex.
    // Нужен, когда сканер пищит и читает, а на плату ничего не приходит:
    // сразу видно, идут ли байты вообще, и не мусор ли это из-за баррейта.
    void setDebug(bool on) { _debug = on; }

    // Сколько байт всего пришло от сканера с момента включения.
    // Если 0 — данные не доходят физически (провода, уровни, режим USB).
    uint32_t rxBytes() const { return _rxBytes; }
    // Сколько байт было отброшено как непечатные (признак неверного баррейта
    // или того, что сканер шлёт SSI-пакеты вместо сырого текста).
    uint32_t droppedBytes() const { return _dropped; }

    // Низкий уровень — открыты, чтобы можно было дослать что-то своё
    void sendRaw(const uint8_t *data, size_t len);
    bool sendParam1(uint8_t param, uint8_t value, bool persist);
    bool sendParam2(uint8_t prefix, uint8_t sub, uint8_t value, bool persist);

    // Собрать кадр с корректной контрольной суммой. Возвращает длину.
    static size_t buildFrame(uint8_t *buf, const uint8_t *payload, size_t payloadLen);
    static uint16_t checksum(const uint8_t *data, size_t len);

    HardwareSerial *port() { return _ser; }

private:
    HardwareSerial *_ser = nullptr;
    String   _acc;                 // накопитель символов кода
    uint32_t _lastByteMs = 0;
    bool     _debug   = false;
    uint32_t _rxBytes = 0;
    uint32_t _dropped = 0;
    int      _minLen  = 4;         // короче — служебный мусор, не код
    uint32_t _guardMs = 0;         // окно тишины после включения луча (0 = нет)
    uint32_t _beamOnMs = 0;        // когда луч включили (0 = выключен)
    bool     _asleep  = false;
    bool     _configOk = false;   // удалась ли последняя configure()

    bool waitAck(uint32_t timeoutMs = 300);
    void swallowAck(uint32_t timeoutMs = 120);
    bool acceptable(const String &s) const;
};
