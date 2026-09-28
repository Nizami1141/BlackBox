// Интеграционный стенд: OV5640 + LCD1602/PCF8574 + команды ПК по UART0.
#include <Arduino.h>
#include <esp_camera.h>

#include "camera_pins.h"
#include "config.h"
#include "lcd.h"

namespace {

constexpr int TEST_LCD_SDA = 47;
constexpr int TEST_LCD_SCL = 21;

bool g_cameraOk = false;
bool g_lcdOk = false;
bool g_waitingSaved = false;

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
        Serial.printf("[FAIL] CAMERA init=0x%X\n", static_cast<unsigned>(err));
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        Serial.println("[FAIL] CAMERA sensor pointer is null");
        return false;
    }
    sensor->set_hmirror(sensor, CAM_HMIRROR);
    sensor->set_vflip(sensor, CAM_VFLIP);
    Serial.printf("[PASS] CAMERA OV5640 PID=0x%04X\n", sensor->id.PID);

    for (int i = 0; i < 3; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        delay(120);
    }
    return true;
}

void printReady() {
    Serial.printf("READY CAMERA=%s LCD=%s LCD_ADDR=0x%02X\n",
                  g_cameraOk ? "OK" : "FAIL", g_lcdOk ? "OK" : "FAIL",
                  lcd::address());
}

void captureAndSend() {
    if (!g_cameraOk) {
        Serial.println("CAPTURE_FAIL camera_not_ready");
        if (g_lcdOk) lcd::show("CAMERA FAIL", "Check wiring");
        return;
    }

    Serial.println("[CMD] CAPTURE received from GUI");
    if (g_lcdOk) lcd::show("Capturing...", "Please wait");

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("CAPTURE_FAIL no_frame");
        if (g_lcdOk) lcd::show("CAPTURE FAIL", "No frame");
        return;
    }
    if (fb->format != PIXFORMAT_JPEG || fb->len < 4) {
        Serial.println("CAPTURE_FAIL invalid_jpeg");
        if (g_lcdOk) lcd::show("CAPTURE FAIL", "Invalid JPEG");
        esp_camera_fb_return(fb);
        return;
    }

    Serial.printf("[PASS] CAMERA frame %ux%u, %u bytes\n",
                  static_cast<unsigned>(fb->width),
                  static_cast<unsigned>(fb->height),
                  static_cast<unsigned>(fb->len));
    Serial.printf("JPEG_BEGIN %u %u %u\n",
                  static_cast<unsigned>(fb->len),
                  static_cast<unsigned>(fb->width),
                  static_cast<unsigned>(fb->height));
    Serial.write(fb->buf, fb->len);
    Serial.print("\nJPEG_END\n");
    Serial.flush();
    esp_camera_fb_return(fb);

    g_waitingSaved = true;
    if (g_lcdOk) lcd::show("Photo sent", "Saving on PC...");
    Serial.println("[PASS] JPEG sent to PC; waiting for SAVED confirmation");
}

void processCommand(String command) {
    command.trim();
    command.toUpperCase();
    if (command == "CAPTURE") {
        captureAndSend();
    } else if (command == "SAVED") {
        g_waitingSaved = false;
        if (g_lcdOk) lcd::show("OK", "Photo saved");
        Serial.println("[PASS] PC confirmed: photo saved; LCD shows OK");
    } else if (command == "STATUS") {
        printReady();
    } else if (command == "LCDTEST") {
        // Повторно сканируем шину: можно исправить проводку при выключенном
        // питании, включить плату и проверить без новой прошивки.
        g_lcdOk = lcd::begin(TEST_LCD_SDA, TEST_LCD_SCL, 0);
        if (g_lcdOk) lcd::show("I2C OK", "LCD works");
        Serial.printf("[%s] LCDTEST I2C address=0x%02X errors=%lu\n",
                      g_lcdOk ? "PASS" : "FAIL", lcd::address(),
                      static_cast<unsigned long>(lcd::errors()));
    } else if (!command.isEmpty()) {
        Serial.println("COMMANDS: STATUS, LCDTEST, CAPTURE, SAVED");
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1200);
    Serial.println();
    Serial.println("============================================================");
    Serial.println(" ESP32-S3 CAMERA + LCD I2C + PC GUI TEST");
    Serial.println("============================================================");
    Serial.printf("[I2C] LCD wiring: SDA=GPIO%d, SCL=GPIO%d\n",
                  TEST_LCD_SDA, TEST_LCD_SCL);
    Serial.printf("[MEM] PSRAM=%s, free=%u KB\n", psramFound() ? "OK" : "FAIL",
                  static_cast<unsigned>(ESP.getFreePsram() / 1024));

    g_lcdOk = lcd::begin(TEST_LCD_SDA, TEST_LCD_SCL, 0);
    if (g_lcdOk) {
        Serial.printf("[PASS] I2C LCD found at 0x%02X; communication works\n",
                      lcd::address());
        lcd::show("I2C OK", "Starting camera");
    } else {
        Serial.println("[FAIL] I2C LCD not found (expected 0x27 or 0x3F)");
    }

    g_cameraOk = initCamera();
    if (g_lcdOk) {
        lcd::show(g_cameraOk ? "READY" : "CAMERA FAIL",
                  g_cameraOk ? "GUI: Capture" : "Check wiring");
    }
    printReady();
    Serial.println("Commands: STATUS, LCDTEST, CAPTURE");
}

void loop() {
    if (Serial.available()) {
        processCommand(Serial.readStringUntil('\n'));
    }
    delay(10);
}
