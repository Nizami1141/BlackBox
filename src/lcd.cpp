#include "lcd.h"

#include <Wire.h>

#include "config.h"

namespace {

// --- биты PCF8574 -> выводы HD44780 ---------------------------------------
const uint8_t BIT_RS = 0x01;
const uint8_t BIT_RW = 0x02;
const uint8_t BIT_EN = 0x04;
const uint8_t BIT_BL = 0x08;      // подсветка

uint8_t  g_addr = 0;
bool     g_ok = false;
uint8_t  g_bl = BIT_BL;
uint32_t g_holdUntil = 0;
bool     g_idleShown = true;

// --- живучесть шины --------------------------------------------------------
//
//  ЗАЧЕМ ЭТО ЗДЕСЬ. Экран на посту не отвечал, и я считал это безобидным:
//  begin() не найдёт адрес, g_ok останется false, print() будет молча
//  выходить. Так и есть — пока экран не отвечает СОВСЕМ.
//
//  Опасен другой случай: адаптер отозвался при поиске (адрес нашёлся), а
//  дальше шина работает через раз. Ровно это и даёт питание 5 В без
//  преобразователя уровней: подтяжки адаптера тянут линии к 5 В, входы
//  ESP32-S3 к этому не приспособлены, и обмен идёт на грани.
//
//  Цена ошибки считается так. Один вывод двух строк — это 2 x (1 команда +
//  16 символов) = 34 байта, каждый уходит двумя полубайтами, каждый
//  полубайт это три обращения к расширителю. Итого около 200 транзакций I2C
//  на одно сообщение. На исправной шине это 50 мс. На зависшей каждая
//  транзакция упирается в таймаут Wire (по умолчанию 50 мс), и вывод одного
//  сообщения занимает ДЕСЯТЬ СЕКУНД.
//
//  А рисуется экран в той же задаче, которая опрашивает сканер
//  (photoTask в main.cpp: scannerStep -> buttonStep -> lcdStep). Значит
//  полудохлый экран замораживает чтение штрихкодов на эти же десять секунд.
//  Сканер честно прочитал код и отдал его в UART — а забрать оттуда некому.
//  Снаружи это выглядит как «из-за экрана не читается штрихкод», и это
//  ровно то, что показалось пользователю. Причина не в вольтах на входе
//  сканера, а в том, что общая задача стоит на зависшей шине.
//
//  Отсюда правило: экран не имеет права задерживать пост. Считаем подряд
//  идущие сбои, и после LCD_MAX_FAILS объявляем экран мёртвым — обращения
//  прекращаются мгновенно. Раз в LCD_RETRY_MS пробуем поднять заново:
//  контакт мог быть плохим, а не оборванным.
uint16_t g_fails    = 0;      // сбоев подряд
uint32_t g_deadAt   = 0;      // когда объявили мёртвым (0 = жив)
uint32_t g_errTotal = 0;      // всего сбоев с включения — для лога
int      g_sda = -1, g_scl = -1;
uint8_t  g_wantAddr = 0;

// Код возврата Wire.endTransmission() — самая ценная величина во всей
// диагностике шины, и раньше мы её выбрасывали. Разные коды означают
// совершенно разные неисправности, и лечатся они по-разному.
const char *i2cErrText(int e) {
    switch (e) {
        case 0: return "успех";
        case 1: return "данные не влезли в буфер";
        case 2: return "NACK на АДРЕС — ведомый не отозвался";
        case 3: return "NACK на ДАННЫЕ — адрес принял, байт не принял";
        case 4: return "прочая ошибка шины";
        case 5: return "ТАЙМАУТ — линия занята или такты не идут";
        default: return "неизвестный код";
    }
}

int  g_lastErr  = 0;          // код последнего сбоя, для отчёта
bool g_reported = false;      // полный разбор печатаем один раз, а не каждые 30 с

void recordResult(int e) {
    if (e == 0) {
        g_fails = 0;
        return;
    }

    g_lastErr = e;
    g_errTotal++;
    if (++g_fails < LCD_MAX_FAILS) return;

    // Порог перейдён. Дальше молчим: каждая следующая попытка стоит
    // таймаута, а таймауты складываются в секунды простоя поста.
    g_ok = false;
    g_deadAt = millis();
    Serial.printf("[lcd] шина не отвечает: код %d (%s), сбоев подряд %u, "
                  "всего %lu. Экран отключён, чтобы не тормозить сканер.\n",
                  e, i2cErrText(e), (unsigned)g_fails,
                  (unsigned long)g_errTotal);
}

void expanderWrite(uint8_t data) {
    if (!g_addr) return;
    Wire.beginTransmission(g_addr);
    Wire.write(data | g_bl);
    recordResult(Wire.endTransmission());
}

// Строб E: данные защёлкиваются по спаду. Задержки взяты из даташита
// HD44780 с запасом — экономить тут нечего, вывод редкий.
void pulse(uint8_t data) {
    expanderWrite(data | BIT_EN);
    delayMicroseconds(1);
    expanderWrite(data & ~BIT_EN);
    delayMicroseconds(50);
}

void write4(uint8_t nibble) {
    expanderWrite(nibble);
    pulse(nibble);
}

// Байт уходит двумя полубайтами: адаптер даёт всего 4 линии данных.
void send(uint8_t value, uint8_t mode) {
    write4((value & 0xF0) | mode);
    write4(((value << 4) & 0xF0) | mode);
}

void command(uint8_t c) { send(c, 0); }
void data(uint8_t c)    { send(c, BIT_RS); }

// ---------------------------------------------------------------------------
//  Знакоместа для азербайджанских букв, 5x8 точек.
//  Каждый байт — строка сверху вниз, младшие 5 бит — точки слева направо.
// ---------------------------------------------------------------------------
const uint8_t GLYPHS[7][8] = {
    // ə — перевёрнутая e
    {0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01110, 0b00000},
    // ı — i без точки
    {0b00000, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110, 0b00000},
    // ö
    {0b01010, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110, 0b00000},
    // ü
    {0b01010, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101, 0b00000},
    // ç — c с седилью
    {0b00000, 0b01110, 0b10001, 0b10000, 0b10001, 0b01110, 0b00100, 0b01000},
    // ş — s с седилью
    {0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110, 0b00100, 0b01000},
    // ğ — g с бревисом
    {0b01110, 0b00000, 0b01111, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110},
};

// Коды 0x08..0x0E — псевдонимы знакомест CGRAM 0..6. Берём именно их,
// а не 0x00..0x06: ноль в строке C означал бы её конец, и текст обрывался бы
// на первой же букве «ə».
const uint8_t CH_SCHWA = 0x08;   // ə
const uint8_t CH_DOTI  = 0x09;   // ı
const uint8_t CH_OUML  = 0x0A;   // ö
const uint8_t CH_UUML  = 0x0B;   // ü
const uint8_t CH_CCED  = 0x0C;   // ç
const uint8_t CH_SCED  = 0x0D;   // ş
const uint8_t CH_GBRV  = 0x0E;   // ğ

void loadGlyphs() {
    for (uint8_t i = 0; i < 7; i++) {
        command(0x40 | (i << 3));            // адрес CGRAM
        for (uint8_t r = 0; r < 8; r++) data(GLYPHS[i][r]);
    }
    command(0x80);                            // вернуться в DDRAM
}

// ---------------------------------------------------------------------------
//  UTF-8 -> коды экрана.
//
//  Все азербайджанские буквы, кроме основной латиницы, кодируются в UTF-8
//  двумя байтами. Разбираем только эти пары; всё, что не опознали,
//  заменяем вопросительным знаком, чтобы на экране была видна проблема,
//  а не случайный символ из знакогенератора.
// ---------------------------------------------------------------------------
uint8_t mapPair(uint8_t b1, uint8_t b2) {
    uint16_t cp = 0;
    if ((b1 & 0xE0) == 0xC0) cp = ((uint16_t)(b1 & 0x1F) << 6) | (b2 & 0x3F);
    switch (cp) {
        case 0x0259: return CH_SCHWA;   // ə
        case 0x018F: return CH_SCHWA;   // Ə -> строчной глиф
        case 0x0131: return CH_DOTI;    // ı
        case 0x0130: return 'I';        // İ -> обычная I, глифа не осталось
        case 0x00F6: case 0x00D6: return CH_OUML;   // ö Ö
        case 0x00FC: case 0x00DC: return CH_UUML;   // ü Ü
        case 0x00E7: case 0x00C7: return CH_CCED;   // ç Ç
        case 0x015F: case 0x015E: return CH_SCED;   // ş Ş
        case 0x011F: case 0x011E: return CH_GBRV;   // ğ Ğ
        default:     return '?';
    }
}

}  // namespace

