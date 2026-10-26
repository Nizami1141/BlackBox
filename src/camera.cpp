#include "camera.h"

#include <driver/ledc.h>
#include <esp_log.h>

#include "camera_pins.h"
#include "config.h"
#include "power.h"

namespace {

SemaphoreHandle_t g_mutex = nullptr;
cam::State        g_state = cam::State::OFF;
camera_config_t   g_cfg   = {};
bool              g_driverUp = false;

uint32_t g_shots        = 0;
uint32_t g_lastWakeMs   = 0;
uint32_t g_activeTotal  = 0;
uint32_t g_activeSince  = 0;
uint32_t g_lastShotMs   = 0;
uint32_t g_standbySince = 0;
uint32_t g_rejected     = 0;
size_t   g_lastLen      = 0;

// --- регистры OV5640 -------------------------------------------------------
// 0x3008 SYSTEM CTROL0:
//    бит 7 — программный сброс
//    бит 6 — программное выключение (standby)
// 0x02 = обычная работа, 0x42 = сон.
const int OV5640_SYS_CTRL0 = 0x3008;
const int OV5640_STANDBY   = 0x42;
const int OV5640_NORMAL    = 0x02;

// 0x3503 AEC/AGC MANUAL CONTROL:
//    бит 0 — выдержка вручную (1 = автоматика выключена)
//    бит 1 — усиление вручную
//    бит 2 — задержка применения ФАПЧ
// 0x00 = и выдержка, и усиление считаются автоматикой.
//
// Зачем трогать явно. При выходе из программного сна регистр иногда
// остаётся с выставленным битом ручного режима — сенсор тогда снимает
// с последней вручную зафиксированной выдержкой и не подстраивается
// под свет вообще. Кадр приходит белым и БЕЛЫМ ОСТАЁТСЯ, сколько его
// ни пересниимай. Одна запись здесь закрывает этот случай.
const int OV5640_AEC_CTRL   = 0x3503;
const int OV5640_AEC_AUTO   = 0x00;

void fillConfig() {
    camera_config_t &c = g_cfg;
    c = camera_config_t{};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM;   c.pin_d1 = Y3_GPIO_NUM;
    c.pin_d2 = Y4_GPIO_NUM;   c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM;   c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM;   c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk     = XCLK_GPIO_NUM;
    c.pin_pclk     = PCLK_GPIO_NUM;
    c.pin_vsync    = VSYNC_GPIO_NUM;
    c.pin_href     = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn     = PWDN_GPIO_NUM;
    c.pin_reset    = RESET_GPIO_NUM;
    c.xclk_freq_hz = CAM_XCLK_HZ;
    c.pixel_format = PIXFORMAT_JPEG;      // сжатие делает сам сенсор
    c.frame_size   = CAM_FRAMESIZE;
    c.jpeg_quality = CAM_JPEG_QUALITY;
    c.grab_mode    = CAMERA_GRAB_LATEST;  // всегда свежий кадр, не из очереди

    if (psramFound()) {
        c.fb_location = CAMERA_FB_IN_PSRAM;
        // Один буфер вместо двух. Двойная буферизация нужна видеопотоку,
        // чтобы кадры шли непрерывно. Здесь снимается один кадр за раз,
        // а второй буфер означает лишний кадр, который сенсор обязан
        // отснять — то есть лишнюю работу и лишнее тепло.
        c.fb_count    = CAM_FB_COUNT;
    } else {
        if (CAM_FRAMESIZE > FRAMESIZE_SVGA) {
            Serial.println("[cam] PSRAM не найдена — высокое разрешение недоступно");
        } else {
            Serial.println("[cam] PSRAM не найдена — падаю до QVGA");
        }
        c.fb_location = CAMERA_FB_IN_DRAM;
        c.fb_count    = 1;
        c.frame_size  = FRAMESIZE_QVGA;
    }
}

void applySensorSettings() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    const int mirrorResult = s->set_hmirror(s, CAM_HMIRROR);
    const int flipResult = s->set_vflip(s, CAM_VFLIP);
    Serial.printf("[cam] orientation hmirror=%d vflip=%d; status=%d/%d\n",
                  CAM_HMIRROR, CAM_VFLIP, mirrorResult, flipResult);
    if (mirrorResult || flipResult) {
        Serial.println("[cam] WARNING: sensor rejected orientation settings");
    }
}

