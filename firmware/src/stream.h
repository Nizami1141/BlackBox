// HTTP-сервер с видеопотоком MJPEG.
//
// MJPEG = поток обычных JPEG-кадров, склеенных multipart-ответом. Браузер
// и OpenCV понимают его без дополнительных кодеков — поэтому для стенда это
// самый дешёвый способ получить «живое видео».
#pragma once

#include <Arduino.h>

namespace stream {

void  begin();          // поднять сервер на порту 80
float fps();            // текущая частота кадров потока
bool  hasClient();

}  // namespace stream
