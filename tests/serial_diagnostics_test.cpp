#include "../src/serial_diagnostics.h"
#include "../src/gm67.h"
#include "../src/camera.h"
#include "../src/eth_link.h"
#include "../src/lcd.h"
#include <cassert>
#include <iostream>
#include <string>

TestESP ESP;
namespace {
uint32_t nowMs = 100;
bool lcdAck = true;
bool lcdReady = true;
bool spiOk = true;
bool linkOk = true;
bool buttonDown = false;
unsigned buttonClicks = 1;
unsigned probes = 0;
unsigned lcdChecks = 0;
HardwareSerial scannerPort;

class Terminal : public Stream {
public:
    std::string input, output;
    size_t cursor = 0;
    int available() override { return (int)(input.size() - cursor); }
    int read() override { return available() ? (unsigned char)input[cursor++] : -1; }
    size_t print(const char *text) override { output += text; return std::string(text).size(); }
    bool has(const char *text) const { return output.find(text) != std::string::npos; }
    void clear() { output.clear(); }
};
}  // namespace

uint32_t millis() { return nowMs; }
void GM67::begin(int, int, int, uint32_t) { _ser = &scannerPort; }
namespace btn {
bool isDown() { return buttonDown; }
int rawLevel() { return buttonDown ? 0 : 1; }
bool armed() { return !buttonDown; }
// Consuming a click would make the test fail: reporting must only observe.
bool pressed() { if (buttonClicks) { --buttonClicks; return true; } return false; }
}
namespace cam {
const char *stateName() { return "OFF"; }
uint32_t shotsTaken() { return 0; }
size_t lastLen() { return 0; }
uint32_t rejected() { return 0; }
}
namespace lcd {
bool ok() { return lcdReady; }
bool probe() { ++probes; return lcdAck; }
uint8_t address() { return 0x27; }
uint32_t errors() { return lcdAck ? 0 : 1; }
}
namespace checkpoint {
bool diagnoseLCD() { ++lcdChecks; return lcdReady; }
}
namespace eth {
bool hardwareResponding() { return spiOk; }
bool asleep() { return false; }
bool linkUp() { return linkOk; }
String localIP() { return "172.16.5.193"; }
}