namespace lcd {

// ---------------------------------------------------------- расклинить шину
//
// I2C умеет зависать так, что программно её уже не поднять: ведомый, сбитый
// помехой или сбросом мастера посреди байта, держит SDA прижатой к земле и
// ждёт недостающих тактов. Мастер видит вечно занятую шину и не может даже
// начать транзакцию — при этом «оборванным» снаружи ничего не выглядит.
//
// Лечится единственным способом: снять SDA и SCL с периферии I2C, взять их
// обычными выводами и выдать до девяти тактов вручную. Ведомый досчитает
// свой байт и отпустит SDA, после чего выдаём условие STOP.
static void unstickBus(int sda, int scl) {
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    delayMicroseconds(10);
    if (digitalRead(sda) == HIGH) return;    // линия свободна, вмешиваться не во что

    Serial.println("[lcd] SDA прижата к земле — расклиниваю шину тактами");
    pinMode(scl, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 9 && digitalRead(sda) == LOW; i++) {
        digitalWrite(scl, LOW);  delayMicroseconds(5);
        digitalWrite(scl, HIGH); delayMicroseconds(5);
    }
    // Условие STOP: SDA уходит вверх при высокой SCL.
    pinMode(sda, OUTPUT_OPEN_DRAIN);
    digitalWrite(sda, LOW);  delayMicroseconds(5);
    digitalWrite(scl, HIGH); delayMicroseconds(5);
    digitalWrite(sda, HIGH); delayMicroseconds(5);

    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
}

// ------------------------------------------------------------ разбор шины --
// Печатается при неудачной инициализации. Три факта, по которым причина
// обычно определяется сразу — вместо гадания «наверное уровни».
//
// ВНИМАНИЕ: функция оставляет шину закрытой (Wire.end). Это нормально:
// вызывается она только на пути к возврату false, а следующая попытка
// поднять экран начнётся с полного begin().
void busReport(int sda, int scl) {
    Serial.printf("[lcd] --- разбор шины I2C (SDA=%d, SCL=%d) ---\n", sda, scl);
    Serial.printf("      последний код ошибки: %d (%s)\n",
                  g_lastErr, i2cErrText(g_lastErr));

    // 1. Сколько адресов «откликается». На исправной шине с одним экраном
    //    ровно один. Если откликаются десятки — отвечает не экран, а сам
    //    драйвер: транзакция не доходит до проводов, и любой адрес
    //    выглядит живым. Это единственный способ отличить настоящий отклик
    //    от ложного, а ложный уводит поиск совсем не туда.
    int found = 0, firstAddr = -1;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            found++;
            if (firstAddr < 0) firstAddr = a;
        }
    }
    Serial.printf("      откликнулось адресов: %d", found);
    if (firstAddr >= 0) Serial.printf(", первый 0x%02X", firstAddr);
    Serial.println();
    if (found > 3) {
        Serial.println("      Неожиданно много откликов для одной LCD-панели.");
        Serial.println("      Проверьте шину и другие подключённые I2C-устройства.");
    }

    // 2. Уровни на линиях БЕЗ внутренних подтяжек. Так проверяется, работают
    //    ли подтяжки самого адаптера — то же самое, что померить мультиметром,
    //    только руки свободны.
    Wire.end();
    pinMode(sda, INPUT);
    pinMode(scl, INPUT);
    delayMicroseconds(100);
    bool sdaHi = digitalRead(sda) == HIGH;
    bool sclHi = digitalRead(scl) == HIGH;
    Serial.printf("      без внутренних подтяжек: SDA=%s, SCL=%s\n",
                  sdaHi ? "высокий" : "НИЗКИЙ", sclHi ? "высокий" : "НИЗКИЙ");

    if (sdaHi && sclHi) {
        // ВАЖНО: сам по себе этот результат НИЧЕГО не доказывает, и раньше
        // здесь стоял слишком смелый вывод («питание есть, обрыва нет»).
        // Вход без подтяжки — висящий в воздухе вход, он читается как
        // угодно, чаще всего единицей. Проверка ниже даёт ещё одно наблюдение.
        Serial.println("      Обе линии не прижаты к земле. Само по себе это");
        Serial.println("      НЕ доказывает, что подтяжки адаптера работают:");
        Serial.println("      висящий в воздухе вход тоже читается единицей.");
        Serial.println("      Следом проверяем влияние слабой внутренней подтяжки вниз.");
    } else {
        Serial.println("      Линия прижата к земле. Варианты: короткое на");
        Serial.println("      макетке, или ведомый завис посреди байта и");
        Serial.println("      держит SDA.");
    }

    // 3. Observe external pullups on the ESP side. A level shifter can hold
    // these lines high without proving the remote adapter's power, common
    // ground, or continuity. This does not replace electrical measurements.
    pinMode(sda, INPUT_PULLDOWN);
    pinMode(scl, INPUT_PULLDOWN);
    delay(2);
    bool sdaPd = digitalRead(sda) == HIGH;
    bool sclPd = digitalRead(scl) == HIGH;
    Serial.printf("      с внутренней подтяжкой ВНИЗ: SDA=%s, SCL=%s\n",
                  sdaPd ? "высокий" : "НИЗКИЙ", sclPd ? "высокий" : "НИЗКИЙ");
    if (sdaPd && sclPd) {
        Serial.println("      На стороне ESP32 внешние подтяжки удерживают линии высокими.");
        Serial.println("      Это не подтверждает общую землю или связь до самого HW-61.");
    } else {
        Serial.println("      Хотя бы одна линия не удерживается на высоком уровне.");
        Serial.println("      Проверьте питание LV/HV, подтяжки, общую землю и проводку.");
    }

    // 4. Могут ли сами выводы платы прижать линии к земле. Отделяет
    //    неисправность ESP32 от неисправности экрана. В открытом стоке
    //    вход остаётся включённым, поэтому читаем реальный уровень провода,
    //    а не то, что мы туда выставили.
    pinMode(sda, OUTPUT_OPEN_DRAIN); digitalWrite(sda, LOW);
    delayMicroseconds(200);
    bool sdaLow = digitalRead(sda) == LOW;
    digitalWrite(sda, HIGH);
    pinMode(scl, OUTPUT_OPEN_DRAIN); digitalWrite(scl, LOW);
    delayMicroseconds(200);
    bool sclLow = digitalRead(scl) == LOW;
    digitalWrite(scl, HIGH);
    pinMode(sda, INPUT);
    pinMode(scl, INPUT);
    Serial.printf("      плата прижимает линии к земле: SDA=%s, SCL=%s\n",
                  sdaLow ? "да" : "НЕТ", sclLow ? "да" : "НЕТ");
    if (!sdaLow || !sclLow) {
        Serial.println("      Вывод не смог прижать линию. Либо он повреждён,");
        Serial.println("      либо на линии висит источник напряжения, либо");
        Serial.println("      этот пин занят другой периферией платы.");
    }

    Serial.println("[lcd] --- конец разбора ---");
}

