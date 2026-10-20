#pragma once
#include <Arduino.h>
#include "audio_config.h"
#include "audio_protocol.h"

namespace audio_companion {
void begin();
void say(audio_wire::Prompt prompt);
void startScan(uint32_t windowMs);
bool pollBarcode(String &code);
void stopScan();
bool scanLinkSeen();
}
