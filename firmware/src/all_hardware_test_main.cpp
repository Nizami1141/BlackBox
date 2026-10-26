// Стендовая проверка всех модулей на одной ESP32-S3 Freenove/Goouuu.
// Это аппаратный интеграционный тест: PASS означает реальный ответ устройства.
#include <Arduino.h>
#include <Ethernet.h>
#include <SPI.h>
#include <esp_camera.h>

#include "button.h"
#include "camera_pins.h"
#include "config.h"
#include "gm67.h"
#include "lcd.h"

namespace {

constexpr int LCD_TEST_SDA = 47;
constexpr int LCD_TEST_SCL = 21;
constexpr uint32_t SCAN_REARM_MS = 6000;
constexpr uint32_t HEARTBEAT_MS = 5000;

GM67 scanner;
bool lcdPassed = false;
bool cameraPassed = false;
bool ethernetChipPassed = false;
bool ethernetLinkPassed = false;
bool ethernetPeerPassed = false;
bool gm67CommandPassed = false;
bool gm67CodePassed = false;
bool buttonPassed = false;
bool cameraDriverUp = false;
bool scannerReady = false;
uint32_t lastScanArm = 0;
uint32_t lastHeartbeat = 0;
uint32_t lastLinkCheck = 0;
uint32_t lastSummary = 0;
uint32_t previousLcdErrors = 0;

void printSummary() {
    const bool ethernetPassed = ethernetChipPassed && ethernetLinkPassed && ethernetPeerPassed;
    const bool gm67Passed = gm67CommandPassed && gm67CodePassed;
    const bool allPassed = ethernetPassed && gm67Passed && buttonPassed &&
                           lcdPassed && cameraPassed;
    Serial.println("---------------- HARDWARE TEST ----------------");
    Serial.printf("[%s] ETHERNET W5500 SPI + link + PC PING/PONG\n",
                  ethernetPassed ? "PASS" : "WAIT/FAIL");
    Serial.printf("[%s] GM67 UART command + barcode\n", gm67Passed ? "PASS" : "WAIT/FAIL");
    Serial.printf("[%s] BUTTON GPIO%d real press\n", buttonPassed ? "PASS" : "WAIT/FAIL", BUTTON_PIN);
    Serial.printf("[%s] LCD I2C ACK + write, addr=0x%02X\n", lcdPassed ? "PASS" : "WAIT/FAIL", lcd::address());
    Serial.printf("[%s] CAMERA sensor + complete JPEG\n", cameraPassed ? "PASS" : "WAIT/FAIL");
    Serial.printf("RESULT: %s\n", allPassed ? "PASS 5/5" : "NOT COMPLETE");
    Serial.println("-----------------------------------------------");
    if (allPassed && lcd::ok()) lcd::show("ALL TESTS PASS", "5 of 5 OK");
}

bool checkCamera() {
    if (!cameraDriverUp) {
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

        esp_err_t err = esp_camera_init(&c);
        if (err != ESP_OK) {
            Serial.printf("[FAIL] CAMERA init=0x%X (power/pins/ribbon)\n", unsigned(err));
            return false;
        }
        cameraDriverUp = true;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        Serial.println("[FAIL] CAMERA sensor did not answer SCCB");
        return false;
    }
    Serial.printf("[INFO] CAMERA PID=0x%04X\n", sensor->id.PID);
    bool valid = false;
    for (int attempt = 0; attempt < 3 && !valid; ++attempt) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            valid = fb->format == PIXFORMAT_JPEG && fb->len > 4 &&
                    fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
                    fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
            Serial.printf("[%s] CAMERA JPEG %ux%u, %u bytes\n", valid ? "PASS" : "FAIL",
                          unsigned(fb->width), unsigned(fb->height), unsigned(fb->len));
            esp_camera_fb_return(fb);
        }
        if (!valid) delay(150);
    }
    if (!valid) Serial.println("[FAIL] CAMERA no complete JPEG (check DVP/power)");
    return valid;
}

void checkLcd() {
    lcdPassed = lcd::begin(LCD_TEST_SDA, LCD_TEST_SCL, 0);
    if (lcdPassed) {
        previousLcdErrors = lcd::errors();
        lcd::show("I2C OK", "Testing modules");
        lcdPassed = lcd::ok() && lcd::errors() == previousLcdErrors;
    }
    Serial.printf("[%s] LCD I2C addr=0x%02X, SDA=%d SCL=%d, errors=%lu\n",
                  lcdPassed ? "PASS" : "FAIL", lcd::address(), LCD_TEST_SDA,
                  LCD_TEST_SCL, static_cast<unsigned long>(lcd::errors()));
}

