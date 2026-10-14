#pragma once

#include <stdint.h>

// Independent of Arduino: only complete, debounced short press/release
// cycles count toward a restart. A held button at reset must be released.
class ButtonPressFilter {
public:
    void reset(uint32_t now, bool down) {
        raw_ = stable_ = down;
        changed_ = now;
        waitForRelease_ = true;
        candidate_ = false;
    }

    bool sample(uint32_t now, bool down, uint32_t &pressedAt) {
        if (down != raw_) {
            raw_ = down;
            changed_ = now;
        }
        if (uint32_t(now - changed_) < 30) return false;
        if (waitForRelease_) {
            stable_ = down;
            if (!down) waitForRelease_ = false;
            return false;
        }
        if (down == stable_) return false;
        stable_ = down;
        if (down) {
            downAt_ = now;
            candidate_ = true;
            return false;
        }
        const bool valid = candidate_ && uint32_t(now - downAt_) >= 80 &&
                           uint32_t(now - downAt_) < 2000;
        candidate_ = false;
        if (valid) pressedAt = downAt_;
        return valid;
    }

private:
    bool raw_ = false;
    bool stable_ = false;
    bool waitForRelease_ = true;
    bool candidate_ = false;
    uint32_t changed_ = 0;
    uint32_t downAt_ = 0;
};
