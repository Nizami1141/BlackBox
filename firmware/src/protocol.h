// ===========================================================================
//  Протокол передачи фотографий по BLE
//
//  BLE не умеет отправлять «файл» — только короткие уведомления размером
//  MTU-3 байт. Поэтому JPEG режется на чанки и собирается обратно на стороне
//  GUI. Точная копия этого описания живёт в gui/protocol.py — если правите
//  здесь, правьте и там.
//
//  Все многобайтные поля — little-endian (как у ESP32 и у Python '<').
//
//  Пакет 1 — заголовок кадра (17 байт, влезает даже в минимальный MTU 23):
//      [0]      0x01
//      [1..4]   frame_id     uint32   номер кадра, растёт с 1
//      [5..8]   total_len    uint32   размер JPEG в байтах
//      [9..10]  chunk_size   uint16   размер полезной нагрузки чанка
//      [11..12] n_chunks     uint16   сколько чанков будет
//      [13..14] width        uint16
//      [15..16] height       uint16
//
//  Пакеты 2..N — данные:
//      [0]      0x02
//      [1..2]   seq          uint16   номер чанка, с 0
//      [3..]    payload
//
//  Последний пакет — конец кадра (9 байт):
//      [0]      0x03
//      [1..4]   frame_id     uint32
//      [5..8]   crc32        uint32   CRC-32 (IEEE 802.3) всего JPEG
//
//  Зачем CRC, если у BLE есть своя проверка на канальном уровне: канальная
//  CRC ловит искажение пакета, но не ловит ПОТЕРЮ уведомления — а notify
//  доставку не гарантирует. Сквозная контрольная сумма ловит и это.
//
//  Файл сознательно не зависит от Arduino.h — так его можно собрать на
//  обычном ПК и прогнать тесты сборки/разборки пакетов (см. tests/).
// ===========================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// --- UUID сервиса и характеристик (случайные, чтобы ни с чем не пересечься) -
#define SVC_UUID    "9a1f0001-4b2c-4d8e-9f10-2a3b4c5d6e7f"
#define CHR_CTRL    "9a1f0002-4b2c-4d8e-9f10-2a3b4c5d6e7f"  // write   — команды
#define CHR_DATA    "9a1f0003-4b2c-4d8e-9f10-2a3b4c5d6e7f"  // notify  — фото
#define CHR_STATUS  "9a1f0004-4b2c-4d8e-9f10-2a3b4c5d6e7f"  // notify  — статус

// --- типы пакетов ---
static const uint8_t PKT_HEADER  = 0x01;
static const uint8_t PKT_CHUNK   = 0x02;
static const uint8_t PKT_END     = 0x03;
static const uint8_t PKT_BARCODE = 0x04;
// Событие от платы к серверу: пока единственное — "BUTTON", нажатие кнопки
// на посту. Нужен, чтобы проверку запускал сотрудник у камеры, а не только
// оператор мышью на сайте: на границе рядом с постом компьютера не будет.
static const uint8_t PKT_EVENT   = 0x05;

static const size_t PKT_HEADER_LEN = 17;
static const size_t PKT_END_LEN    = 9;
static const size_t PKT_CHUNK_OVERHEAD = 3;    // [тип][seq u16]
static const size_t PKT_BARCODE_OVERHEAD = 7;  // [тип][seq u32][len u16]
static const size_t PKT_EVENT_OVERHEAD   = 3;  // [тип][len u16]

//  Пакет штрихкода (отдельный от фотографий, идёт по той же характеристике):
//      [0]     0x04
//      [1..4]  seq   uint32   номер скана, растёт с 1
//      [5..6]  len   uint16   длина строки в байтах
//      [7..]   ASCII содержимое штрихкода
//
//  Штрихкоды короткие (обычно до 30 символов), поэтому режем на чанки их не
//  нужно — код влезает в одно уведомление. Длина всё равно передаётся явно:
//  при маленьком MTU строку придётся обрезать, и приёмник должен это увидеть.

//  Пакет события (плата -> сервер):
//      [0]     0x05
//      [1..2]  len   uint16   длина строки
//      [3..]   ASCII имя события ("BUTTON", "BUTTON_LONG")
//
//  Старые приёмники, не знающие про 0x05, просто игнорируют такой пакет —
//  тип неизвестен, разбор возвращает «ничего». Поэтому добавление события
//  не ломает совместимость с прежним GUI.

