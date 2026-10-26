// Автономная проверка OV5640: снимок JPEG передаётся на ПК через UART0.
#include <Arduino.h>
#include <esp_camera.h>

#include "camera_pins.h"
#include "config.h"

namespace {

bool cameraReady = false;

bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM;
    c.pin_d1 = Y3_GPIO_NUM;
    c.pin_d2 = Y4_GPIO_NUM;
    c.pin_d3 = Y5_GPIO_NUM;
    c.pin_d4 = Y6_GPIO_NUM;
    c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM;
    c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM;
    c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM;
    c.pin_href = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM;
    c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn = PWDN_GPIO_NUM;
    c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = CAM_XCLK_HZ;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size = FRAMESIZE_VGA;
    c.jpeg_quality = CAM_JPEG_QUALITY;
    c.fb_count = 1;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

    const esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        Serial.printf("CAMERA_FAIL init=0x%X\n", static_cast<unsigned>(err));
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        Serial.println("CAMERA_FAIL no_sensor");
        return false;
    }
    sensor->set_hmirror(sensor, CAM_HMIRROR);
    sensor->set_vflip(sensor, CAM_VFLIP);
    Serial.printf("CAMERA_SENSOR PID=0x%04X\n", sensor->id.PID);

    // Первые кадры после запуска нужны автоматике экспозиции OV5640.
    for (int i = 0; i < 3; ++i) {
        camera_fb_t *warmup = esp_camera_fb_get();
        if (warmup) esp_camera_fb_return(warmup);
        delay(120);
    }
    return true;
}

void sendPhoto() {
    if (!cameraReady) {
        Serial.println("CAPTURE_FAIL camera_not_ready");
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("CAPTURE_FAIL no_frame");
        return;
    }
    if (fb->format != PIXFORMAT_JPEG || fb->len < 4) {
        Serial.println("CAPTURE_FAIL not_jpeg");
        esp_camera_fb_return(fb);
        return;
    }

    Serial.printf("JPEG_BEGIN %u %u %u\n",
                  static_cast<unsigned>(fb->len),
                  static_cast<unsigned>(fb->width),
                  static_cast<unsigned>(fb->height));
    Serial.write(fb->buf, fb->len);
    Serial.print("\nJPEG_END\n");
    Serial.flush();
    esp_camera_fb_return(fb);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1200);
    Serial.println("ESP32-S3 OV5640 CAMERA CHECK");
    Serial.printf("PSRAM=%s free=%u KB\n", psramFound() ? "OK" : "NO",
                  static_cast<unsigned>(ESP.getFreePsram() / 1024));
    cameraReady = initCamera();
    if (cameraReady) {
        Serial.println("CAMERA_READY send CAPTURE");
    }
}

void loop() {
    if (!Serial.available()) {
        delay(10);
        return;
    }
    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toUpperCase();
    if (command == "CAPTURE") {
        sendPhoto();
    } else if (!command.isEmpty()) {
        Serial.println("COMMANDS: CAPTURE");
    }
}
