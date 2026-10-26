#include <Arduino.h>

#include "gm67.h"

namespace {
constexpr int kRxPin = 1;   // GM67 TX -> ESP32-S3 GPIO1
constexpr int kTxPin = 2;   // ESP32-S3 GPIO2 -> GM67 RX
constexpr uint32_t kScanWindowMs = 30000;

GM67 scanner;
bool commandAck = false;
bool barcodeSeen = false;
bool testRunning = false;
uint32_t scanStartedMs = 0;
uint32_t lastStartMs = 0;

void printStatus() {
    Serial.printf("[STATUS] GM67 command ACK=%s, barcode=%s, scan RX bytes=%lu, dropped=%lu\n",
                  commandAck ? "PASS" : "FAIL/UNKNOWN",
                  barcodeSeen ? "PASS" : "WAIT/FAIL",
                  static_cast<unsigned long>(scanner.rxBytes()),
                  static_cast<unsigned long>(scanner.droppedBytes()));
    Serial.println("[INFO] UART loopback PASS proves GPIO1/2; this test checks the scanner itself.");
}

void startTest() {
    if (testRunning) {
        Serial.println("[INFO] Test is already running");
        return;
    }
    commandAck = false;
    barcodeSeen = false;
    Serial.println("[TEST] GM67 on UART1, ESP RX=GPIO1 <- GM67 TX, ESP TX=GPIO2 -> GM67 RX, 9600 8N1");
    Serial.println("[INFO] No jumper between GPIO1 and GPIO2; GM67 connected; temporary settings only.");

    // GM67 may need roughly two seconds after power-on before it can answer.
    scanner.flushInput();
    scanner.wakeHard();

    const bool serialAck = scanner.sendParam2(GM67::PX_PREFIX, 0x01, 0x00, false);
    Serial.printf("[%s] GM67 serial-interface command ACK\n", serialAck ? "PASS" : "WARN");
    const bool hostAck = scanner.sendParam1(GM67::P_TRIGGER_MODE, 0x08, false);
    Serial.printf("[%s] GM67 host-mode command ACK\n", hostAck ? "PASS" : "WARN");
    const bool asciiAck = scanner.sendParam1(GM67::P_DECODE_FMT, 0x00, false);
    Serial.printf("[%s] GM67 ASCII-output command ACK\n", asciiAck ? "PASS" : "WARN");
    const bool terminatorAck = scanner.sendParam2(GM67::PX_PREFIX, GM67::PX_TERMINATOR, 0x01, false);
    Serial.printf("[%s] GM67 CR/LF command ACK\n", terminatorAck ? "PASS" : "WARN");
    commandAck = serialAck || hostAck || asciiAck || terminatorAck;
    Serial.printf("[%s] GM67 bidirectional UART: at least one valid command ACK\n",
                  commandAck ? "PASS" : "FAIL");
    if (!commandAck) {
        Serial.println("[HINT] Check GM67 power, common GND, crossed TX/RX, 3.3 V signal levels, UART mode and 9600 baud.");
    }

    scanner.flushInput();
    scanner.startDecode();
    scanStartedMs = millis();
    lastStartMs = scanStartedMs;
    testRunning = true;
    Serial.println("[ACTION] Show GM67 any barcode during the next 30 seconds.");
}

void finishTest() {
    testRunning = false;
    scanner.stopDecode();
    printStatus();
    if (commandAck && barcodeSeen) {
        Serial.println("RESULT: PASS — command response and barcode received");
    } else if (commandAck) {
        Serial.println("RESULT: PARTIAL — UART command/ACK works; barcode not received");
    } else if (barcodeSeen) {
        Serial.println("RESULT: PARTIAL — barcode received; command ACK not confirmed");
    } else {
        Serial.println("RESULT: FAIL — no command ACK and no barcode");
    }
    Serial.println("GM67_TEST_DONE");
}
}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(250);
    delay(1200);
    scanner.begin(1, kRxPin, kTxPin, 9600);
    scanner.setMinLength(3);
    scanner.setDebug(true);
    Serial.println("ESP32-S3 GM67 CHECK — scanner only, no loopback jumper");
    Serial.println("Commands: TEST, STATUS (send via tools/gm67_command.py)");
}

void loop() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toUpperCase();
        if (command == "TEST") startTest();
        else if (command == "STATUS") printStatus();
        else if (command.length()) Serial.println("Commands: TEST, STATUS");
    }

    if (testRunning) {
        String code;
        if (scanner.poll(code)) {
            barcodeSeen = true;
            Serial.printf("[PASS] GM67 barcode received: %s\n", code.c_str());
            finishTest();
        } else if (millis() - scanStartedMs >= kScanWindowMs) {
            finishTest();
        } else if (millis() - lastStartMs >= 5000 && scanner.idle()) {
            scanner.startDecode();
            lastStartMs = millis();
            Serial.println("[INFO] GM67 scan restarted; keep barcode in view");
        }
    }
    delay(5);
}
