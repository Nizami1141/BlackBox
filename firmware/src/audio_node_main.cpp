// Standalone ESP32-S3 audio/scanner node. Native USB is REQUIRED: UART0
// controls DFPlayer, UART1 is the inter-board boardUart, UART2 is GM67.
#include <Arduino.h>
#include "audio_config.h"
#include "audio_protocol.h"
#include "dfplayer_command.h"
#include "gm67.h"

#if !ARDUINO_USB_CDC_ON_BOOT
#error "Audio node needs native USB Serial; UART0 is reserved for DFPlayer"
#endif

namespace {
HardwareSerial boardUart(1);
HardwareSerial player(0);
GM67 scanner;
audio_wire::Parser parser;
uint32_t session = 0, closedSession = 0, lastBarcode = 0, scanStart = 0, scanWindow = 0;
uint32_t playerStart = 0, lastPlayerCommand = 0;
uint32_t settleLastByte = 0;
bool settling = false, pendingBarcode = false, volumeSet = false;
uint8_t pendingTrack = 0;
String savedBarcode;

void send(uint8_t type, uint32_t id, const uint8_t *data = nullptr, size_t length = 0) {
    uint8_t bytes[audio_wire::MAX_FRAME];
    const size_t size = audio_wire::encode(bytes, type, id, data, length);
    if (size) boardUart.write(bytes, size);
}

// DFPlayer's documented 10-byte protocol, no ACK requested. Sending one
// command only queues UART bytes; playback never blocks scanner/boardUart service.
void playerCommand(uint8_t command, uint16_t value) {
    uint8_t bytes[10];
    dfplayer::command(bytes, command, value);
    player.write(bytes, sizeof(bytes));
    lastPlayerCommand = millis();
}

void audioTick() {
    // Let the player mount its SD card. A newer stage replaces an older
    // pending prompt; never announce a stale verdict after the stage changed.
    if (millis() - playerStart < 2000 || millis() - lastPlayerCommand < 150) return;
    if (!volumeSet) {
        playerCommand(0x06, AUDIO_PLAYER_VOLUME);
        volumeSet = true;
    } else if (pendingTrack) {
        playerCommand(0x12, pendingTrack);  // /mp3/0001.mp3 ... 0009.mp3
        Serial.printf("[audio] play /mp3/%04u.mp3 (command sent)\n", pendingTrack);
        pendingTrack = 0;
    }
    // Drain replies so an attached DFPlayer TX cannot fill the RX buffer.
    while (player.available()) player.read();
}

void closeScan() {
    if (session) closedSession = session;
    session = 0; settling = false; pendingBarcode = false;
    savedBarcode = "";
    scanner.flushInput();
}

void handle(const audio_wire::Frame &frame) {
    if (frame.type == audio_wire::PLAY && frame.length == 1 &&
        audio_wire::validPrompt(frame.data[0])) {
        pendingTrack = frame.data[0];
    } else if (frame.type == audio_wire::STOP_SCAN && frame.length == 0) {
        if (!frame.session || frame.session == session) closeScan();
    } else if (frame.type == audio_wire::ARM && frame.session && frame.length == 4 &&
               frame.session != closedSession) {
        const uint32_t duration = audio_wire::get32(frame.data);
        if (!duration || duration > 120000) return;
        if (session != frame.session) {
            closeScan();
            session = frame.session;
            scanner.flushInput();
            settling = true;
            settleLastByte = millis();
            scanStart = millis();
            Serial.println("[scanner] new verification: waiting for a fresh barcode");
        }
        // Same-session ARM retries never erase a barcode awaiting ACK.
        scanWindow = duration;
        scanStart = millis();
        send(audio_wire::ARMED, session);
    }
}
void scannerTick() {
    if (!session) {
        scanner.flushInput();
        return;
    }
    if (millis() - scanStart >= scanWindow) { closeScan(); return; }
    if (settling) {
        // Discard any scan already in flight when recognition completed.
        // Require 80 ms of UART silence before accepting a new scan.
        if (scanner.port()->available()) {
            scanner.flushInput();
            settleLastByte = millis();
        }
        if (millis() - settleLastByte < 80) return;
        settling = false;
    }
    if (!pendingBarcode) {
        String code;
        if (scanner.poll(code) && audio_wire::validBarcode(
                reinterpret_cast<const uint8_t *>(code.c_str()), code.length())) {
            savedBarcode = code;
            pendingBarcode = true;
            lastBarcode = millis() - 250;
        }
    }
    if (pendingBarcode && millis() - lastBarcode >= 250) {
        send(audio_wire::BARCODE, session,
             reinterpret_cast<const uint8_t *>(savedBarcode.c_str()), savedBarcode.length());
        lastBarcode = millis();
    }
}
}

void setup() {
    Serial.begin(115200);
    boardUart.setRxBufferSize(2048);
    boardUart.begin(AUDIO_LINK_BAUD, SERIAL_8N1, AUDIO_LINK_RX_PIN, AUDIO_LINK_TX_PIN);
    scanner.begin(2, AUDIO_SCANNER_RX_PIN, AUDIO_SCANNER_TX_PIN, 9600);
    scanner.setMinLength(3);
    scanner.setGuardMs(0);
    player.begin(9600, SERIAL_8N1, AUDIO_PLAYER_RX_PIN, AUDIO_PLAYER_TX_PIN);
    playerStart = millis();
    Serial.println("[audio] ESP32-S3 companion; GM67 passive UART2; DFPlayer UART0");
    Serial.println("[audio] boardUart RX1/TX2; GM67 RX16/TX17; DFPlayer RX18/TX21");
}

void loop() {
    audio_wire::Frame frame;
    // Bound work even if the boardUart is flooded with garbage.
    for (unsigned i = 0; i < 512 && boardUart.available(); ++i) {
        if (parser.feed(static_cast<uint8_t>(boardUart.read()), millis(), frame)) {
            handle(frame);
        }
    }
    scannerTick();
    audioTick();
    delay(1);
}
