#include "audio_companion.h"
#if ENABLE_AUDIO_COMPANION
#include <esp_system.h>

namespace {
HardwareSerial boardUart(1);
audio_wire::Parser parser;
uint32_t session = 0, started = 0, window = 0, lastArm = 0;
bool linkSeen = false;
void send(uint8_t type, uint32_t id, const uint8_t *data = nullptr, size_t length = 0) {
    uint8_t bytes[audio_wire::MAX_FRAME];
    const size_t count = audio_wire::encode(bytes, type, id, data, length);
    if (count) boardUart.write(bytes, count);
}
void arm(uint32_t remaining) {
    uint8_t payload[4]; audio_wire::put32(payload, remaining);
    send(audio_wire::ARM, session, payload, sizeof(payload));
    lastArm = millis();
}
}
namespace audio_companion {
void begin() {
    boardUart.setRxBufferSize(2048);
    boardUart.begin(AUDIO_LINK_BAUD, SERIAL_8N1, AUDIO_LINK_RX_PIN, AUDIO_LINK_TX_PIN);
    send(audio_wire::STOP_SCAN, 0);
    Serial.printf("[audio] companion UART1 RX=%d TX=%d, %d baud; scanner on second ESP32\n",
                  AUDIO_LINK_RX_PIN, AUDIO_LINK_TX_PIN, AUDIO_LINK_BAUD);
}
void say(audio_wire::Prompt prompt) {
    const uint8_t track = static_cast<uint8_t>(prompt);
    send(audio_wire::PLAY, 0, &track, 1);
}
void stopScan() {
    if (session) send(audio_wire::STOP_SCAN, session);
    session = 0;
    parser.reset();
}
void startScan(uint32_t windowMs) {
    stopScan();
    while (boardUart.available()) boardUart.read();
    do { session = esp_random(); } while (!session);
    started = millis(); window = windowMs > 120000 ? 120000 : windowMs;
    linkSeen = false;
    arm(window);
}
bool scanLinkSeen() { return linkSeen; }
bool pollBarcode(String &code) {
    if (!session) return false;
    const uint32_t elapsed = millis() - started;
    if (elapsed >= window) { stopScan(); return false; }
    audio_wire::Frame frame;
    while (boardUart.available()) {
        if (!parser.feed(static_cast<uint8_t>(boardUart.read()), millis(), frame)) continue;
        if (frame.session != session) continue;  // stale scan from another verification
        if (frame.type == audio_wire::ARMED && frame.length == 0) linkSeen = true;
        if (audio_wire::barcodeFor(frame, session)) {
            code = "";
            for (size_t i = 0; i < frame.length; ++i) code += static_cast<char>(frame.data[i]);
            linkSeen = true;
            stopScan();  // acknowledges barcode and stops retransmissions
            return true;
        }
    }
    // Re-arm with the SAME nonce and remaining time: survives a node reset,
    // does not clear an already received barcode on the second board.
    if (millis() - lastArm >= 500) arm(window - elapsed);
    return false;
}
}
#else
namespace audio_companion {
void begin() {}
void say(audio_wire::Prompt) {}
void startScan(uint32_t) {}
bool pollBarcode(String &) { return false; }
void stopScan() {}
bool scanLinkSeen() { return false; }
}
#endif
