#include "../src/lcd.h"
#include <Wire.h>
#include <cassert>
#include <iostream>

TestSerial Serial;
TestWire Wire;
namespace {
uint32_t nowMs = 100;
int modes[64] = {}, values[64] = {};
}
uint32_t millis() { return nowMs; }
void delay(uint32_t ms) { nowMs += ms; }
void delayMicroseconds(uint32_t us) { nowMs += us / 1000; }
void pinMode(int pin, int mode) { modes[pin] = mode; }
void digitalWrite(int pin, int value) { values[pin] = value; }
int digitalRead(int pin) { return modes[pin] == OUTPUT_OPEN_DRAIN ? values[pin] : HIGH; }

int main() {
    // A failed controller initialization cannot proceed to a fictitious scan.
    Wire.beginOk = false;
    assert(!lcd::begin(47, 3));
    assert(Wire.transactions == 0);
    assert(Serial.has("Wire.begin FAILED"));
    assert(!lcd::ok() && lcd::address() == 0);

    // Manual retry uses saved pins and can recover without rebooting.
    Wire.beginOk = true;
    Wire.ackAddress = 0x27;
    assert(lcd::diagnose());
    assert(Wire.sda == 47 && Wire.scl == 3);
    assert(lcd::ok() && lcd::address() == 0x27);
    assert(lcd::probe());
    assert(Wire.writes > 0);

    // A later disconnect produces fresh raw diagnostics and an actual NACK.
    Serial.output.clear();
    Wire.ackAddress = -1;
    unsigned before = Wire.transactions;
    assert(!lcd::diagnose());
    assert(!lcd::ok() && lcd::address() == 0);
    assert(Serial.has("last_i2c_error=2"));
    assert(Serial.has("I2C (SDA=47, SCL=3)"));
    assert(Wire.transactions - before < 160); // Fixed-size, bounded address scans.

    Serial.output.clear();
    assert(!lcd::diagnose());
    assert(Serial.has("I2C (SDA=47, SCL=3)")); // Report is not suppressed on next command.

    // A different valid backpack address is discovered on the next attempt.
    Wire.ackAddress = 0x3f;
    assert(lcd::diagnose());
    assert(lcd::address() == 0x3f && lcd::ok());
    std::cout << "LCD manual recovery: initialization failure, retry, NACK, 0x27/0x3F passed\n";
}
