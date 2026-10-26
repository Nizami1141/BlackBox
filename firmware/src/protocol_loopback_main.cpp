// Проверка интерфейсов ESP32-S3. Нужны указанные перемычки; модули снимают
// с тестируемых линий до подачи питания. PASS не означает исправность модуля.
#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_camera.h>

#include "camera_pins.h"
#include "config.h"

namespace {

bool uartPass = false;
bool spiPass = false;
bool i2cPass = false;
bool gpioPass = false;
bool cameraPass = false;
bool cameraUp = false;
bool pinsPass = false;
Preferences results;
volatile int slaveCount = 0;
volatile uint8_t slaveFirst = 0;
volatile uint8_t slaveSecond = 0;

void onI2cReceive(int count) {
    slaveCount = count;
    if (count > 0) slaveFirst = Wire1.read();
    if (count > 1) slaveSecond = Wire1.read();
    while (Wire1.available()) Wire1.read();
}

void onI2cRequest() { Wire1.write(uint8_t(0xA5)); }

void remember(const char *key, bool passed) {
    results.putBool(key, passed);
}

void summary() {
    Serial.println("---------------- PROTOCOL TEST ----------------");
    Serial.printf("[%s] UART1 TX GPIO42 -> RX GPIO14 loopback\n", uartPass ? "PASS" : "WAIT/FAIL");
    Serial.printf("[%s] W5500 SPI MOSI GPIO38 -> MISO GPIO40 loopback\n", spiPass ? "PASS" : "WAIT/FAIL");
    Serial.printf("[%s] I2C master GPIO47/21 -> slave GPIO14/42\n", i2cPass ? "PASS" : "WAIT/FAIL");
    Serial.printf("[%s] BUTTON GPIO output 42 -> input 14 loopback\n", gpioPass ? "PASS" : "WAIT/FAIL");
    Serial.printf("[%s] CAMERA SCCB + DVP actual JPEG\n", cameraPass ? "PASS" : "WAIT/FAIL");
    Serial.printf("RESULT: %s\n", uartPass && spiPass && i2cPass && gpioPass && cameraPass
                                   ? "PASS 5/5" : "NOT COMPLETE");
    Serial.println("-----------------------------------------------");
}

void checkUart() {
    Serial.println("[TEST] UART1: disconnect W5500/button; GPIO42 -> 1k -> GPIO14");
    Serial1.begin(9600, SERIAL_8N1, 14, 42);
    delay(50);
    while (Serial1.available()) Serial1.read();
    const uint8_t pattern[] = {0x55, 0xA5, 0x00, 0xFF, 0x3C};
    Serial1.write(pattern, sizeof(pattern));
    Serial1.flush();
    uint8_t received[sizeof(pattern)] = {};
    size_t count = 0;
    uint32_t start = millis();
    while (count < sizeof(pattern) && millis() - start < 500) {
        if (Serial1.available()) received[count++] = uint8_t(Serial1.read());
        delay(1);
    }
    uartPass = count == sizeof(pattern);
    for (size_t i = 0; i < count && uartPass; ++i) uartPass = received[i] == pattern[i];
    Serial1.end();
    remember("uart", uartPass);
    Serial.printf("[%s] UART1 9600 8N1, received=%u/%u bytes\n", uartPass ? "PASS" : "FAIL",
                  unsigned(count), unsigned(sizeof(pattern)));
    summary();
}

void checkPins() {
    Serial.println("[TEST] PINS: same GPIO42 -> 1k -> GPIO14; W5500/button disconnected");
    pinMode(42, OUTPUT);
    pinMode(14, INPUT_PULLUP);
    digitalWrite(42, LOW);
    delay(20);
    bool low = digitalRead(14) == LOW;
    pinMode(14, INPUT_PULLDOWN);
    digitalWrite(42, HIGH);
    delay(20);
    bool high = digitalRead(14) == HIGH;
    pinMode(42, INPUT);
    pinMode(14, INPUT);
    pinsPass = low && high;
    Serial.printf("[%s] PINS GPIO42->GPIO14 electrical path: LOW=%s, HIGH=%s\n",
                  pinsPass ? "PASS" : "FAIL", low ? "seen" : "missing",
                  high ? "seen" : "missing");
    if (!pinsPass) Serial.println("       Check jumper contact and IO42/IO14 labels on the ESP32-S3 board");
    else Serial.println("       GPIO path works; retry UART to check UART peripheral routing");
    summary();
}

void checkSpi() {
    Serial.println("[TEST] SPI: disconnect W5500; jumper GPIO38 -> GPIO40");
    pinMode(41, OUTPUT);
    digitalWrite(41, HIGH);
    SPI.begin(39, 40, 38, 41);
    const uint8_t pattern[] = {0x55, 0xA5, 0x3C, 0xC3};
    uint8_t received[sizeof(pattern)] = {};
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(41, LOW);
    for (size_t i = 0; i < sizeof(pattern); ++i) received[i] = SPI.transfer(pattern[i]);
    digitalWrite(41, HIGH);
    SPI.endTransaction();
    SPI.end();
    spiPass = true;
    for (size_t i = 0; i < sizeof(pattern); ++i) spiPass &= received[i] == pattern[i];
    remember("spi", spiPass);
    Serial.printf("[%s] SPI 1 MHz MOSI/MISO data loopback\n", spiPass ? "PASS" : "FAIL");
    Serial.println("       SCK/CS electrical output and W5500 PHY are not proven by this jumper");
    summary();
}

void checkI2c() {
    Serial.println("[TEST] I2C: disconnect LCD/W5500/button; jumpers 47->14 (SDA), 21->42 (SCL)");
    slaveCount = 0;
    slaveFirst = slaveSecond = 0;
    Wire1.onReceive(onI2cReceive);
    Wire1.onRequest(onI2cRequest);
    bool slaveStarted = Wire1.begin(uint8_t(0x42), 14, 42, 100000);
    bool masterStarted = Wire.begin(47, 21, 100000);
    delay(100);
    int ack = -1;
    int response = -1;
    if (slaveStarted && masterStarted) {
        Wire.beginTransmission(0x42);
        Wire.write(uint8_t(0x5A));
        Wire.write(uint8_t(0xC3));
        ack = Wire.endTransmission();
        delay(30);
        if (Wire.requestFrom(uint8_t(0x42), uint8_t(1)) == 1) response = Wire.read();
    }
    i2cPass = slaveStarted && masterStarted && ack == 0 &&
              slaveCount == 2 && slaveFirst == 0x5A && slaveSecond == 0xC3 &&
              response == 0xA5;
    Serial.printf("[%s] I2C 100 kHz, ACK=%d, slave_rx=%d, reply=0x%02X\n",
                  i2cPass ? "PASS" : "FAIL", ack, slaveCount, response);
    Wire.end();
    Wire1.end();
    remember("i2c", i2cPass);
    summary();
}

void checkGpio() {
    Serial.println("[TEST] GPIO: disconnect button and W5500; jumper GPIO42 -> GPIO14 via 1 kOhm");
    pinMode(14, INPUT_PULLUP);
    pinMode(42, OUTPUT);
    digitalWrite(42, LOW);
    delay(20);
    bool low = digitalRead(14) == LOW;
    digitalWrite(42, HIGH);
    delay(20);
    bool high = digitalRead(14) == HIGH;
    pinMode(42, INPUT);
    gpioPass = low && high;
    remember("gpio", gpioPass);
    Serial.printf("[%s] BUTTON input GPIO14 follows GPIO42 low/high\n",
                  gpioPass ? "PASS" : "FAIL");
    summary();
}

void checkCamera() {
    Serial.println("[TEST] CAMERA: connected OV5640 required for SCCB/DVP test");
    if (!cameraUp) {
        camera_config_t c = {};
        c.ledc_channel = LEDC_CHANNEL_0;
        c.ledc_timer = LEDC_TIMER_0;
        c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM;
        c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
        c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
        c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
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
        esp_err_t err = esp_camera_init(&c);
        if (err != ESP_OK) {
            Serial.printf("[FAIL] CAMERA init 0x%X\n", unsigned(err));
            cameraPass = false;
            remember("cam", false);
            summary();
            return;
        }
        cameraUp = true;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    cameraPass = false;
    for (int attempt = 0; attempt < 3 && !cameraPass; ++attempt) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            cameraPass = sensor && fb->format == PIXFORMAT_JPEG && fb->len > 4 &&
                         fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
                         fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
            if (cameraPass) Serial.printf("[PASS] CAMERA SCCB PID=0x%04X + DVP JPEG %u bytes\n",
                                          sensor->id.PID, unsigned(fb->len));
            esp_camera_fb_return(fb);
        }
        if (!cameraPass) delay(150);
    }
    if (!cameraPass) Serial.println("[FAIL] CAMERA no valid JPEG");
    remember("cam", cameraPass);
    summary();
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(250);
    delay(1200);
    results.begin("s3-loop-v2", false);
    uartPass = results.getBool("uart", false);
    spiPass = results.getBool("spi", false);
    i2cPass = results.getBool("i2c", false);
    gpioPass = results.getBool("gpio", false);
    cameraPass = results.getBool("cam", false);
    Serial.println("ESP32-S3 PROTOCOL / PIN LOOPBACK TEST");
    Serial.println("Commands: PINS UART SPI I2C GPIO CAM STATUS CLEAR");
    Serial.println("Disconnect modules from loopback pins and move jumpers with power OFF.");
    Serial.println("Results are saved across power cycles; CLEAR resets them.");
    Serial.println("After reconnecting power, send the matching command.");
    summary();
}

void loop() {
    if (!Serial.available()) { delay(10); return; }
    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toUpperCase();
    if (command == "UART") checkUart();
    else if (command == "PINS") checkPins();
    else if (command == "SPI") checkSpi();
    else if (command == "I2C") checkI2c();
    else if (command == "GPIO") checkGpio();
    else if (command == "CAM") checkCamera();
    else if (command == "STATUS") summary();
    else if (command == "CLEAR") {
        results.clear();
        uartPass = spiPass = i2cPass = gpioPass = cameraPass = false;
        Serial.println("[INFO] Saved protocol results cleared");
        summary();
    }
    else if (command.length()) Serial.println("Commands: PINS UART SPI I2C GPIO CAM STATUS CLEAR");
}
