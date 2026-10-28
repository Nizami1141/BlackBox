// Isolate GPIO21 from the camera, W5500, LCD, scanner and main application.
// GPIO21 stays INPUT_PULLUP; this diagnostic never drives the pin high/low.
#include <Arduino.h>
#include "button.h"

namespace {
constexpr int TEST_PIN = 21;
uint32_t highSamples = 0, lowSamples = 0, edges = 0, clicks = 0, holds = 0;
uint32_t lastReport = 0;
int lastRaw = HIGH;
}

void setup() {
    Serial.begin(115200);
    btn::begin(TEST_PIN, true);
    lastRaw = digitalRead(TEST_PIN);
    Serial.println("[button-test] GPIO21 INPUT_PULLUP; native USB; no camera/network/LCD/scanner");
    Serial.println("[button-test] First leave GPIO21 disconnected: expect HIGH and armed=YES.");
    Serial.println("[button-test] Then button between GPIO21 and GND: press 0.5 s and release.");
    Serial.println("[button-test] Do not connect GPIO21 to 5V. Test image replaces application until reflashed.");
}

void loop() {
    const int raw = digitalRead(TEST_PIN);
    if (raw == HIGH) ++highSamples;
    else ++lowSamples;
    if (raw != lastRaw) { ++edges; lastRaw = raw; }
    if (btn::pressed()) { ++clicks; Serial.println("[button-test] CLICK accepted"); }
    if (btn::longPressed()) { ++holds; Serial.println("[button-test] HOLD >=2s (not a click)"); }
    if (millis() - lastReport >= 1000) {
        lastReport = millis();
        const bool down = btn::isDown();
        const bool armed = btn::armed();
        Serial.printf("[button-test] GPIO21 raw=%s; high=%lu low=%lu edges=%lu; "
                      "down=%s armed=%s; clicks=%lu holds=%lu\n",
                      raw == HIGH ? "HIGH" : "LOW", (unsigned long)highSamples,
                      (unsigned long)lowSamples, (unsigned long)edges,
                      down ? "YES" : "NO", armed ? "YES" : "NO",
                      (unsigned long)clicks, (unsigned long)holds);
        highSamples = lowSamples = edges = 0;
    }
    delay(1);
}
