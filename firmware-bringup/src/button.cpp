#include "button.h"

namespace {

const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_MS = 2000;

int      g_pin = -1;
bool     g_activeLow = true;
bool     g_stable = false;        // подтверждённое состояние: true = нажата
bool     g_raw = false;
uint32_t g_changedMs = 0;
uint32_t g_downMs = 0;
bool     g_shortPending = false;
bool     g_longFired = false;

bool readRaw() {
    int v = digitalRead(g_pin);
    return g_activeLow ? (v == LOW) : (v == HIGH);
}

void update() {
    if (g_pin < 0) return;
    bool r = readRaw();
    uint32_t now = millis();

    if (r != g_raw) {
        g_raw = r;
        g_changedMs = now;             // уровень дрогнул — отсчёт заново
        return;
    }
    if (g_raw == g_stable) return;
    if (now - g_changedMs < DEBOUNCE_MS) return;

    g_stable = g_raw;
    if (g_stable) {
        g_downMs = now;
        g_longFired = false;
    } else {
        // Отпустили. Короткое нажатие засчитываем только если длинное
        // уже не сработало — иначе одно удержание дало бы два события.
        if (!g_longFired) g_shortPending = true;
    }
}

}  // namespace

namespace btn {

void begin(int pin, bool activeLow) {
    g_pin = pin;
    g_activeLow = activeLow;
    // INPUT_PULLUP включает внутренний резистор — внешний не нужен.
    pinMode(pin, activeLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    delay(5);
    g_raw = g_stable = readRaw();
    g_changedMs = millis();
}

bool pressed() {
    update();
    if (!g_shortPending) return false;
    g_shortPending = false;
    return true;
}

bool longPressed() {
    update();
    if (!g_stable || g_longFired) return false;
    if (millis() - g_downMs < LONG_MS) return false;
    g_longFired = true;
    return true;
}

bool isDown() {
    update();
    return g_stable;
}

}  // namespace btn