// --- команды, которые сервер пишет в CHR_CTRL (обычный ASCII) ---
//   "PHOTO ON"      начать присылать фотографии
//   "PHOTO OFF"     прекратить
//   "SHOT"          снять и прислать один кадр прямо сейчас
//   "INTERVAL 3000" период съёмки в миллисекундах
//   "QUALITY 12"    качество JPEG (10..25, меньше = лучше)
//   "SCAN ON"/"SCAN OFF"   луч сканера
//   "LCD1 <текст>"  первая строка экрана, UTF-8
//   "LCD2 <текст>"  вторая строка
//   "LCDHOLD <мс>"  сколько держать текущее сообщение до возврата к дежурному
//
// Текст для экрана идёт как есть, в UTF-8: перевод в знакоместа HD44780
// делает сама плата (src/lcd.cpp). Так сервер не обязан знать ничего про
// таблицу символов экрана, а тексты правятся в одном месте — в config.yaml.
//
// ASCII выбран сознательно: команды можно послать с телефона любым
// BLE-клиентом (nRF Connect) и убедиться, что плата жива, без GUI.

// ---------------------------------------------------------------------------
//  CRC-32 (IEEE 802.3) — тот же полином и то же начальное значение,
//  что у zlib.crc32() в Python. Реализация без таблицы: 1 КБ во flash
//  дороже, чем 8 сдвигов на байт при наших объёмах.
// ---------------------------------------------------------------------------
inline uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
//  Сборка пакетов. Вынесено сюда, чтобы ровно тот же код, который работает
//  на плате, можно было проверить тестом на ПК.
// ---------------------------------------------------------------------------
inline size_t buildHeader(uint8_t *out, uint32_t frameId, uint32_t totalLen,
                          uint16_t chunkSize, uint16_t nChunks,
                          uint16_t w, uint16_t h) {
    out[0] = PKT_HEADER;
    memcpy(out + 1,  &frameId,   4);
    memcpy(out + 5,  &totalLen,  4);
    memcpy(out + 9,  &chunkSize, 2);
    memcpy(out + 11, &nChunks,   2);
    memcpy(out + 13, &w,         2);
    memcpy(out + 15, &h,         2);
    return PKT_HEADER_LEN;
}

inline size_t buildChunk(uint8_t *out, uint16_t seq,
                         const uint8_t *payload, size_t n) {
    out[0] = PKT_CHUNK;
    memcpy(out + 1, &seq, 2);
    memcpy(out + 3, payload, n);
    return n + PKT_CHUNK_OVERHEAD;
}

inline size_t buildEnd(uint8_t *out, uint32_t frameId, uint32_t crc) {
    out[0] = PKT_END;
    memcpy(out + 1, &frameId, 4);
    memcpy(out + 5, &crc,     4);
    return PKT_END_LEN;
}

// Собрать пакет штрихкода. maxPayload — сколько байт строки влезает
// в одно уведомление; лишнее отбрасывается, но поле len остаётся честным,
// чтобы приёмник понял, что строка пришла не полностью.
// Собрать пакет события. Строка короткая, режем по maxPayload на всякий случай.
inline size_t buildEvent(uint8_t *out, const char *name, size_t len,
                         size_t maxPayload) {
    uint16_t full = (uint16_t)len;
    size_t   put  = len > maxPayload ? maxPayload : len;
    out[0] = PKT_EVENT;
    memcpy(out + 1, &full, 2);
    memcpy(out + 3, name, put);
    return put + PKT_EVENT_OVERHEAD;
}

inline size_t buildBarcode(uint8_t *out, uint32_t seq,
                           const char *text, size_t len, size_t maxPayload) {
    uint16_t full = (uint16_t)len;
    size_t   put  = len > maxPayload ? maxPayload : len;
    out[0] = PKT_BARCODE;
    memcpy(out + 1, &seq,  4);
    memcpy(out + 5, &full, 2);
    memcpy(out + 7, text, put);
    return put + PKT_BARCODE_OVERHEAD;
}
