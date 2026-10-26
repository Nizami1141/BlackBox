#include "button.h"

namespace {

const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_MS = 2000;
const uint32_t RELEASE_MS = 150;

int      g_pin = -1;
bool     g_activeLow = true;
bool     g_stable = false;        // подтверждённое состояние: true = нажата
bool     g_raw = false;
uint32_t g_changedMs = 0;
uint32_t g_downMs = 0;
bool     g_shortPending = false;
bool     g_longFired = false;
bool     g_armed = false;
bool     g_validPress = false;

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
    if (g_raw == g_stable) {
        // A LOW input at boot (or after a check) is not a new press.
        // Require an observed, stable release before accepting another one.
        if (!g_stable && now - g_changedMs >= RELEASE_MS) g_armed = true;
        return;
    }
    if (now - g_changedMs < DEBOUNCE_MS) return;

    g_stable = g_raw;
    if (g_stable) {
        g_downMs = now;
        g_longFired = false;
        g_validPress = g_armed;
        g_armed = false;
    } else {
        // Отпустили. Короткое нажатие засчитываем только если длинное
        // уже не сработало — иначе одно удержание дало бы два события.
        // Suppress long holds even when longPressed() was never polled
        // (idle uses pressed(), while barcode waiting uses longPressed()).
        if (g_validPress && !g_longFired && now - g_downMs < LONG_MS)
            g_shortPending = true;
        g_validPress = false;
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
    g_downMs = g_changedMs;
    g_shortPending = false;
    g_longFired = false;
    g_armed = false;
    g_validPress = false;
    // Poll the debounced level only. GPIO noise must never restart the ESP.
}

bool pressed() {
    update();
    if (!g_shortPending) return false;
    g_shortPending = false;
    return true;
}

void discardPending() {
    if (g_pin < 0) return;
    g_raw = g_stable = readRaw();
    g_changedMs = g_downMs = millis();
    g_shortPending = false;
    g_longFired = g_stable; // Ignore the release of an already held button.
    g_armed = false;
    g_validPress = false;
}

bool longPressed() {
    update();
    if (!g_stable || !g_validPress || g_longFired) return false;
    if (millis() - g_downMs < LONG_MS) return false;
    g_longFired = true;
    return true;
}

bool isDown() {
    update();
    return g_stable;
}

int rawLevel() { return g_pin < 0 ? -1 : digitalRead(g_pin); }

bool armed() {
    update();
    return g_armed;
}

}  // namespace btn
