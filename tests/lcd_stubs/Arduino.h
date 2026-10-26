#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

using String = std::string;
constexpr int LOW = 0, HIGH = 1;
constexpr int INPUT = 0, OUTPUT_OPEN_DRAIN = 1, INPUT_PULLUP = 2, INPUT_PULLDOWN = 3;
uint32_t millis();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);

struct TestSerial {
    std::string output;
    template<class T> void print(const T &value) {
        std::ostringstream text;
        text << value;
        output += text.str();
    }
    void println() { output += '\n'; }
    template<class T> void println(const T &value) { print(value); println(); }
    template<class... Args> void printf(const char *format, Args... args) {
        char text[1024];
        std::snprintf(text, sizeof(text), format, args...);
        output += text;
    }
    bool has(const char *text) const { return output.find(text) != std::string::npos; }
};
extern TestSerial Serial;
