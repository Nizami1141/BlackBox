#pragma once

namespace button_reboot {
// Start after checkpoint boot, when btn::begin has configured INPUT_PULLUP.
// Samples independently of blocking camera/HTTP operations on the main task.
bool begin(int pin);
}