// ------------------------------------------------- инициализация HD44780 --
// Вынесено отдельно, потому что нужно в двух местах: при первом подъёме
// и при восстановлении из рассинхрона (см. resync ниже).
//
// Три посылки 0x30 — не суеверие, а предписанный даташитом способ вывести
// контроллер в известное состояние ИЗ ЛЮБОГО, включая съехавший на пол-байта.
// В 4-битном режиме каждый байт идёт двумя полубайтами, и оборванная посреди
// них передача смещает контроллер навсегда: дальше любой текст превращается
// в мусор, а ошибок на шине при этом НЕТ — расширитель PCF8574 честно
// подтверждает байты, рассинхрон живёт за ним, в самой панели.
void hd44780Init() {
    delay(50);
    expanderWrite(0);
    delay(50);
    write4(0x30); delay(5);
    write4(0x30); delayMicroseconds(150);
    write4(0x30); delayMicroseconds(150);
    write4(0x20);                       // теперь точно 4 бита

    command(0x28);   // 4 бита, 2 строки, шрифт 5x8
    command(0x08);   // экран выключен
    command(0x01);   // очистка
    delay(2);
    command(0x06);   // курсор вправо, экран не сдвигается
    command(0x0C);   // экран включён, курсор и мигание выключены

    loadGlyphs();
}

