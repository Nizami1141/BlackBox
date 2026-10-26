#pragma once

#include <Arduino.h>

class GM67;

namespace diagnostics {

// All calls run on the checkpoint task; reports never capture or submit data.
void begin(GM67 *scanner, Stream *primary, Stream *alternate = nullptr);
void ready();
void tick();
void report();
void stage(const char *name);
void event(const char *component, const char *message);
void cameraInit(bool ok);
void capture(bool ok, uint16_t width, uint16_t height, size_t bytes);
void barcode(bool received, bool canceled, uint32_t elapsedMs, size_t length);
void serverResult(const char *operation, bool ok, int httpCode);
void startCheck(uint32_t id);
void finishCheck(const char *outcome);

}  // namespace diagnostics
