// ===========================================================================
//  Распиновка DVP-камеры (DC5640-AF = модуль на сенсоре OV5640)
//
//  Интерфейс OV5640 в этих модулях — параллельный DVP:
//     SCCB:  SIOD (SDA), SIOC (SCL)
//     тактирование: XCLK (от ESP32 к камере), PCLK (от камеры к ESP32)
//     синхро: VSYNC, HREF
//     данные: D0..D7  (в драйвере они называются Y2..Y9)
//     питание: 3V3 + GND, часто отдельный 1.8V/2.8V формируется на модуле
//
//  ВАЖНО: у ESP32-S3 с Octal PSRAM пины GPIO35/36/37 заняты связью с PSRAM
//  и НЕ могут использоваться для камеры (см. примечание в даташите
//  esp-dev-kits, раздел ESP32-S3-DevKitC-1).
// ===========================================================================
#pragma once
#include "config.h"

#if CAMERA_PRESET_FREENOVE_S3
  // Freenove ESP32-S3-WROOM CAM / Goouuu ESP32-S3 — плата со шлейфом камеры
  #define PWDN_GPIO_NUM   -1
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM   15
  #define SIOD_GPIO_NUM    4
  #define SIOC_GPIO_NUM    5
  #define Y9_GPIO_NUM     16
  #define Y8_GPIO_NUM     17
  #define Y7_GPIO_NUM     18
  #define Y6_GPIO_NUM     12
  #define Y5_GPIO_NUM     10
  #define Y4_GPIO_NUM      8
  #define Y3_GPIO_NUM      9
  #define Y2_GPIO_NUM     11
  #define VSYNC_GPIO_NUM   6
  #define HREF_GPIO_NUM    7
  #define PCLK_GPIO_NUM   13

#elif CAMERA_PRESET_ESP32S3_EYE
  #define PWDN_GPIO_NUM   -1
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM   15
  #define SIOD_GPIO_NUM    4
  #define SIOC_GPIO_NUM    5
  #define Y9_GPIO_NUM     16
  #define Y8_GPIO_NUM     17
  #define Y7_GPIO_NUM     18
  #define Y6_GPIO_NUM     12
  #define Y5_GPIO_NUM     10
  #define Y4_GPIO_NUM      8
  #define Y3_GPIO_NUM      9
  #define Y2_GPIO_NUM     11
  #define VSYNC_GPIO_NUM   6
  #define HREF_GPIO_NUM    7
  #define PCLK_GPIO_NUM   13

#elif CAMERA_PRESET_XIAO_S3
  #define PWDN_GPIO_NUM   -1
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM   10
  #define SIOD_GPIO_NUM   40
  #define SIOC_GPIO_NUM   39
  #define Y9_GPIO_NUM     48
  #define Y8_GPIO_NUM     11
  #define Y7_GPIO_NUM     12
  #define Y6_GPIO_NUM     14
  #define Y5_GPIO_NUM     16
  #define Y4_GPIO_NUM     18
  #define Y3_GPIO_NUM     17
  #define Y2_GPIO_NUM     15
  #define VSYNC_GPIO_NUM  38
  #define HREF_GPIO_NUM   47
  #define PCLK_GPIO_NUM   13

#elif CAMERA_PRESET_CUSTOM
  // Впишите свои пины сюда, сверяясь со схемой вашей платы
  #define PWDN_GPIO_NUM   -1
  #define RESET_GPIO_NUM  -1
  #define XCLK_GPIO_NUM   -1
  #define SIOD_GPIO_NUM   -1
  #define SIOC_GPIO_NUM   -1
  #define Y9_GPIO_NUM     -1
  #define Y8_GPIO_NUM     -1
  #define Y7_GPIO_NUM     -1
  #define Y6_GPIO_NUM     -1
  #define Y5_GPIO_NUM     -1
  #define Y4_GPIO_NUM     -1
  #define Y3_GPIO_NUM     -1
  #define Y2_GPIO_NUM     -1
  #define VSYNC_GPIO_NUM  -1
  #define HREF_GPIO_NUM   -1
  #define PCLK_GPIO_NUM   -1
  #error "CAMERA_PRESET_CUSTOM: впишите реальные пины в camera_pins.h"

#else
  #error "Не выбран ни один CAMERA_PRESET_* в config.h"
#endif
