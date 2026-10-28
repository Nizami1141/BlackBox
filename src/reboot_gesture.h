#pragma once
#include <stdint.h>

// No Arduino/ISR dependencies: feed sampled logical button states.
class RebootGesture {
public:
    static constexpr uint32_t WINDOW_MS = 3000;
    static constexpr unsigned CLICKS = 5;

    void reset(bool down, uint32_t now) {
        raw_ = stable_ = down;
        changedAt_ = pressedAt_ = firstAt_ = now;
        armed_ = candidate_ = false;
        count_ = 0;
    }

    bool sample(bool down, uint32_t now) {
        if (count_ && uint32_t(now - firstAt_) > WINDOW_MS) count_ = 0;
        if (down != raw_) { raw_ = down; changedAt_ = now; return false; }
        if (raw_ == stable_) {
            // A held input at startup must first be released. Require a real
            // release between clicks; raw electrical edges do not count.
            if (!stable_ && uint32_t(now - changedAt_) >= RELEASE_MS) armed_ = true;
            if (stable_ && uint32_t(now - pressedAt_) > MAX_PRESS_MS) {
                candidate_ = false;
                count_ = 0;
            }
            return false;
        }
        if (uint32_t(now - changedAt_) < DEBOUNCE_MS) return false;
        stable_ = raw_;
        if (stable_) {
            pressedAt_ = now;
            candidate_ = armed_;
            armed_ = false;
        } else {
            const uint32_t duration = now - pressedAt_;
            if (candidate_ && duration >= MIN_PRESS_MS && duration <= MAX_PRESS_MS) {
                if (!count_) firstAt_ = pressedAt_;
                if (++count_ == CLICKS) {
                    count_ = 0;
                    candidate_ = false;
                    return true;
                }
            } else if (candidate_) count_ = 0;
            candidate_ = false;
        }
        return false;
    }
private:
    static constexpr uint32_t DEBOUNCE_MS = 30;
    static constexpr uint32_t RELEASE_MS = 100;
    static constexpr uint32_t MIN_PRESS_MS = 80;
    static constexpr uint32_t MAX_PRESS_MS = 1000;
    bool raw_ = false, stable_ = false, armed_ = false, candidate_ = false;
    uint32_t changedAt_ = 0, pressedAt_ = 0, firstAt_ = 0;
    unsigned count_ = 0;
};
