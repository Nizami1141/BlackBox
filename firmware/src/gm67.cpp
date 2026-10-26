#include "gm67.h"

#include "config.h"   // GM67_ALLOW_SLEEP

// ---- готовые кадры из Приложения 6 руководства GM67 -----------------------
const uint8_t GM67::CMD_ACK[6]      = {0x04, 0xD0, 0x04, 0x00, 0xFF, 0x28};
const uint8_t GM67::START_DECODE[6] = {0x04, 0xE4, 0x04, 0x00, 0xFF, 0x14};
const uint8_t GM67::STOP_DECODE[6]  = {0x04, 0xE5, 0x04, 0x00, 0xFF, 0x13};
const uint8_t GM67::SCAN_ENABLE[6]  = {0x04, 0xE9, 0x04, 0x00, 0xFF, 0x0F};
const uint8_t GM67::SCAN_DISABLE[6] = {0x04, 0xEA, 0x04, 0x00, 0xFF, 0x0E};
const uint8_t GM67::LED_ON[7]       = {0x05, 0xE7, 0x04, 0x00, 0x01, 0xFF, 0x0F};
const uint8_t GM67::LED_OFF[7]      = {0x05, 0xE8, 0x04, 0x00, 0x01, 0xFF, 0x0E};
const uint8_t GM67::SLEEP_CMD[6]    = {0x04, 0xEB, 0x04, 0x00, 0xFF, 0x0D};
const uint8_t GM67::RESET_CMD[6]    = {0x04, 0xFA, 0x04, 0x00, 0xFE, 0xFE};

// ---------------------------------------------------------------- checksum
uint16_t GM67::checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint16_t)((0x10000UL - (sum & 0xFFFF)) & 0xFFFF);
}

size_t GM67::buildFrame(uint8_t *buf, const uint8_t *payload, size_t payloadLen) {
    memcpy(buf, payload, payloadLen);
    uint16_t ck = checksum(payload, payloadLen);
    buf[payloadLen]     = (uint8_t)(ck >> 8);
    buf[payloadLen + 1] = (uint8_t)(ck & 0xFF);
    return payloadLen + 2;
}

// ------------------------------------------------------------------- begin
void GM67::begin(int uartNum, int rxPin, int txPin, uint32_t baud) {
    static HardwareSerial serial1(1);
    static HardwareSerial serial2(2);
    _ser = (uartNum == 1) ? &serial1 : &serial2;
    _ser->begin(baud, SERIAL_8N1, rxPin, txPin);
    _ser->setTimeout(50);
    delay(50);
    flushInput();
}

void GM67::flushInput() {
    if (!_ser) return;
    while (_ser->available()) _ser->read();
    _acc = "";
}

// ------------------------------------------------------------------ wakeup
// Руководство, Приложение 6: «Under the sleep mode, need to wake up first
// (Wake up command: 0x00, 50ms), then send the effective command».
void GM67::wakeup() {
    if (!_ser) return;
    uint8_t zero = 0x00;
    _ser->write(&zero, 1);
    _ser->flush();
    delay(50);
}

void GM67::sendRaw(const uint8_t *data, size_t len) {
    if (!_ser) return;
    wakeup();
    _ser->write(data, len);
    _ser->flush();
}

// Сканер отвечает CMD_ACK (04 D0 04 00 FF 28) на принятую команду.
// В сыром режиме вывода он же может не ответить ничего — поэтому
// отсутствие ACK не считаем фатальной ошибкой, только логируем.
bool GM67::waitAck(uint32_t timeoutMs) {
    if (!_ser) return false;
    uint32_t t0 = millis();
    uint8_t buf[8];
    size_t n = 0;
    while (millis() - t0 < timeoutMs) {
        while (_ser->available() && n < sizeof(buf)) {
            buf[n++] = (uint8_t)_ser->read();
            if (n >= 2 && buf[n - 2] == 0xFF && buf[n - 1] == 0x28) return true;
        }
        delay(2);
    }
    return false;
}

// --------------------------------------------------------------- параметры
// Формат: [LEN][0xC6][0x04][STATUS][BEEP=0x00][PARAM][VALUE][CK][CK]
//   STATUS 0x00 = только до перезагрузки, 0x08 = записать во флеш
// Пример из руководства: Host Mode = 07 C6 04 08 00 8A 08 FE 95
bool GM67::sendParam1(uint8_t param, uint8_t value, bool persist) {
    uint8_t payload[7] = {0x07, 0xC6, 0x04, (uint8_t)(persist ? 0x08 : 0x00),
                          0x00, param, value};
    uint8_t frame[9];
    size_t n = buildFrame(frame, payload, sizeof(payload));
    sendRaw(frame, n);
    return waitAck();
}

