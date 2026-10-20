#include "audio_protocol.h"
#include "dfplayer_command.h"
#include <assert.h>
#include <stdio.h>
using namespace audio_wire;

static bool parse(Parser &parser, const uint8_t *data, size_t size, Frame &frame,
                  uint32_t start = 1000) {
    bool received = false;
    for (size_t i = 0; i < size; ++i) received |= parser.feed(data[i], start + uint32_t(i / 8), frame);
    return received;
}
int main() {
    // Independent published CRC vector, not only an encoder/decoder round trip.
    assert(crc(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0x29b1);
    uint8_t bytes[MAX_FRAME], payload[MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = uint8_t(i);
    Frame frame;
    Parser parser;
    size_t size = encode(bytes, BARCODE, 0x12345678, payload, MAX_PAYLOAD);
    assert(size == MAX_FRAME);
    assert(parse(parser, bytes, size, frame));
    assert(frame.session == 0x12345678 && frame.length == MAX_PAYLOAD);
    assert(memcmp(frame.data, payload, MAX_PAYLOAD) == 0);
    assert(encode(bytes, PLAY, 0, nullptr, 1) == 0);
    assert(encode(bytes, PLAY, 0, payload, MAX_PAYLOAD + 1) == 0);

    const uint8_t barcode[] = "CARGO-123";
    size = encode(bytes, BARCODE, 42, barcode, sizeof(barcode) - 1);
    // Every single-bit corruption must be rejected, including CRC/header.
    for (size_t i = 0; i < size; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            parser.reset(); bytes[i] ^= uint8_t(1 << bit);
            assert(!parse(parser, bytes, size, frame));
            bytes[i] ^= uint8_t(1 << bit);
        }
    }
    parser.reset();
    assert(parse(parser, bytes, size, frame));
    assert(barcodeFor(frame, 42));
    assert(!barcodeFor(frame, 41));  // previous verification
    assert(!barcodeFor(frame, 0));   // idle / canceled / timed out
    frame.type = ARMED;
    assert(!barcodeFor(frame, 42));
    frame.type = BARCODE; frame.data[0] = 0;
    assert(!barcodeFor(frame, 42));
    assert(!validBarcode(reinterpret_cast<const uint8_t *>("+++"), 3));
    assert(!validBarcode(reinterpret_cast<const uint8_t *>("AB"), 2));

    // Noise, repeated magic, truncated frames, oversized length and recovery.
    parser.reset();
    uint8_t noise[] = {0, 0x5a, 0xa5, 0xa5};
    assert(!parse(parser, noise, sizeof(noise), frame));
    assert(parse(parser, bytes, size, frame));
    assert(parse(parser, bytes, size, frame)); // consecutive frames
    parser.reset();
    assert(!parse(parser, bytes, 8, frame));
    assert(parse(parser, bytes, size, frame, 2000)); // expired partial frame
    uint8_t oversized[] = {0xa5, 0x5a, 1, 1, 0, 0, 0, 0, 0xff, 0xff};
    assert(!parse(parser, oversized, sizeof(oversized), frame));
    assert(parse(parser, bytes, size, frame));
    parser.reset();
    assert(!parse(parser, bytes, 8, frame, 0xfffffff0));
    assert(parse(parser, bytes, size, frame, 200)); // timer rollover

    // Vendor protocol example: /mp3/0001.mp3, command 0x12.
    uint8_t player[10];
    dfplayer::command(player, 0x12, 1);
    const uint8_t expected[] = {0x7e, 0xff, 0x06, 0x12, 0, 0, 1, 0xfe, 0xe8, 0xef};
    assert(memcmp(player, expected, sizeof(expected)) == 0);
    assert(validPrompt(1) && validPrompt(9) && !validPrompt(0) && !validPrompt(10));
    puts("PASS: CRC, corruption, bounds, parser recovery, stale sessions and DFPlayer command");
}
