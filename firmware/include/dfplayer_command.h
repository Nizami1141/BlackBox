#pragma once
#include <stdint.h>
namespace dfplayer {
inline void command(uint8_t *out, uint8_t code, uint16_t parameter) {
    out[0] = 0x7e; out[1] = 0xff; out[2] = 0x06; out[3] = code; out[4] = 0;
    out[5] = uint8_t(parameter >> 8); out[6] = uint8_t(parameter);
    uint16_t sum = 0;
    for (unsigned i = 1; i <= 6; ++i) sum += out[i];
    sum = uint16_t(0 - sum);
    out[7] = uint8_t(sum >> 8); out[8] = uint8_t(sum); out[9] = 0xef;
}
}
