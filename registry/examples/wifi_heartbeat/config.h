#pragma once

// Wi-Fi
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// Сервер (реестр устройств)
#define SERVER_HOST     "192.168.1.50"   // IP или hostname сервера
#define SERVER_PORT     8001
// Если на сервере задан DEVICE_API_KEY — укажите его здесь.
#define API_KEY         ""

// Идентификация устройства — меняйте для каждой платы!
#define DEVICE_ID       "ESP32-BAKU-01"  // уникально для каждой платы
#define DEVICE_REGION   "baku"           // регион / площадка
#define DEVICE_NAME     "Склад №1"       // человекочитаемое имя (необязательно)

#define FW_VERSION      "1.1.0"

// Тайминги
#define HEARTBEAT_INTERVAL_MS  30000     // как часто слать heartbeat
#define LOG_FLUSH_INTERVAL_MS  5000      // как часто отправлять накопленные логи
#define LOG_BUFFER_SIZE        32        // максимум логов в памяти (старые вытесняются)
