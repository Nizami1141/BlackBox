#include "../src/button.h"
#include <cassert>
#include <iostream>

namespace {
uint32_t nowMs = 0;
int level = HIGH;
unsigned events = 0;

void reset(int initial = HIGH) {
    nowMs = 100;
    level = initial;
    btn::begin(14);
    events = 0;
    nowMs += 150;
    (void)btn::isDown(); // Observe a released input before accepting a click.
}

void sample(int nextLevel, uint32_t elapsed) {
    level = nextLevel;
    nowMs += elapsed;
    if (btn::pressed()) ++events;
}

void click() {
    sample(LOW, 1);
    sample(LOW, 35);
    sample(HIGH, 1);
    sample(HIGH, 35);
    sample(HIGH, 150);
}
}

uint32_t millis() { return nowMs; }
void delay(uint32_t ms) { nowMs += ms; }
void pinMode(int, int) {}
int digitalRead(int) { return level; }

int main() {
    reset();
    click();
    assert(events == 1);
    assert(!btn::pressed());

    // Serial status samples levels without consuming a queued short press.
    reset();
    level = LOW;
    (void)btn::isDown();
    nowMs += 35;
    assert(btn::isDown());
    level = HIGH;
    (void)btn::isDown();
    nowMs += 35;
    assert(!btn::isDown());
    assert(!btn::isDown());
    assert(btn::pressed());
    assert(!btn::pressed());

    // Repeated real clicks stay ordinary events; there is no reset path.
    reset();
    for (unsigned i = 0; i < 20; ++i) click();
    assert(events == 20);

    // Electrical edges shorter than the debounce window are ignored.
    reset();
    for (unsigned i = 0; i < 100; ++i) {
        sample(LOW, 5);
        sample(HIGH, 5);
    }
    sample(HIGH, 35);
    assert(events == 0);
    click();
    assert(events == 1);

    // A click during an expired scan window must not auto-start a new check.
    reset();
    level = LOW;
    (void)btn::longPressed();
    nowMs += 35;
    (void)btn::longPressed();
    level = HIGH;
    (void)btn::longPressed();
    nowMs += 35;
    (void)btn::longPressed(); // The release has queued a short click.
    btn::discardPending();
    assert(!btn::pressed());
    sample(HIGH, 150);
    click();
    assert(events == 1);

    // Releasing a button held at timeout must not auto-start either.
    reset();
    level = LOW;
    btn::discardPending();
    sample(HIGH, 1);
    sample(HIGH, 35);
    assert(events == 0);
    sample(HIGH, 150);
    click();
    assert(events == 1);

    // Regression: idle only calls pressed(), not longPressed(). A long LOW
    // followed by release must not be turned into a fresh short click.
    reset();
    sample(LOW, 1);
    sample(LOW, 35);
    sample(LOW, 120000);
    sample(HIGH, 1);
    sample(HIGH, 35);
    assert(events == 0);
    sample(HIGH, 150);
    click();
    assert(events == 1);

    // Long press cancels once and does not also start a new check on release.
    reset();
    sample(LOW, 1);
    sample(LOW, 35);
    nowMs += 2000;
    assert(btn::longPressed());
    assert(!btn::longPressed());
    sample(HIGH, 1);
    sample(HIGH, 35);
    assert(events == 0);
    sample(HIGH, 150);
    click();
    assert(events == 1);

    // A held button at boot is not an authorized click or cancellation.
    reset(LOW);
    assert(!btn::longPressed());
    nowMs += 2000;
    assert(!btn::longPressed());
    sample(HIGH, 1);
    sample(HIGH, 35);
    assert(events == 0);
    sample(HIGH, 150);
    assert(btn::armed());
    click();
    assert(events == 1);

    // Brief HIGH noise on a stuck LOW line cannot rearm the input.
    reset(LOW);
    sample(HIGH, 1);
    sample(HIGH, 35);
    sample(LOW, 1);
    sample(LOW, 35);
    sample(HIGH, 1);
    sample(HIGH, 35);
    assert(events == 0);

    // The first release from LOW at boot also must not emit a short click.
    reset(LOW);
    sample(HIGH, 1);
    sample(HIGH, 35);
    assert(events == 0);

    // Active-high callers still work; production remains active-low.
    nowMs = 100;
    level = LOW;
    btn::begin(14, false);
    nowMs += 150;
    assert(btn::armed() && btn::rawLevel() == LOW);
    sample(HIGH, 1);
    sample(HIGH, 35);
    sample(LOW, 1);
    sample(LOW, 35);
    assert(events == 1);

    // Unsigned elapsed-time arithmetic still works across millis() rollover.
    reset();
    nowMs = UINT32_MAX - 20;
    click();
    assert(events == 1);

    // Checkpoint starts on the FIRST debounced press, before any release.
    reset();
    level = LOW;
    assert(!btn::startPressed());
    nowMs += 35;
    (void)btn::isDown(); // Diagnostics can observe the edge first.
    assert(btn::startPressed());
    assert(!btn::startPressed());
    nowMs += 5000;
    assert(!btn::startPressed());
    assert(!btn::longPressed()); // The start hold is not barcode cancellation.
    level = HIGH;
    assert(!btn::startPressed());
    nowMs += 35;
    assert(!btn::startPressed() && !btn::pressed());
    nowMs += 150;
    assert(btn::armed());
    level = LOW;
    assert(!btn::startPressed());
    nowMs += 35;
    assert(btn::startPressed()); // Next physical press is accepted once.

    // No restart on release after a check completes while still held.
    btn::discardPending();
    nowMs += 5000;
    assert(!btn::startPressed());
    level = HIGH;
    assert(!btn::startPressed());
    nowMs += 35;
    assert(!btn::startPressed());
    nowMs += 150;
    assert(btn::armed());

    // Boot-held and sub-debounce noise never start a check.
    reset(LOW);
    nowMs += 5000;
    assert(!btn::startPressed());
    reset();
    for (unsigned i = 0; i < 50; ++i) {
        level = LOW; nowMs += 5; assert(!btn::startPressed());
        level = HIGH; nowMs += 5; assert(!btn::startPressed());
    }
    std::cout << "Button polling: release clicks and immediate checkpoint start/hold/noise/rearm passed\n";
}
