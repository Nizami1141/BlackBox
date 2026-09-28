#pragma once

#include <Arduino.h>

namespace eth {

struct VerifyResult {
    bool   ok = false;            // Запрос дошёл и сервер вернул 200 OK
    bool   matched = false;       // Лицо распознано и прошло liveness
    int    logId = 0;             // log_id сессии от центрального сервера
    String token;                 // Строковый alias для logId (для совместимости с FSM)
    int    expiresIn = 90;        // TTL сессии; сервер может вернуть расширение expires_in
    float  score = 0.0f;          // косинусная схожесть (расширение локального сервера)
    String reason;                // Сообщение об ошибке (например, "SAXTA ÜZ!")
    String name;                  // ФИО сотрудника
    String lcd1, lcd2;            // Сгенерированные строки для экрана
    int    httpCode = 0;
    String error;                 // Ошибка транспортного уровня (кабель, таймаут, SPI)
};

struct ScanResult {
    bool   ok = false;            // Баркод успешно привязан к логу
    int    recordId = 0;          // Внутренняя запись журнала (если сервер вернул)
    String reason;                // Сообщение от сервера
    String lcd1, lcd2;            // Строки для экрана
    int    httpCode = 0;
    String error;
};

bool begin();
bool linkUp();
bool hardwarePresent();
void reapply();
void sleep();
bool wake();
bool asleep();
String localIP();
bool health(String &out);
// Полностью локальная команда с сайта ноутбука. Возвращает true только когда
// сервер выдал этому посту ожидающий запрос на одну проверку.
bool takeRemoteTrigger();
// Connectivity report to the main server and Device Registry.
bool heartbeat();
void registryLog(const char *level, const char *message);

// Отправка кадра на /api/recognize (multipart/form-data)
VerifyResult verify(const uint8_t *jpeg, size_t len);

// Отправка штрихкода на /api/log_cargo (multipart/form-data)
ScanResult scan(const String &token, const String &barcode);

}  // namespace eth
