#include "lcd.h"

#include <Wire.h>

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

void expanderWrite(uint8_t data) {
    Wire.beginTransmission(g_addr);
    Wire.write(data | g_bl);
    Wire.endTransmission();
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

bool begin(int sda, int scl, uint8_t addr) {
    Wire.begin(sda, scl, 100000);            // 100 кГц: HD44780 быстрее не нужно

    auto probe = [](uint8_t a) {
        Wire.beginTransmission(a);
        return Wire.endTransmission() == 0;
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
        return false;
    }

    // Инициализация 4-битного режима по даташиту HD44780. Три посылки 0x03
    // нужны, чтобы вывести контроллер в известное состояние независимо от
    // того, в каком он был (после сброса платы экран не сбрасывается).
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
    g_ok = true;
    return true;
}

bool ok() { return g_ok; }
uint8_t address() { return g_addr; }

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

void tick(const String &idleLine1, const String &idleLine2) {
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