// Расширенные параметры: [0x08][0xC6][0x04][STATUS][0x00][PREFIX][SUB][VALUE][CK][CK]
// Пример: терминатор CR LF = 08 C6 04 08 00 F2 05 01 FE 2E
bool GM67::sendParam2(uint8_t prefix, uint8_t sub, uint8_t value, bool persist) {
    uint8_t payload[8] = {0x08, 0xC6, 0x04, (uint8_t)(persist ? 0x08 : 0x00),
                          0x00, prefix, sub, value};
    uint8_t frame[10];
    size_t n = buildFrame(frame, payload, sizeof(payload));
    sendRaw(frame, n);
    return waitAck();
}

// ----------------------------------------------------------- конфигурация
bool GM67::configure(Mode mode, bool persist, bool use115200) {
    bool ok = true;

    // 0. Принудительно включить последовательный интерфейс (TTL-232).
    //    У GM67 два разъёма — UART и USB — и параметр, каким из них он
    //    пользуется. Если модуль настроен на USB-KBW (эмуляция клавиатуры),
    //    он будет исправно читать и пищать, но по UART не пришлёт НИЧЕГО.
    //    Снаружи это выглядит как «сканер работает, а плата его не слышит».
    //    Таблица 6-2: Communication Interface -> Serial.
    ok &= sendParam2(PX_PREFIX, 0x01, 0x00, persist);
    delay(80);

    // 1. Режим чтения.
    //    0x04 = Continuous: сканер читает сам, непрерывно (таблица 6-2).
    //    0x08 = Host Mode:  читает только по команде START_DECODE.
    ok &= sendParam1(P_TRIGGER_MODE, mode == Mode::HOST ? 0x08 : 0x04, persist);
    delay(60);

    // 2. Сырой ASCII на выходе вместо SSI-пакета DECODE_DATA.
    //    С пакетом пришлось бы разбирать заголовок и слать ACK на каждый код.
    ok &= sendParam1(P_DECODE_FMT, 0x00, persist);
    delay(60);

    // 3. Запретить ACK/NAK: сканер не будет ждать от нас подтверждений.
    ok &= sendParam1(P_ACKNAK, 0x00, persist);
    delay(60);

    // 4. Терминатор CR LF — по нему ловим конец штрихкода.
    ok &= sendParam2(PX_PREFIX, PX_TERMINATOR, 0x01, persist);
    delay(60);

    // 5. Одиночный скан = 5 с.
    ok &= sendParam2(PX_PREFIX, PX_SINGLE_SCAN, 0x05, persist);
    delay(60);

    // 6. Пауза перед повторным чтением ТОГО ЖЕ кода.
    //    В непрерывном режиме без неё сканер будет слать один и тот же код
    //    десятки раз в секунду, пока штрихкод лежит перед объективом.
    //    Параметр 0x89, единица измерения 100 мс, значение 0x1E = 3 секунды.
    //    Дополнительно тот же код отсекается программно (см. main.cpp).
    if (mode == Mode::CONTINUOUS) {
        ok &= sendParam1(0x89, 0x1E, persist);
        delay(60);
    }

    // 7. Опционально поднять скорость. Меняем баррейт ПОСЛЕДНИМ и сразу
    //    переоткрываем свой порт, иначе потеряем связь со сканером.
    if (use115200) {
        sendParam1(P_BAUD, 0x0A, persist);
        delay(120);
        _ser->updateBaudRate(115200);
        delay(50);
        flushInput();
    }

    _configOk = ok;
    return ok;
}

// -------------------------------------------------------- старт/стоп скана
//
//  ВАЖНО, из-за чего здесь появился swallowAck(). Сканер отвечает кадром
//  подтверждения на КАЖДУЮ принятую команду: 04 D0 04 00 FF 28. Раньше
//  flushInput() стоял только ПЕРЕД отправкой, а ответ приходил ПОСЛЕ неё —
//  и оставался в буфере. Дальше poll() пропускал эти байты через фильтр
//  «оставляем только печатный ASCII», непечатные 04 D0 04 00 FF отбрасывал,
//  а 0x28 — это символ '(' — оставлял. Через 60 мс паузы накопленный
//  одиночный символ уезжал наружу как «прочитанный штрихкод».
//
//  Симптом со стороны сервера: как только лицо опознано и ушла команда
//  SCAN ON, мгновенно приходит штрихкод из одного знака препинания.
//  У разных команд контрольная сумма разная, поэтому мусорный символ
//  бывал то '(', то '$'.
//
//  В непрерывном режиме этого не было: START_DECODE во время работы
//  не посылался вообще, и ответу неоткуда было взяться.
bool GM67::startDecode() {
    flushInput();
    sendRaw(START_DECODE, sizeof(START_DECODE));
    swallowAck();
    _beamOnMs = millis();
    return true;
}

