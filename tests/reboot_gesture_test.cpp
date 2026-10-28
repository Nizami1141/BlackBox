#include "../src/reboot_gesture.h"
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {
RebootGesture gesture;
uint32_t nowMs = 0;
unsigned resets = 0;
void sample(bool down, unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) {
        nowMs += 5;
        if (gesture.sample(down, nowMs)) ++resets;
    }
}
void reset(bool held = false, uint32_t start = 0) {
    nowMs = start; resets = 0;
    gesture.reset(held, nowMs);
    sample(held, 30);
}
void click() { sample(true, 30); sample(false, 30); } // 150ms down + 150ms up
}

int main() {
    reset();
    for (unsigned i = 0; i < 4; ++i) click();
    assert(resets == 0);
    // Fifth press alone does not restart: its release must be confirmed.
    sample(true, 30);
    assert(resets == 0);
    sample(false, 30);
    assert(resets == 1);
    sample(false, 2000);
    assert(resets == 1);

    // A pause expires the whole streak; later ordinary checks do not add up.
    reset();
    for (unsigned i = 0; i < 4; ++i) click();
    sample(false, 700);
    click();
    assert(resets == 0);
    for (unsigned i = 0; i < 4; ++i) click();
    assert(resets == 1);

    // Long holds, including a LOW input at boot, never count as a click.
    reset(true);
    sample(true, 1000);
    sample(false, 30);
    for (unsigned i = 0; i < 4; ++i) click();
    assert(resets == 0);
    reset();
    for (unsigned i = 0; i < 4; ++i) click();
    sample(true, 300);
    sample(false, 30);
    click();
    assert(resets == 0);

    // Neither contact bounce nor stable but extremely short pulses count.
    reset();
    for (unsigned i = 0; i < 100; ++i) { sample(true, 2); sample(false, 2); }
    assert(resets == 0);
    reset();
    for (unsigned i = 0; i < 20; ++i) { sample(true, 9); sample(false, 30); }
    assert(resets == 0);

    // Brief release glitches on a permanently held line cannot re-arm it.
    reset(true);
    for (unsigned i = 0; i < 30; ++i) { sample(false, 10); sample(true, 20); }
    assert(resets == 0);

    reset(false, UINT32_MAX - 500);
    for (unsigned i = 0; i < 5; ++i) click();
    assert(resets == 1);
    std::cout << "Five-click reboot: threshold/window/holds/boot/noise/rollover passed\n";
}
