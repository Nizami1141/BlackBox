#pragma once
#include <cstdint>

struct TestWire {
    bool beginOk = true;
    bool initialized = false;
    int ackAddress = -1;
    int address = 0;
    int sda = -1, scl = -1;
    bool payload = false;
    unsigned transactions = 0, writes = 0;
    bool begin(int data, int clock, uint32_t) {
        sda = data; scl = clock;
        initialized = beginOk;
        return initialized;
    }
    void end() { initialized = false; }
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t value) { address = value; payload = false; }
    void write(uint8_t) { ++writes; payload = true; }
    uint8_t endTransmission() {
        ++transactions;
        return !initialized ? 4 : (address == ackAddress ? 0 : 2);
    }
};
extern TestWire Wire;
