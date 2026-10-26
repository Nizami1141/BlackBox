#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

using String = std::string;
class Stream {
public:
    virtual ~Stream() = default;
    virtual int available() = 0;
    virtual int read() = 0;
    virtual size_t print(const char *) = 0;
};
class HardwareSerial : public Stream {
public:
    int available() override { return queued; }
    int read() override { ++reads; return -1; }
    size_t print(const char *) override { return 0; }
    int queued = 0;
    unsigned reads = 0;
};
struct TestESP {
    unsigned getFreeHeap() { return 100000; }
    unsigned getFreePsram() { return 6000000; }
    unsigned getPsramSize() { return 8000000; }
};
extern TestESP ESP;
uint32_t millis();