bool driverInit() {
    // High-resolution JPEG and its transmission copy need working PSRAM.
    // Do not silently turn the 5 MP prototype into a QVGA camera.
    if (CAM_FRAMESIZE > FRAMESIZE_SVGA && !psramFound()) {
        Serial.println("[cam] high-resolution capture requires PSRAM; camera not started");
        return false;
    }
    Serial.printf("[cam] requested image=%ux%u; JPEG quality=%d; PSRAM free=%u\n",
                  resolution[g_cfg.frame_size].width,
                  resolution[g_cfg.frame_size].height, g_cfg.jpeg_quality,
                  (unsigned)ESP.getFreePsram());
    esp_err_t err = esp_camera_init(&g_cfg);
    if (err != ESP_OK) {
        Serial.printf("[cam] esp_camera_init: 0x%x — проверьте пины "
                      "в camera_pins.h\n", err);
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) Serial.printf("[cam] сенсор PID=0x%04x\n", s->id.PID);
    applySensorSettings();
    g_driverUp = true;
    return true;
}

// --------------------------------------------------------------- XCLK -----
// Тактирование сенсора идёт с блока LEDC. Останавливая его, мы снимаем
// клок с OV5640 — а это и есть главный источник его потребления.
// ОГОВОРКА, которая стоила замусоренного лога. Таймер LEDC настраивает
// драйвер esp32-camera, а не мы. На части версий ядра прямые вызовы ledc_*
// к ЕГО таймеру возвращают ESP_ERR_INVALID_STATE — внутренний объект
// драйвера LEDC в такой сборке не создан, хотя клок исправно идёт.
//
// Коды возврата раньше не проверялись. Из-за этого IDF писала
// «ledc: LEDC is not initialized» на каждой проверке, а мы следом
// рапортовали «XCLK снят» — хотя он никуда не девался.
//
// Что это значит по существу: сенсор всё равно уходит в программный сон
// (регистр 0x3008) и кадры снимать перестаёт, но клок продолжает идти,
// и экономия тока получается частичной. Полностью клок снимает только
// off() — он останавливает драйвер целиком.
//
// Защита от «уехавшего» XCLK при этом сохраняется, но держится не на
// ledc_set_freq, а на порядке вызовов: частота процессора поднимается ДО
// пробуждения камеры и опускается ПОСЛЕ её засыпания, поэтому кадр всегда
// снимается на той же частоте APB, на которой драйвер считал делитель.
bool g_ledcOk = true;             // пока не доказано обратное

void ledcComplain(const char *what, esp_err_t e) {
    if (!g_ledcOk) return;        // жалуемся ровно один раз за загрузку
    g_ledcOk = false;
    Serial.printf("[cam] LEDC не отдаёт управление таймером XCLK (%s: %s).\n",
                  what, esp_err_to_name(e));
    Serial.println("      Таймер принадлежит драйверу esp32-camera и в этой");
    Serial.println("      сборке ядра снаружи не управляется. Сенсор в сне");
    Serial.println("      кадры не снимает, но клок продолжает идти — экономия");
    Serial.println("      тока частичная. Полностью клок снимается только");
    Serial.println("      полным выключением камеры (см. CAM_DEEP_OFF_MS).");
}

void xclkStop() {
    esp_err_t e = ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    if (e != ESP_OK) ledcComplain("pause", e);
}

void xclkStart() {
    // Частоту задаём заново каждый раз, когда это возможно: делитель LEDC
    // считается от шины APB, а она меняется вместе с частотой процессора.
    esp_err_t e = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, CAM_XCLK_HZ);
    if (e != ESP_OK) { ledcComplain("set_freq", e); return; }
    e = ledc_timer_resume(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    if (e != ESP_OK) ledcComplain("resume", e);
}

bool sensorStandby(bool on) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_reg) return false;
    return s->set_reg(s, OV5640_SYS_CTRL0, 0xFF,
                      on ? OV5640_STANDBY : OV5640_NORMAL) == 0;
}