bool begin(int sda, int scl, uint8_t addr) {
    g_sda = sda; g_scl = scl; g_wantAddr = addr;
    g_addr = 0; g_fails = 0; g_deadAt = 0;

    // Wire.end() перед Wire.begin() обязателен, и вот почему.
    // При повторном begin() на уже запущенной шине arduino-esp32 печатает
    // "Bus already started in Master Mode" и НИЧЕГО не делает: ни таймингов,
    // ни пинов, ни состояния контроллера не трогает. То есть попытка поднять
    // экран заново раз в LCD_RETRY_MS была холостой — переинициализации не
    // происходило, и экран, однажды объявленный мёртвым, ожить уже не мог.
    Wire.end();
    unstickBus(sda, scl);

    if (!Wire.begin(sda, scl, 100000)) {
        g_ok = false;
        g_lastErr = 4;
        g_deadAt = millis();
        Serial.printf("[lcd] Wire.begin FAILED: I2C0, SDA=%d SCL=%d; address scan skipped\n",
                      sda, scl);
        return false;
    }
    // Таймаут одной транзакции. По умолчанию в arduino-esp32 это 50 мс, и на
    // зависшей шине вывод одного сообщения занимал бы десять секунд — см.
    // разбор у expanderWrite(). На исправной шине транзакция укладывается
    // в доли миллисекунды, так что 10 мс — это уже с большим запасом.
    Wire.setTimeOut(LCD_I2C_TIMEOUT_MS);

    auto probe = [](uint8_t a) {
        Wire.beginTransmission(a);
        g_lastErr = Wire.endTransmission();
        return g_lastErr == 0;
    };

    if (addr) {
        g_addr = probe(addr) ? addr : 0;
    } else {
        // Два адреса покрывают почти все бэкпаки: 0x27 у PCF8574T,
        // 0x3F у PCF8574AT. Если перемычки адреса перепаяны — ищем по всей шине.
        if (probe(0x27))      g_addr = 0x27;
        else if (probe(0x3F)) g_addr = 0x3F;
        else {
            for (uint8_t a = 0x20; a <= 0x27 && !g_addr; a++) if (probe(a)) g_addr = a;
            for (uint8_t a = 0x38; a <= 0x3F && !g_addr; a++) if (probe(a)) g_addr = a;
        }
    }

    if (!g_addr) {
        g_ok = false;
        // Разбор печатаем только при первой неудаче: экран пробуется заново
        // раз в LCD_RETRY_MS, и полный отчёт каждые полминуты залил бы лог.
        if (!g_reported) { g_reported = true; busReport(sda, scl); }
        // Отметка времени нужна, чтобы tick() знал, когда пробовать снова.
        // Без неё ненайденный при загрузке экран остался бы ненайденным
        // навсегда, даже если провод потом воткнули.
        g_deadAt = millis();
        return false;
    }

    // Сколько сбоев шины было ДО инициализации — чтобы в конце понять,
    // прошла она чисто или адаптер отвечает через раз.
    const uint32_t errBefore = g_errTotal;

    hd44780Init();

    // Адрес отозвался, но вся инициализация прошла со сбоями — это и есть
    // «шина работает через раз». Признать такой экран рабочим нельзя:
    // он будет упираться в таймауты на каждом сообщении и тормозить пост.
    if (g_errTotal != errBefore) {
        Serial.printf("[lcd] адрес 0x%02X отозвался, но инициализация прошла "
                      "со сбоями (%lu). Экран не включаю: шина, которая "
                      "работает через раз, хуже отсутствующей — она тормозит "
                      "задачу, где опрашивается сканер.\n",
                      g_addr, (unsigned long)(g_errTotal - errBefore));
        busReport(sda, scl);
        g_ok = false;
        g_deadAt = millis();
        return false;
    }

    g_ok = true;
    g_deadAt = 0;
    return true;
}

