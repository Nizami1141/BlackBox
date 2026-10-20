#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Bounded binary frames: A5 5A version type session(LE32) length(LE16)
// payload CRC16-CCITT(LE16). CRC covers version through payload.
namespace audio_wire {
constexpr size_t MAX_PAYLOAD = 256;
constexpr size_t MAX_FRAME = MAX_PAYLOAD + 12;
enum Type : uint8_t { PLAY = 1, ARM = 2, STOP_SCAN = 3, ARMED = 4, BARCODE = 5 };
enum class Prompt : uint8_t {
    LOOK = 1, CHECKING = 2, SCAN = 3, DENIED = 4, NETWORK_ERROR = 5,
    SAVED = 6, SYSTEM_ERROR = 7, TIMEOUT = 8, CANCELED = 9
};
inline bool validPrompt(uint8_t value) { return value >= 1 && value <= 9; }
inline void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint32_t get32(const uint8_t *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8 * i);
    return value;
}
inline uint16_t crc(const uint8_t *data, size_t size) {
    uint16_t value = 0xffff;
    while (size--) {
        value ^= uint16_t(*data++) << 8;
        for (unsigned i = 0; i < 8; ++i)
            value = (value & 0x8000) ? uint16_t((value << 1) ^ 0x1021) : uint16_t(value << 1);
    }
    return value;
}
struct Frame {
    uint8_t type = 0;
    uint32_t session = 0;
    uint16_t length = 0;
    uint8_t data[MAX_PAYLOAD] = {};
};
inline size_t encode(uint8_t *out, uint8_t type, uint32_t session,
                     const uint8_t *data = nullptr, size_t length = 0) {
    if (length > MAX_PAYLOAD || (length && !data)) return 0;
    out[0] = 0xa5; out[1] = 0x5a; out[2] = 1; out[3] = type;
    put32(out + 4, session);
    out[8] = static_cast<uint8_t>(length); out[9] = static_cast<uint8_t>(length >> 8);
    if (length) memcpy(out + 10, data, length);
    const uint16_t check = crc(out + 2, length + 8);
    out[10 + length] = static_cast<uint8_t>(check);
    out[11 + length] = static_cast<uint8_t>(check >> 8);
    return length + 12;
}
class Parser {
public:
    void reset() { used_ = 0; }
    bool feed(uint8_t byte, uint32_t now, Frame &frame) {
        if (used_ && uint32_t(now - last_) > 100) reset();
        last_ = now;
        if (used_ == 0) { if (byte == 0xa5) data_[used_++] = byte; return false; }
        if (used_ == 1 && byte != 0x5a) {
            used_ = byte == 0xa5 ? 1 : 0;
            return false;
        }
        data_[used_++] = byte;
        if (used_ == 3 && byte != 1) { reset(); return false; }
        if (used_ < 10) return false;
        const size_t length = size_t(data_[8]) | (size_t(data_[9]) << 8);
        if (length > MAX_PAYLOAD) { reset(); return false; }
        if (used_ < length + 12) return false;
        const uint16_t check = uint16_t(data_[10 + length]) | uint16_t(data_[11 + length]) << 8;
        const bool ok = crc(data_ + 2, length + 8) == check;
        if (ok) {
            frame.type = data_[3]; frame.session = get32(data_ + 4);
            frame.length = static_cast<uint16_t>(length);
            if (length) memcpy(frame.data, data_ + 10, length);
        }
        reset();
        return ok;
    }
private:
    uint8_t data_[MAX_FRAME] = {};
    size_t used_ = 0;
    uint32_t last_ = 0;
};
inline bool validBarcode(const uint8_t *data, size_t length) {
    if (length < 3 || length > MAX_PAYLOAD) return false;
    bool alnum = false;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = data[i];
        if (c < 0x20 || c >= 0x7f) return false;
        alnum |= (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    return alnum;
}
inline bool barcodeFor(const Frame &frame, uint32_t activeSession) {
    return activeSession && frame.session == activeSession && frame.type == BARCODE &&
           validBarcode(frame.data, frame.length);
}
}  // namespace audio_wire
