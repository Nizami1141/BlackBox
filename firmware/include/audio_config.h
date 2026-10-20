#pragma once

// Enabled only by the explicit esp32s3cam_audio[_uart] profiles.
#ifndef ENABLE_AUDIO_COMPANION
#define ENABLE_AUDIO_COMPANION 0
#endif
#define AUDIO_LINK_RX_PIN 1
#define AUDIO_LINK_TX_PIN 2
#define AUDIO_LINK_BAUD 115200
// Second board: ESP32-S3, logs through native USB (UART0 is the player).
#define AUDIO_SCANNER_RX_PIN 16
#define AUDIO_SCANNER_TX_PIN 17
#define AUDIO_PLAYER_RX_PIN 18
#define AUDIO_PLAYER_TX_PIN 21
#define AUDIO_PLAYER_VOLUME 20  // 0..30

#if ENABLE_AUDIO_COMPANION && (defined(FW_ROLE_GATEWAY) || defined(FW_ROLE_CAMERA_NODE))
#error "Audio companion requires the main ESP32-S3 with its local camera"
#endif