bool GM67::stopDecode() {
    sendRaw(STOP_DECODE, sizeof(STOP_DECODE));
    swallowAck();
    _beamOnMs = 0;
    return true;
}

// Дождаться служебного ответа на команду и выбросить его вместе с хвостом.
// Ждём недолго: ответ приходит за единицы миллисекунд, а если сканер
// настроен не отвечать (ACK/NAK выключен параметром 0x9F), ждать нечего —
// поэтому отсутствие ответа не ошибка, просто выходим по таймауту.
void GM67::swallowAck(uint32_t timeoutMs) {
    if (!_ser) return;
    uint32_t t0 = millis();
    uint32_t lastByte = 0;
    while (millis() - t0 < timeoutMs) {
        if (_ser->available()) {
            _ser->read();
            lastByte = millis();
        } else if (lastByte && (millis() - lastByte) > 15) {
            break;                 // поток кончился, дальше ждать незачем
        } else {
            delay(2);
        }
    }
    _acc = "";                     // накопитель тоже чистим
}

// ----------------------------------------------------------- питание скана
void GM67::sleep() {
    if (!_ser) return;
#if !GM67_ALLOW_SLEEP
    // Команда сна запрещена флагом GM67_ALLOW_SLEEP в config.h.
    //
    // Причина запрета — асимметрия сбросов. Признак «он спит» живёт в
    // оперативной памяти ESP32 и обнуляется при перезагрузке платы. Сам
    // сканер при этом не сбрасывается: питание у него теперь своё. Значит
    // после прошивки плата считает сканер бодрым, а модуль спит с прошлого
    // запуска и теряет первые кадры. Луч гасим — это безопасно, — а спать
    // не отправляем.
    stopDecode();
    _asleep = false;
    return;
#else
    if (_asleep) return;
    stopDecode();                  // сначала гасим луч, потом усыпляем
    sendRaw(SLEEP_CMD, sizeof(SLEEP_CMD));
    swallowAck();
    _asleep = true;
    Serial.println("[gm67] сканер усыплён");
#endif
}

// --------------------------------------------------------------- wakeHard
// Жёсткое пробуждение — для случая, когда СОСТОЯНИЕ СКАНЕРА НЕИЗВЕСТНО.
//
// Обычный wake() полагается на флаг _asleep, а флаг живёт в памяти платы.
// После перезагрузки он всегда false, даже если модуль спит с прошлого
// запуска, — и wake() просто ничего не делает.
//
// Дальше срабатывает вторая ловушка. По Приложению 6 разбудить должен один
// байт 0x00 с паузой 50 мс, но на практике этого хватает только чтобы
// поднять приёмник: СЛЕДУЮЩИЙ кадр модуль всё равно теряет. Отсюда «сканер
// молчит» три раза подряд на исправном железе.
//
// Поэтому здесь: три байта 0x00 с паузами по 120 мс, ещё 200 мс на
// раскачку и чистка приёмного тракта, чтобы эхо пробуждения не попало
// в разбор ответа. Стоит около 560 мс и выполняется только при загрузке.
void GM67::wakeHard() {
    if (!_ser) return;
    uint8_t zero = 0x00;
    for (int i = 0; i < 3; i++) {
        _ser->write(&zero, 1);
        _ser->flush();
        delay(120);
    }
    delay(200);
    flushInput();
    _asleep = false;
}

bool GM67::wake() {
    if (!_ser) return false;
    if (!_asleep) return true;
    // Пробуждение по руководству: байт 0x00, пауза 50 мс, и только потом
    // осмысленные команды. Делает это wakeup(), поэтому достаточно послать
    // любую безобидную команду — она сама разбудит сканер.
    // Короткого wakeup() здесь недостаточно — см. разбор у wakeHard().
    wakeHard();
    Serial.println("[gm67] сканер разбужен");
    return true;
}