// Вернуть автоматику экспозиции и усиления в автоматический режим.
// Вызывается после каждого выхода из сна, см. комментарий у OV5640_AEC_CTRL.
void aecRestart() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    if (s->set_reg) s->set_reg(s, OV5640_AEC_CTRL, 0xFF, OV5640_AEC_AUTO);
    // Дублируем через штатный интерфейс драйвера: у части сборок
    // esp32-camera set_reg для OV5640 не реализован, а эти два вызова есть
    // всегда. Дешевле сделать оба, чем гадать, какой сработал.
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_gain_ctrl)     s->set_gain_ctrl(s, 1);
    if (s->set_whitebal)      s->set_whitebal(s, 1);
}

// Выбросить кадры, пока автоматика не сойдётся.
//
// Раньше здесь стояло фиксированное число кадров, и это была ошибка: сколько
// нужно на самом деле, зависит от света, а не от константы в config.h.
// В темноте выдержка длинная, кадры идут реже, сходимость дольше.
//
// Признак несошедшейся автоматики — подозрительно лёгкий JPEG. Пересвеченный
// кадр это почти равномерное белое поле, а равномерное поле сжимается в разы
// сильнее нормальной картинки (то же и с полностью чёрным). Поэтому здесь
// смотрим не на пиксели, а на длину кадра: как только она перевалила порог
// и перестала скакать — картинка устоялась, дальше ждать нечего.
//
// Возвращает размер последнего выброшенного кадра, 0 — если кадров не было.
size_t settleFrames(int maxFrames) {
    size_t prev = 0, len = 0;
    for (int i = 0; i < maxFrames; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        len = fb->len;
        esp_camera_fb_return(fb);

        if (CAM_MIN_JPEG_BYTES <= 0) continue;      // проверка выключена

        // Кадр «весомый» и почти такой же, как предыдущий -> устоялось.
        if (len >= (size_t)CAM_MIN_JPEG_BYTES && prev) {
            size_t d = (len > prev) ? len - prev : prev - len;
            if (d * 5 < prev) {                     // разница меньше 20 %
                Serial.printf("[cam] автоматика сошлась за %d кадр(а), "
                              "%u байт\n", i + 1, (unsigned)len);
                return len;
            }
        }
        prev = len;
    }
    if (CAM_MIN_JPEG_BYTES > 0 && len && len < (size_t)CAM_MIN_JPEG_BYTES) {
        Serial.printf("[cam] автоматика не сошлась за %d кадр(ов), "
                      "последний %u байт — кадр, вероятно, пересвечен\n",
                      maxFrames, (unsigned)len);
    }
    return len;
}

void markActive() {
    if (g_activeSince == 0) g_activeSince = millis();
}

void markIdle() {
    if (g_activeSince) {
        g_activeTotal += millis() - g_activeSince;
        g_activeSince = 0;
    }
}

}  // namespace

