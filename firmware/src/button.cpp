#include "button.h"

namespace {

const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_MS = 2000;
const uint32_t EDGE_GUARD_MS = 180;
constexpr uint8_t PRESS_QUEUE_SIZE = 8;
portMUX_TYPE g_pressMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_pressTimes[PRESS_QUEUE_SIZE];
volatile uint8_t g_pressHead = 0;
volatile uint8_t g_pressTail = 0;
volatile uint32_t g_lastEdgeMs = 0;

void IRAM_ATTR onPressEdge() {
    const uint32_t now = millis();
    portENTER_CRITICAL_ISR(&g_pressMux);
    if (now - g_lastEdgeMs >= EDGE_GUARD_MS) {
        const uint8_t next = (g_pressHead + 1) % PRESS_QUEUE_SIZE;
        if (next != g_pressTail) {
            g_pressTimes[g_pressHead] = now;
            g_pressHead = next;
            g_lastEdgeMs = now;
        }
    }
    portEXIT_CRITICAL_ISR(&g_pressMux);
}

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
    g_pressHead = g_pressTail = 0;
    g_lastEdgeMs = millis() - EDGE_GUARD_MS;
    attachInterrupt(digitalPinToInterrupt(pin), onPressEdge,
                    activeLow ? FALLING : RISING);
}

bool pressedAt(uint32_t &timeMs) {
    bool available = false;
    portENTER_CRITICAL(&g_pressMux);
    if (g_pressTail != g_pressHead) {
        timeMs = g_pressTimes[g_pressTail];
        g_pressTail = (g_pressTail + 1) % PRESS_QUEUE_SIZE;
        available = true;
    }
    portEXIT_CRITICAL(&g_pressMux);
    return available;
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