// ------------------------------------------------------------------- poll
// Собираем символы до CR/LF. Если терминатор по какой-то причине выключен,
// строку закрываем по паузе между символами (interCharTimeoutMs).
bool GM67::poll(String &out, uint32_t interCharTimeoutMs) {
    if (!_ser) return false;

    while (_ser->available()) {
        int c = _ser->read();
        if (c < 0) break;
        _lastByteMs = millis();
        _rxBytes++;

        if (_debug) {
            // Печатаем и hex, и символ — сразу видно, текст это или мусор.
            Serial.printf("[gm67:raw] %02X %s\n", (uint8_t)c,
                          (c >= 0x20 && c < 0x7F) ? String((char)c).c_str()
                                                  : "(непечатный)");
        }

        if (c == '\r' || c == '\n') {
            if (_acc.length() > 0) {
                String s = _acc;
                _acc = "";
                if (!acceptable(s)) continue;   // обрывок служебного кадра
                out = s;
                return true;
            }
            continue;                       // пустая строка — игнорируем
        }
        // Отбрасываем управляющие байты: в сыром режиме их быть не должно,
        // но при неудачной конфигурации сюда может прилететь хвост SSI-пакета.
        if (c >= 0x20 && c < 0x7F) {
            _acc += (char)c;
            if (_acc.length() > 256) _acc = "";   // защита от мусора
        } else {
            // Раньше такие байты выбрасывались молча, и при неверном
            // баррейте это выглядело как «сканер вообще ничего не шлёт».
            // Теперь они считаются, и счётчик видно в логе.
            _dropped++;
        }
    }

    // Терминатора нет, но символы были и пауза вышла — отдаём что есть.
    if (_acc.length() > 0 && (millis() - _lastByteMs) > interCharTimeoutMs) {
        String s = _acc;
        _acc = "";
        if (!acceptable(s)) return false;
        out = s;
        return true;
    }
    return false;
}

// Отсев того, что штрихкодом быть не может.
//
// ЧТО ЗДЕСЬ БЫЛО НЕ ТАК. Изначально стояли две проверки: минимальная длина
// и «окно тишины» — 150 мс после включения луча, в которые приходящему
// не верим вообще. Вторая проверка ловила служебный кадр сканера, но заодно
// съедала настоящие коды, и именно она была причиной «сканер пищит, а
// в журнале пусто».
//
// Рассуждение, которое меня подвело: я считал, что человек физически не
// успеет поднести штрихкод за 150 мс после команды SCAN ON. Успевает — если
// он поднёс его ЗАРАНЕЕ. А он так и делает: экран на посту показывает
// «Kodu skan edin» с задержкой (или, как сейчас, не работает вовсе), и
// сотрудник держит груз перед сканером с самого начала проверки. Луч
// вспыхивает — код читается за 60–120 мс — и попадает ровно в окно, в
// котором прошивка ему не верит.
//
// Хуже того, промах необратим: в Host Mode START_DECODE запускает ОДИН сеанс
// чтения, и после удачного декода сканер останавливается сам. Выбросив этот
// единственный декод, мы остаёмся с горящим лучом, который больше ничего
// не прочитает — до конца таймаута. Снаружи это выглядит именно так, как
// описал пользователь: пискнуло, а записи нет.
//
// Что вместо этого. Служебный кадр — 04 D0 04 00 FF 28 — после фильтра
// «только печатный ASCII» может дать максимум один-два символа, и это
// байты контрольной суммы: знаки препинания вроде '(' или '$'. Значит,
// отличать надо не по времени прихода, а по содержимому:
//
//   1. Длина. Один-два символа кодом груза не бывают.
//   2. Наличие буквы или цифры. В настоящем штрихкоде они есть всегда,
//      в обрывке контрольной суммы — как повезёт, но одного знака
//      препинания достаточно, чтобы его опознать.
//
// Обе проверки смотрят на строку, а не на часы, и настоящий код не съедают
// независимо от того, когда его поднесли.
bool GM67::acceptable(const String &s) const {
    if ((int)s.length() < _minLen) {
        Serial.printf("[gm67] отброшено как служебное: \"%s\" (%d симв., "
                      "минимум %d)\n", s.c_str(), (int)s.length(), _minLen);
        return false;
    }
    bool hasAlnum = false;
    for (size_t i = 0; i < s.length(); i++) {
        if (isalnum((unsigned char)s[i])) { hasAlnum = true; break; }
    }
    if (!hasAlnum) {
        Serial.printf("[gm67] отброшено: \"%s\" — ни буквы, ни цифры, "
                      "это обрывок служебного кадра\n", s.c_str());
        return false;
    }
    // Окно тишины оставлено выключаемым (GM67_GUARD_MS), но по умолчанию 0:
    // см. разбор выше. Включайте, только если проверки по содержимому вдруг
    // не хватило — и помните, чем это грозит.
    if (_guardMs && _beamOnMs && (millis() - _beamOnMs) < _guardMs) {
        Serial.printf("[gm67] отброшено окном тишины: пришло через %lu мс "
                      "после включения луча\n",
                      (unsigned long)(millis() - _beamOnMs));
        return false;
    }
    return true;
}

// Можно ли безопасно перевзвести чтение прямо сейчас.
// startDecode() чистит приёмный буфер, и если сделать это посреди
// приходящего кода, код пропадёт. Поэтому перевзводим только когда
// в буфере пусто и накопитель пуст.
bool GM67::idle() const {
    if (!_ser) return false;
    return _acc.length() == 0 && _ser->available() == 0;
}