namespace cam {

// ===================================================================== begin
bool begin() {
    g_mutex = xSemaphoreCreateMutex();

    // Гасим ругань IDF по тегу "ledc". Свои коды возврата мы теперь
    // проверяем сами и говорим о них один раз, по-человечески
    // (см. ledcComplain выше), а IDF печатает ESP_LOGE ВНУТРИ функции,
    // до того как вернуть код ошибки — снаружи это никак не перехватить.
    // В итоге на каждую проверку в лог падали три строки "LEDC is not
    // initialized", за которыми не было видно ничего полезного.
    //
    // Глушим только этот тег и только его: остальные ошибки IDF нужны.
    esp_log_level_set("ledc", ESP_LOG_NONE);

    fillConfig();

    if (!driverInit()) {
        // Состояние OFF — не приговор: wake() при следующей проверке поднимет
        // драйвер с нуля и попробует снова. Так пост переживает шлейф, который
        // не сел с первого раза, и оживает без перезагрузки — сообщение ниже
        // говорит об этом прямо, чтобы «камера не инициализирована» на старте
        // не читалось как конец света.
        g_state = State::OFF;
        Serial.println("[cam] драйвер не поднялся. Это не окончательно:");
        Serial.println("      при первой же проверке попробую заново. Если и");
        Serial.println("      тогда не выйдет — переткните шлейф камеры,");
        Serial.println("      он садится в разъём до щелчка.");
        return false;
    }
    g_state = State::ACTIVE;
    markActive();

    Serial.printf("[cam] XCLK %u МГц, буферов кадра: %d\n",
                  (unsigned)(CAM_XCLK_HZ / 1000000), CAM_FB_COUNT);

    // Сразу в сон: до первой проверки камере делать нечего.
#if CAM_POWER_PLAN
    sleep();
#endif
    return true;
}

bool  ok()    { return g_driverUp || g_state != State::OFF; }
State state() { return g_state; }

const char *stateName() {
    switch (g_state) {
        case State::ACTIVE:  return "снимает";
        case State::STANDBY: return "лёгкий сон";
        default:             return "выключена";
    }
}

// ===================================================================== sleep
void sleep() {
#if CAM_POWER_PLAN == 0
    return;
#else
    if (g_state != State::ACTIVE) return;

    // Порядок важен: сначала усыпляем сенсор, пока клок ещё идёт (по SCCB
    // он общается, опираясь на XCLK), и только потом снимаем тактирование.
    if (!sensorStandby(true)) {
        // Сенсор не поддерживает программный сон (не OV5640) — тогда
        // единственный способ его остановить это выключить драйвер.
        Serial.println("[cam] сенсор не умеет standby, выключаю драйвер");
        off();
        return;
    }
    delay(5);
    xclkStop();

    g_state = State::STANDBY;
    g_standbySince = millis();
    markIdle();
    Serial.printf("[cam] сон: сенсор остановлен, XCLK %s\n",
                  g_ledcOk ? "снят" : "продолжает идти (см. сообщение о LEDC)");
#endif
}

// ======================================================================= off
void off() {
    if (g_state == State::OFF) return;
    esp_camera_deinit();
    g_driverUp = false;
    g_state = State::OFF;
    markIdle();
    Serial.println("[cam] выключена полностью");
}

// ====================================================================== wake
bool wake() {
    if (g_state == State::ACTIVE) return true;

    uint32_t t0 = millis();
    // Полную частоту поднимаем ДО пробуждения камеры: менять её при
    // работающем XCLK нельзя.
    power::active();

    if (g_state == State::STANDBY) {
        xclkStart();
        // Системе ФАПЧ сенсора нужно время, чтобы захватить частоту.
        delay(CAM_PLL_LOCK_MS);
        if (!sensorStandby(false)) {
            Serial.println("[cam] сенсор не вышел из сна, переинициализирую");
            off();
        } else {
            g_state = State::ACTIVE;
            markActive();
            // Программный сон останавливает конвейер сенсора целиком.
            // Значения регистров переживают сон, а вот петля регулирования
            // экспозиции — нет: после выхода она стартует заново. Поэтому
            // «тёплое» пробуждение отличается от холодного только тем, что
            // не надо поднимать драйвер; автоматике времени нужно столько же.
            aecRestart();
            delay(CAM_WARM_SETTLE_MS);
            settleFrames(CAM_WARM_SKIP_FRAMES);
            g_lastWakeMs = millis() - t0;
            Serial.printf("[cam] проснулась из сна за %lu мс\n",
                          (unsigned long)g_lastWakeMs);
            return true;
        }
    }

    // Холодный старт: драйвер поднимается с нуля.
    if (!driverInit()) {
        delay(120);
        if (!driverInit()) {
            Serial.println("[cam] камера не поднялась");
            g_state = State::OFF;
            return false;
        }
    }
    g_state = State::ACTIVE;
    markActive();
    // После полной инициализации регистры сброшены в заводские, автоматике
    // надо сойтись с нуля — отсюда более длинная пауза и лишний кадр.
    aecRestart();
    delay(CAM_COLD_SETTLE_MS);
    settleFrames(CAM_COLD_SKIP_FRAMES);
    g_lastWakeMs = millis() - t0;
    Serial.printf("[cam] холодный старт за %lu мс\n",
                  (unsigned long)g_lastWakeMs);
    return true;
}

// ====================================================================== tick
void tick() {
#if CAM_POWER_PLAN == 2
    // После долгого простоя уходим из лёгкого сна в полное выключение.
    // Разница невелика — в лёгком сне сенсор уже почти ничего не ест, —
    // но при простое в часы «почти ничего» тоже складывается.
    if (g_state == State::STANDBY && CAM_DEEP_OFF_MS > 0 &&
        (millis() - g_standbySince) > (uint32_t)CAM_DEEP_OFF_MS) {
        Serial.println("[cam] долгий простой — выключаю совсем");
        off();
    }
#endif
}

// ==================================================================== кадры
camera_fb_t *acquire(uint32_t timeoutMs) {
    if (g_state != State::ACTIVE) return nullptr;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return nullptr;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) xSemaphoreGive(g_mutex);      // не забыть отпустить при неудаче
    return fb;
}

