#pragma once

#include <Arduino.h>

// Надёжный обмен кадрами между двумя ESP32 поверх ESP-NOW.
// Сеть Wi-Fi и роутер не требуются: платы находят друг друга broadcast-пакетом.
namespace interboard {

enum class Role : uint8_t {
    CAMERA_NODE,
    GATEWAY,
};

bool begin(Role role);
void loop();
bool paired();
String peerMac();

// CAMERA_NODE: получить номер запроса, который надо обслужить.
bool takeCaptureRequest(uint16_t &session);

// CAMERA_NODE: отправить уже снятый JPEG. Передача идёт с подтверждением
// каждого блока и повторами, затем проверяется CRC32 всего изображения.
bool sendFrame(uint16_t session, const uint8_t *jpeg, size_t len,
               uint16_t width, uint16_t height);

// CAMERA_NODE: быстро сообщить шлюзу, что камера не смогла снять кадр.
void sendCaptureError(uint16_t session, const String &message);

// GATEWAY: попросить S3 снять кадр и дождаться полного JPEG.
// Успешный вызов передаёт владение *jpeg вызывающему; освободить через free().
bool requestFrame(uint8_t **jpeg, size_t *len, uint16_t *width,
                  uint16_t *height, String &error);

}  // namespace interboard