bool ok() { return g_ok; }
bool diagnose() {
    if (g_sda < 0 || g_scl < 0) {
        Serial.println("[lcd] manual check unavailable: pins not initialized");
        return false;
    }
    Serial.printf("[lcd] manual check: I2C0 SDA=%d SCL=%d; no MCU reset\n", g_sda, g_scl);
    g_reported = false;
    const bool found = begin(g_sda, g_scl, g_wantAddr);
    Serial.printf("[lcd] manual check result=%s; address=0x%02X; last_i2c_error=%d\n",
                  found ? "READY" : "NOT_READY", g_addr, g_lastErr);
    return found;
}
bool probe() {
    if (!g_ok || !g_addr) return false;
    Wire.beginTransmission(g_addr);
    const int result = Wire.endTransmission();
    recordResult(result);
    return result == 0;
}
uint8_t address() { return g_addr; }
uint32_t errors() { return g_errTotal; }

void clear() {
    if (!g_ok) return;
    command(0x01);
    delay(2);
}

void backlight(bool on) {
    g_bl = on ? BIT_BL : 0;
    if (g_ok) expanderWrite(0);
}

void print(uint8_t line, const String &utf8) {
    if (!g_ok) return;
    command(line == 0 ? 0x80 : 0xC0);        // начало строки 1 или 2

    const char *p = utf8.c_str();
    size_t n = utf8.length();
    uint8_t printed = 0;

    for (size_t i = 0; i < n && printed < 16; ) {
        uint8_t b = (uint8_t)p[i];
        if (b < 0x80) {
            data(b);
            i += 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < n) {
            data(mapPair(b, (uint8_t)p[i + 1]));
            i += 2;
        } else {
            // Трёх- и четырёхбайтные последовательности (кириллица тоже
            // двухбайтная, но её глифов нет) — показываем знак вопроса
            // и пропускаем всю последовательность целиком.
            data('?');
            i += ((b & 0xF0) == 0xE0) ? 3 : 4;
        }
        printed++;
    }
    // Добиваем пробелами: иначе от прошлого сообщения останется хвост.
    while (printed++ < 16) data(' ');
}