void release(camera_fb_t *fb) {
    if (!fb) return;
    esp_camera_fb_return(fb);
    xSemaphoreGive(g_mutex);
}

uint8_t *snapshot(size_t *outLen, uint16_t *outW, uint16_t *outH) {
    camera_fb_t *fb = acquire();
    if (!fb) return nullptr;

    uint8_t *copy = (uint8_t *)ps_malloc(fb->len);   // копия в PSRAM
    if (copy) {
        memcpy(copy, fb->buf, fb->len);
        *outLen = fb->len;
        *outW   = fb->width;
        *outH   = fb->height;
    } else {
        Serial.printf("[cam] cannot allocate JPEG copy: need=%u bytes; PSRAM free=%u\n",
                      (unsigned)fb->len, (unsigned)ESP.getFreePsram());
    }
    release(fb);                                     // буфер драйверу сразу
    return copy;
}

uint8_t *snapshotOnce(size_t *outLen, uint16_t *outW, uint16_t *outH) {
    *outLen = 0; *outW = 0; *outH = 0;

    // Защита от частых съёмок подряд. Если кто-то жмёт кнопку без остановки,
    // камера не должна работать непрерывно — иначе весь смысл управления
    // питанием пропадает.
    uint32_t since = millis() - g_lastShotMs;
    if (g_lastShotMs && since < (uint32_t)CAM_MIN_INTERVAL_MS) {
        uint32_t wait = CAM_MIN_INTERVAL_MS - since;
        Serial.printf("[cam] пауза между кадрами: жду %lu мс\n",
                      (unsigned long)wait);
        delay(wait);
    }

    if (!wake()) return nullptr;

    // Пересниять, если кадр вышел «лёгким». Один такой кадр после сна —
    // это нормально, автоматика ещё догоняет; отправлять его на сервер —
    // нет. Сервер по белому пятну честно ответит «не найден», и вы будете
    // искать проблему в модели, а она в кадре.
    uint8_t *jpg = nullptr;
    for (int attempt = 0; attempt <= CAM_SNAP_RETRIES; attempt++) {
        jpg = snapshot(outLen, outW, outH);
        if (!jpg) break;

        g_lastLen = *outLen;
        if (CAM_MIN_JPEG_BYTES <= 0 ||
            *outLen >= (size_t)CAM_MIN_JPEG_BYTES ||
            attempt == CAM_SNAP_RETRIES) {
            break;
        }

        Serial.printf("[cam] кадр %u байт — меньше порога %d, переснимаю "
                      "(попытка %d из %d)\n",
                      (unsigned)*outLen, CAM_MIN_JPEG_BYTES,
                      attempt + 1, CAM_SNAP_RETRIES);
        free(jpg);
        jpg = nullptr;
        g_rejected++;
        delay(80);          // дать автоматике ещё пару кадров
    }

    if (jpg) {
        g_shots++;
        Serial.printf("[cam] кадр %ux%u, %u байт\n",
                      *outW, *outH, (unsigned)*outLen);
    }
    g_lastShotMs = millis();

#if CAM_POWER_PLAN
    sleep();
#endif
    // Частоту НЕ опускаем: сразу после съёмки кадр уходит по BLE или по сети,
    // и это тоже стоит делать на полной частоте. Опустить её должен тот,
    // кто вызвал — когда закончит с кадром.
    return jpg;
}

void setQuality(int q) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_quality(s, q);
}

// =============================================================== наблюдение
float chipTemp() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3
    return temperatureRead();
#else
    return NAN;
#endif
}

uint32_t shotsTaken()   { return g_shots; }
uint32_t rejected()     { return g_rejected; }
size_t   lastLen()      { return g_lastLen; }
uint32_t lastWakeMs()   { return g_lastWakeMs; }
uint32_t activeMsTotal() {
    return g_activeTotal + (g_activeSince ? millis() - g_activeSince : 0);
}

}  // namespace cam
