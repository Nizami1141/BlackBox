#include "config.h"
#include "button_reboot.h"

#if ENABLE_ETH && ENABLE_BUTTON
#include <Arduino.h>
#include "reboot_gesture.h"

namespace {
TaskHandle_t task = nullptr;
int buttonPin = -1;

void monitor(void *) {
    RebootGesture gesture;
    gesture.reset(digitalRead(buttonPin) == LOW, millis());
    TickType_t previous = xTaskGetTickCount();
    while (true) {
        if (gesture.sample(digitalRead(buttonPin) == LOW, millis())) {
            Serial.println("[reboot] 5 clicks within 3 seconds: restarting ESP32");
            Serial.flush();
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP.restart();
        }
        vTaskDelayUntil(&previous, pdMS_TO_TICKS(5));
    }
}
}

bool button_reboot::begin(int pin) {
    if (task) return true;
    buttonPin = pin;
    const bool ok = xTaskCreatePinnedToCore(monitor, "button-reboot", 3072,
                                            nullptr, 2, &task, 0) == pdPASS;
    if (!ok) task = nullptr;
    Serial.printf("[reboot] GPIO%d: %s; five short clicks within 3 seconds\n",
                  pin, ok ? "READY" : "FAILED to start sampler");
    return ok;
}
#else
bool button_reboot::begin(int) { return false; }
#endif