void show(const String &line1, const String &line2) {
    // Раньше при мёртвой шине print() молча выходил, а в логе оставались
    // строки вида "[cp] экран: Yoxlama ucun" — и выглядело это так, будто
    // текст на экране есть. Из-за этого отладка не туда уходила не один раз.
    // Теперь несостоявшийся вывод виден в логе явно.
    if (!g_ok) {
        Serial.printf("[lcd] \"%s\" / \"%s\"  <-- НЕ ВЫВЕДЕНО: шина I2C молчит\n",
                      line1.c_str(), line2.c_str());
    }
    print(0, line1);
    print(1, line2);
    g_holdUntil = 0;
    g_idleShown = false;
}

void showFor(const String &line1, const String &line2, uint32_t ms) {
    show(line1, line2);
    g_holdUntil = millis() + ms;
    g_idleShown = false;
}

// ------------------------------------------------------------ пересинхрон --
// Лечит панель, съехавшую на пол-байта в 4-битном режиме.
//
// Почему это нельзя поймать по ошибкам: рассинхрон живёт ЗА расширителем
// PCF8574. Тот честно подтверждает каждый байт по I2C, драйвер считает
// вывод удачным, а панель показывает мусор — например, застревает на одном
// символе. Единственный признак снаружи — глазами, поэтому просто
// переинициализируем контроллер там, где это дёшево: перед проверкой.
//
// Вместе с загрузкой глифов занимает около 0.3 с на текущей шине;
// вызывается только когда экран считается живым.
void resync() {
    if (!g_ok || !g_addr) return;
    hd44780Init();
    g_idleShown = false;      // дежурную надпись нарисовать заново
}

void tick(const String &idleLine1, const String &idleLine2) {
    // Экран объявляли мёртвым — раз в LCD_RETRY_MS пробуем поднять заново.
    // Плохой контакт на макетке восстанавливается сам, и заставлять из-за
    // него перезагружать пост незачем.
    if (!g_ok && g_deadAt && (millis() - g_deadAt) > (uint32_t)LCD_RETRY_MS) {
        g_deadAt = millis();          // следующая попытка не раньше чем через период
        if (begin(g_sda, g_scl, g_wantAddr)) {
            Serial.println("[lcd] шина ожила, экран снова работает");
            g_idleShown = false;
        }
        return;
    }
    if (!g_ok) return;
    if (g_idleShown) return;
    if (g_holdUntil == 0) return;
    if ((int32_t)(millis() - g_holdUntil) < 0) return;
    print(0, idleLine1);
    print(1, idleLine2);
    g_holdUntil = 0;
    g_idleShown = true;
}

}  // namespace lcd