int main() {
    Terminal uart, usb;
    GM67 scanner;
    scanner.begin(1, 1, 2, 9600);
    scannerPort.queued = 12;
    diagnostics::begin(&scanner, &uart, &usb);
    uart.clear(); usb.clear();
    diagnostics::tick();
    assert(probes == 0); // No bus transactions before hardware initialization.

    diagnostics::cameraInit(true);
    diagnostics::serverResult("health-TCP-TLS", true, 0);
    diagnostics::ready();
    assert(uart.output == usb.output);
    assert(uart.has("init=OK; state=OFF")); // OFF in idle is not a camera failure.
    assert(uart.has("evidence=NOT_CONFIRMED")); // Open UART is not scanner proof.
    assert(uart.has("queued=12"));
    assert(uart.has("SPI_VERSION=OK(0x04)"));
    assert(uart.has("health=TCP/TLS only"));
    assert(scannerPort.reads == 0 && scannerPort.queued == 12);
    assert(buttonClicks == 1);

    uart.clear(); usb.clear();
    nowMs += 9999;
    diagnostics::tick();
    assert(uart.output.empty());
    ++nowMs;
    diagnostics::tick();
    assert(uart.has("=== uptime="));

    // Either serial interface can request a report; CRLF produces one report.
    uart.clear(); usb.clear();
    const unsigned before = probes;
    usb.input = "status\r\n";
    diagnostics::tick();
    assert(probes == before + 1 && uart.output == usb.output);
    assert(buttonClicks == 1 && scannerPort.reads == 0);

    // A disconnected device must never be reported as healthy.
    lcdAck = spiOk = linkOk = false;
    buttonDown = true;
    diagnostics::report();
    assert(uart.has("I2C_ACK=NO"));
    assert(uart.has("SPI_VERSION=FAIL; link=DOWN; IP=UNAVAILABLE"));
    assert(uart.has("BUTTON GPIO14=DOWN"));
    assert(uart.has("raw=LOW; armed=NO"));
    lcdReady = false;
    const unsigned disabledProbes = probes;
    diagnostics::report();
    assert(probes == disabledProbes);
    assert(uart.has("I2C_ACK=NOT_TESTED; driver=NOT_READY"));
    assert(uart.has("send LCDCHECK while idle"));

    // Manual discovery works from either terminal and does not consume a click/code.
    uart.input = "LCDCHECK\r\n";
    uart.cursor = 0;
    uart.clear(); usb.clear();
    diagnostics::tick();
    assert(lcdChecks == 1 && uart.has("LCDCHECK END result=NOT_READY"));
    assert(uart.output == usb.output && buttonClicks == 1 && scannerPort.reads == 0);
    lcdReady = lcdAck = true;
    usb.input = "lcdcheck\n";
    usb.cursor = 0;
    diagnostics::tick();
    assert(lcdChecks == 2 && uart.has("LCDCHECK END result=READY"));

    uart.clear(); usb.clear();
    diagnostics::startCheck(7);
    uart.input = "LCDCHECK\n";
    uart.cursor = 0;
    diagnostics::tick();
    assert(lcdChecks == 2 && uart.has("LCDCHECK BUSY"));
    diagnostics::stage("camera-capture");
    diagnostics::capture(false, 0, 0, 0);
    diagnostics::finishCheck("CAMERA_ERROR");
    diagnostics::report();
    assert(uart.has("frame=FAIL") && uart.has("failures=1"));
    assert(uart.has("END result=CAMERA_ERROR"));
    assert(uart.has("active=NO") && uart.has("last_result=CAMERA_ERROR; saved=0"));

    diagnostics::startCheck(8);
    diagnostics::capture(true, 2560, 1920, 200000);
    diagnostics::barcode(false, false, 15000, 0);
    diagnostics::barcode(false, true, 2000, 0);
    diagnostics::barcode(true, false, 5000, 12);
    diagnostics::serverResult("log_cargo", true, 200);
    diagnostics::finishCheck("SAVED");
    diagnostics::finishCheck("SAVED"); // Accidental duplicate cannot double count.
    uart.clear(); usb.clear();
    diagnostics::report();
    assert(uart.has("last_op=log_cargo; result=OK; HTTP=200"));
    assert(uart.has("last_result=SAVED; saved=1"));
    assert(uart.has("evidence=BARCODE_RX"));
    assert(uart.has("barcodes=1; timeouts=1"));

    // Reports continue across millis() rollover.
    buttonDown = false;
    diagnostics::tick();
    nowMs = UINT32_MAX - 5000;
    diagnostics::report();
    uart.clear(); usb.clear();
    nowMs = 4998;
    diagnostics::tick();
    assert(uart.output.empty());
    ++nowMs;
    diagnostics::tick();
    assert(uart.has("=== uptime="));

    // Long command input is bounded per tick, then rejected without overflow.
    uart.input = std::string(100, 'X') + "\n";
    uart.cursor = 0;
    uart.clear(); usb.clear();
    diagnostics::tick();
    assert(uart.cursor == 32);
    for (int i = 0; i < 3; ++i) diagnostics::tick();
    assert(uart.has("command too long"));
    // Edges appear before the next periodic summary, without consuming clicks.
    buttonDown = false;
    diagnostics::tick();
    uart.clear(); usb.clear();
    buttonDown = true;
    ++nowMs;
    diagnostics::tick();
    assert(uart.has("edge=DOWN; raw=LOW"));
    assert(!uart.has("=== uptime="));
    uart.clear(); usb.clear();
    nowMs += 2000;
    diagnostics::tick();
    assert(uart.has("held DOWN >=2s"));
    assert(buttonClicks == 1 && uart.output == usb.output);
    uart.clear(); usb.clear();
    ++nowMs;
    diagnostics::tick();
    assert(!uart.has("held DOWN"));
    buttonDown = false;
    diagnostics::tick();
    assert(uart.has("edge=UP; raw=HIGH"));
    assert(buttonClicks == 1);
    std::cout << "serial diagnostics: all checks passed\n";
}
