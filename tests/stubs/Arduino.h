#pragma once
#include <cstdint>

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT_PULLUP = 2;
constexpr int INPUT_PULLDOWN = 3;

uint32_t millis();
void delay(uint32_t ms);
void pinMode(int pin, int mode);
int digitalRead(int pin);

// Intentionally no interrupt/restart API: button.cpp must use polling only.
