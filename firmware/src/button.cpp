#include "button.h"
#include "button_press_filter.h"

namespace {

const uint32_t DEBOUNCE_MS = 30;
const uint32_t LONG_MS = 2000;
constexpr uint8_t PRESS_QUEUE_SIZE = 8;
portMUX_TYPE g_pressMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_pressTimes[PRESS_QUEUE_SIZE];
volatile uint8_t g_pressHead = 0;
volatile uint8_t g_pressTail = 0;
uint32_t g_pressEpoch = 0;
TaskHandle_t g_pressTask = nullptr;

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

void pressTask(void *) {
    ButtonPressFilter filter;
    uint32_t epoch = UINT32_MAX;
    for (;;) {
        const uint32_t now = millis();
        const bool down = readRaw();
        portENTER_CRITICAL(&g_pressMux);
        const uint32_t currentEpoch = g_pressEpoch;
        portEXIT_CRITICAL(&g_pressMux);
        if (epoch != currentEpoch) {
            epoch = currentEpoch;
            filter.reset(now, down);
        }
        uint32_t pressedAt = 0;
        if (filter.sample(now, down, pressedAt)) {
            portENTER_CRITICAL(&g_pressMux);
            const uint8_t next = (g_pressHead + 1) % PRESS_QUEUE_SIZE;
            if (epoch == g_pressEpoch && next != g_pressTail) {
                g_pressTimes[g_pressHead] = pressedAt;
                g_pressHead = next;
            }
            portEXIT_CRITICAL(&g_pressMux);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
    resetPressHistory();
    // Sampling continues while camera and HTTP calls block the main loop.
    // Raw GPIO interrupt edges must never trigger a restart directly.
    if (!g_pressTask && xTaskCreatePinnedToCore(
            pressTask, "button-filter", 2048, nullptr, 1, &g_pressTask, 0) != pdPASS) {
        Serial.println("[btn] фильтр не запущен: двойное нажатие отключено");
    }
}

void resetPressHistory() {
    portENTER_CRITICAL(&g_pressMux);
    g_pressHead = g_pressTail = 0;
    ++g_pressEpoch;
    portEXIT_CRITICAL(&g_pressMux);
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