void checkEthernet() {
#if ETH_RST_PIN >= 0
    pinMode(ETH_RST_PIN, OUTPUT);
    digitalWrite(ETH_RST_PIN, LOW);
    delay(30);
    digitalWrite(ETH_RST_PIN, HIGH);
    delay(200);
#endif
    SPI.begin(ETH_SCK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
    Ethernet.init(ETH_CS_PIN);
    Ethernet.setRetransmissionTimeout(300);
    Ethernet.setRetransmissionCount(2);
    byte mac[] = ETH_MAC;
    IPAddress ip ETH_STATIC_IP;
    IPAddress peer(172, 16, 6, 51);
    IPAddress subnet ETH_SUBNET;
    Ethernet.begin(mac, ip, peer, peer, subnet);
    ethernetChipPassed = Ethernet.hardwareStatus() == EthernetW5500;
    ethernetLinkPassed = ethernetChipPassed && Ethernet.linkStatus() == LinkON;
    ethernetPeerPassed = false;
    Serial.printf("[%s] W5500 SPI hardware %s\n", ethernetChipPassed ? "PASS" : "FAIL",
                  ethernetChipPassed ? "detected" : "not detected");
    Serial.printf("[%s] ETHERNET RJ45 link, IP=%s\n",
                  ethernetLinkPassed ? "PASS" : "WAIT/FAIL", Ethernet.localIP().toString().c_str());
    if (!ethernetLinkPassed) {
        Serial.println("[ACTION] Connect RJ45 to laptop/switch; link is required for PASS");
        return;
    }

    EthernetClient client;
    if (client.connect(peer, 8765)) {
        client.print("PING\n");
        uint32_t started = millis();
        String reply;
        while (millis() - started < 2000 && !reply.endsWith("\n")) {
            while (client.available()) reply += char(client.read());
            delay(5);
        }
        reply.trim();
        ethernetPeerPassed = reply == "PONG";
        client.stop();
    }
    Serial.printf("[%s] ETHERNET PC 172.16.6.51:8765 PING/PONG\n",
                  ethernetPeerPassed ? "PASS" : "WAIT/FAIL");
    if (!ethernetPeerPassed)
        Serial.println("[ACTION] Start tools/ethernet_echo_test.py on laptop, then type ETHTEST");
}

void checkScanner() {
    if (!scannerReady) {
        scanner.begin(GM67_UART_NUM, GM67_RX_PIN, GM67_TX_PIN, GM67_BAUD);
        scanner.setMinLength(3);
        scannerReady = true;
    }
    // Параметры действуют до следующего сброса GM67; флеш сканера не меняем.
    gm67CommandPassed = scanner.configure(GM67::Mode::HOST, false, false);
    Serial.printf("[%s] GM67 UART command response, RX=%d TX=%d\n",
                  gm67CommandPassed ? "PASS" : "FAIL", GM67_RX_PIN, GM67_TX_PIN);
    if (gm67CommandPassed) {
        scanner.startDecode();
        lastScanArm = millis();
        Serial.println("[ACTION] Present any barcode to GM67 (within 30 seconds)");
    } else {
        Serial.println("[ACTION] Check GM67 power, crossed TX/RX, GND and UART mode");
    }
}

void runAutomaticChecks() {
    Serial.println("[RUN] Checking LCD, camera, W5500 and GM67...");
    checkLcd();
    cameraPassed = checkCamera();
    checkEthernet();
    checkScanner();
    Serial.printf("[ACTION] Press and release button on GPIO%d\n", BUTTON_PIN);
    printSummary();
}

void pollInteractive() {
    if (!buttonPassed && btn::pressed()) {
        buttonPassed = true;
        Serial.printf("[PASS] BUTTON GPIO%d press/release detected\n", BUTTON_PIN);
        printSummary();
    }
    if (scannerReady && !gm67CodePassed) {
        String code;
        if (scanner.poll(code)) {
            gm67CodePassed = true;
            Serial.printf("[PASS] GM67 barcode received: %s\n", code.c_str());
            printSummary();
        } else if (gm67CommandPassed && millis() - lastScanArm >= SCAN_REARM_MS && scanner.idle()) {
            scanner.startDecode();
            lastScanArm = millis();
        }
    }
    if (ethernetChipPassed && millis() - lastLinkCheck >= 1000) {
        lastLinkCheck = millis();
        bool link = Ethernet.linkStatus() == LinkON;
        if (link != ethernetLinkPassed) {
            ethernetLinkPassed = link;
            if (!link) ethernetPeerPassed = false;
            Serial.printf("[%s] ETHERNET RJ45 link %s\n", link ? "PASS" : "FAIL",
                          link ? "UP" : "DOWN");
            printSummary();
        }
    }
    if (lcdPassed && lcd::errors() != previousLcdErrors) {
        lcdPassed = false;
        Serial.printf("[FAIL] LCD I2C errors increased to %lu\n",
                      static_cast<unsigned long>(lcd::errors()));
        printSummary();
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(250);
    delay(1200);
    Serial.println("ESP32-S3 ALL HARDWARE TEST — W5500/GM67/BUTTON/LCD/OV5640");
    Serial.printf("[POWER] source voltage is not measured; PSRAM=%s, free heap=%u KB\n",
                  psramFound() ? "OK" : "FAIL", unsigned(ESP.getFreeHeap() / 1024));
    btn::begin(BUTTON_PIN, true);
    runAutomaticChecks();
    lastHeartbeat = millis();
}

void loop() {
    pollInteractive();
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toUpperCase();
        if (command == "RUN") runAutomaticChecks();
        else if (command == "STATUS") printSummary();
        else if (command == "LCDTEST") { checkLcd(); printSummary(); }
        else if (command == "CAMTEST") { cameraPassed = checkCamera(); printSummary(); }
        else if (command == "ETHTEST") { checkEthernet(); printSummary(); }
        else if (command == "GMTEST") { checkScanner(); printSummary(); }
        else if (command.length()) Serial.println("COMMANDS: RUN STATUS LCDTEST CAMTEST ETHTEST GMTEST");
    }
    if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
        lastHeartbeat = millis();
        Serial.printf("[alive] heap=%u KB, PSRAM=%u KB, uptime=%lu s\n",
                      unsigned(ESP.getFreeHeap() / 1024), unsigned(ESP.getFreePsram() / 1024),
                      static_cast<unsigned long>(millis() / 1000));
        if (millis() - lastSummary >= 30000) {
            lastSummary = millis();
            printSummary();
        }
    }
    delay(10);
}
