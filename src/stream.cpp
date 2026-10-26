#include "stream.h"

#include "config.h"          // ENABLE_WIFI объявлен здесь — только после этого #if

#if ENABLE_WIFI

#include <WiFi.h>
#include <esp_http_server.h>

#include "ble_photo.h"
#include "camera.h"

namespace {

httpd_handle_t   g_httpd     = nullptr;
volatile float   g_fps       = 0;
volatile bool    g_hasClient = false;

static const char *BOUNDARY   = "frame";
static const char *STREAM_CT  = "multipart/x-mixed-replace;boundary=frame";
static const char *PART_HDR   = "\r\n--frame\r\nContent-Type: image/jpeg\r\n"
                                "Content-Length: %u\r\n\r\n";

// ------------------------------------------------------------ /stream ------
esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t res = httpd_resp_set_type(req, STREAM_CT);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    g_hasClient = true;
    Serial.println("[http] клиент подключился к потоку");
    // Видеопоток несовместим со сном камеры: пока смотрят поток, сенсор
    // обязан работать. Будим его на всё время просмотра и усыпляем в конце.
    cam::wake();

    char    part[64];
    uint32_t frames = 0;
    uint32_t t0 = millis();

    while (true) {
        camera_fb_t *fb = cam::acquire();
        if (!fb) {
            res = ESP_FAIL;
            break;
        }

        size_t n = snprintf(part, sizeof(part), PART_HDR, (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, part, n);
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

        cam::release(fb);
        if (res != ESP_OK) break;          // клиент закрыл вкладку

        if (++frames >= 10) {
            uint32_t dt = millis() - t0;
            g_fps = dt ? (frames * 1000.0f / dt) : 0;
            frames = 0;
            t0 = millis();
        }
        // Отдаём процессор: без этого задача HTTP не даёт дышать BLE,
        // и передача фото начинает тормозить и терять уведомления.
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    g_hasClient = false;
    g_fps = 0;
#if CAM_POWER_PLAN
    cam::sleep();
#endif
    Serial.println("[http] клиент отключился от потока");
    return res;
}

// ------------------------------------------------------------ /snapshot ----
// Одиночный кадр обычным GET — удобно проверить камеру из браузера.
esp_err_t snapshot_handler(httpd_req_t *req) {
    size_t   len = 0;
    uint16_t w = 0, h = 0;
    uint8_t *jpg = cam::snapshotOnce(&len, &w, &h);
    if (!jpg) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snap.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)jpg, len);
    free(jpg);
    return res;
}

// ------------------------------------------------------------ /  (индекс) --
esp_err_t index_handler(httpd_req_t *req) {
    String ip = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString()
                                          : WiFi.localIP().toString();
    String html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>ESP32-S3 CAM</title>"
        "<style>body{background:#15171c;color:#e6e6e6;font-family:sans-serif;"
        "text-align:center;padding:20px}img{max-width:100%;border:1px solid #3a3d46;"
        "border-radius:6px}a{color:#8ecbff}</style></head><body>"
        "<h2>ESP32-S3 CAM</h2>"
        "<img src='/stream'><p>"
        "<a href='/snapshot'>одиночный кадр</a> &middot; "
        "поток: <code>http://" + ip + "/stream</code></p>"
        "<p style='color:#888'>Фотографии передаются отдельно по Bluetooth LE. "
        "Управление — галочкой в приложении на ноутбуке.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.length());
}

}  // namespace

namespace stream {

void begin() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.ctrl_port        = 32768;
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;
    // Поток занимает соединение целиком, поэтому нужен запас сокетов,
    // иначе /snapshot и / перестанут отвечать, пока открыт /stream.
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    if (httpd_start(&g_httpd, &cfg) != ESP_OK) {
        Serial.println("[http] не удалось запустить сервер");
        return;
    }

    httpd_uri_t u_index  = {"/",         HTTP_GET, index_handler,    nullptr};
    httpd_uri_t u_stream = {"/stream",   HTTP_GET, stream_handler,   nullptr};
    httpd_uri_t u_snap   = {"/snapshot", HTTP_GET, snapshot_handler, nullptr};
    httpd_register_uri_handler(g_httpd, &u_index);
    httpd_register_uri_handler(g_httpd, &u_stream);
    httpd_register_uri_handler(g_httpd, &u_snap);

    Serial.println("[http] сервер запущен на порту 80");
}

float fps()       { return g_fps; }
bool  hasClient() { return g_hasClient; }

}  // namespace stream

#endif  // ENABLE_WIFI
