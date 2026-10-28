#include "serial_diagnostics.h"
#include "config.h"

#if ENABLE_ETH

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "button.h"
#include "camera.h"
#include "checkpoint.h"
#include "eth_link.h"
#include "gm67.h"
#include "lcd.h"

namespace {
constexpr uint32_t REPORT_MS = 10000;
bool g_buttonDown = false;
GM67 *g_scanner = nullptr;
Stream *g_outputs[2] = {};
bool g_ready = false;
uint32_t g_lastReport = 0;
const char *g_stage = "boot";
uint32_t g_stageSince = 0;
uint32_t g_checkId = 0;
uint32_t g_checkSince = 0;
bool g_checkActive = false;
uint32_t g_saved = 0;
const char *g_outcome = "NONE";
bool g_cameraSeen = false;
bool g_cameraInitOk = false;
uint32_t g_captureErrors = 0;
uint32_t g_barcodes = 0;
uint32_t g_barcodeTimeouts = 0;
const char *g_serverOp = "NOT_TESTED";
bool g_serverOk = false;
int g_httpCode = 0;
uint32_t g_serverSince = 0;

struct Command {
    char text[16] = {};
    uint8_t length = 0;
    bool overflow = false;
} g_commands[2];

void line(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    for (Stream *output : g_outputs) {
        if (output) output->print(buffer);
    }
}

void commands(unsigned index) {
    Stream *input = g_outputs[index];
    if (!input) return;
    Command &command = g_commands[index];
    // A noisy terminal cannot monopolize the checkpoint loop.
    unsigned budget = 32;
    while (budget-- && input->available()) {
        const int c = input->read();
        if (c < 0) break;
        if (c == '\r' || c == '\n') {
            command.text[command.length] = '\0';
            if (command.overflow) {
                line("[diag] command too long; use HELP\n");
            } else if (!strcmp(command.text, "STATUS")) {
                diagnostics::report();
            } else if (!strcmp(command.text, "LCDCHECK")) {
                if (g_checkActive) {
                    line("[diag] LCDCHECK BUSY: wait until current check ends, then send again\n");
                } else {
                    line("[diag] LCDCHECK BEGIN: repeat LCD discovery; no MCU reset or API request\n");
                    const bool found = checkpoint::diagnoseLCD();
                    line("[diag] LCDCHECK END result=%s\n", found ? "READY" : "NOT_READY");
                    diagnostics::report();
                }
            } else if (!strcmp(command.text, "HELP")) {
                line("[diag] STATUS = report; LCDCHECK = recheck LCD while idle; HELP = commands; send with Enter\n");
            } else if (command.length) {
                line("[diag] unknown command; use HELP\n");
            }
            command = Command{};
        } else if (c >= 0x20 && c < 0x7f) {
            if (command.length < sizeof(command.text) - 1) {
                command.text[command.length++] = (char)toupper((unsigned char)c);
            } else {
                command.overflow = true;
            }
        }
    }
}
}  // namespace

namespace diagnostics {

void begin(GM67 *scanner, Stream *primary, Stream *alternate) {
    g_scanner = scanner;
    g_outputs[0] = primary;
    g_outputs[1] = alternate == primary ? nullptr : alternate;
    g_ready = false;
    g_stageSince = millis();
    line("[diag] serial-diag-button-net-2026-10-26; periodic=10s; button edges=live; commands: STATUS, LCDCHECK, HELP\n");
    line("[diag] Observe real checks: BUTTON -> CAMERA -> AKS -> GM67 -> AKS -> LCD\n");
}

void stage(const char *name) {
    const uint32_t now = millis();
    line("[diag][check=%lu][%lu ms] %s -> %s; elapsed=%lu ms\n",
         (unsigned long)g_checkId, (unsigned long)now, g_stage, name,
         (unsigned long)(now - g_stageSince));
    g_stage = name;
    g_stageSince = now;
}

void event(const char *component, const char *message) {
    line("[diag][check=%lu][%lu ms] %s: %s\n", (unsigned long)g_checkId,
         (unsigned long)millis(), component, message);
}

void cameraInit(bool ok) {
    g_cameraSeen = true;
    g_cameraInitOk = ok;
    event("CAMERA init", ok ? "OK (sensor initialized; frame not tested yet)" : "FAIL");
}

void capture(bool ok, uint16_t width, uint16_t height, size_t bytes) {
    if (ok) {
        // A successful later capture also confirms recovery after failed boot.
        g_cameraSeen = g_cameraInitOk = true;
    } else {
        ++g_captureErrors;
    }
    line("[diag][check=%lu] CAMERA frame=%s; %ux%u; jpeg=%u bytes; failures=%lu\n",
         (unsigned long)g_checkId, ok ? "OK" : "FAIL", width, height,
         (unsigned)bytes, (unsigned long)g_captureErrors);
}

void serverResult(const char *operation, bool ok, int httpCode) {
    g_serverOp = operation;
    g_serverOk = ok;
    g_httpCode = httpCode;
    g_serverSince = millis();
    line("[diag][check=%lu] SERVER op=%s; result=%s; HTTP=%d\n",
         (unsigned long)g_checkId, operation, ok ? "OK" : "FAIL", httpCode);
}

void barcode(bool received, bool canceled, uint32_t elapsedMs, size_t length) {
    if (received) ++g_barcodes;
    else if (!canceled) ++g_barcodeTimeouts;
    line("[diag][check=%lu] GM67 window=%s; elapsed=%lu ms; code_length=%u\n",
         (unsigned long)g_checkId, received ? "BARCODE_RECEIVED" :
         (canceled ? "CANCELED" : "TIMEOUT"), (unsigned long)elapsedMs, (unsigned)length);
}

void startCheck(uint32_t id) {
    g_checkId = id;
    g_checkSince = millis();
    g_checkActive = true;
    stage("check-start");
}

void finishCheck(const char *outcome) {
    if (!g_checkActive) return;
    g_outcome = outcome;
    if (!strcmp(outcome, "SAVED")) ++g_saved;
    line("[diag][check=%lu] END result=%s; total=%lu ms; saved=%lu\n",
         (unsigned long)g_checkId, outcome,
         (unsigned long)(millis() - g_checkSince), (unsigned long)g_saved);
    g_checkActive = false;
    stage("idle");
}

void report() {
    if (!g_ready) return;
    const uint32_t now = millis();
    g_lastReport = now;
    line("[diag] === uptime=%lu s; stage=%s; stage_age=%lu ms; check=%lu; active=%s ===\n",
         (unsigned long)(now / 1000), g_stage,
         (unsigned long)(now - g_stageSince), (unsigned long)g_checkId,
         g_checkActive ? "YES" : "NO");
#if ENABLE_BUTTON
    line("[diag] BUTTON GPIO%d=%s; raw=%s; armed=%s (click is logged separately)\n",
         BUTTON_PIN, btn::isDown() ? "DOWN" : "UP",
         btn::rawLevel() == 0 ? "LOW" : "HIGH", btn::armed() ? "YES" : "NO");
#endif
#if ENABLE_CAMERA
    line("[diag] CAMERA init=%s; state=%s; frames=%lu; jpeg=%u; rejected=%lu; failures=%lu\n",
         !g_cameraSeen ? "NOT_TESTED" : (g_cameraInitOk ? "OK" : "FAIL"),
         cam::stateName(), (unsigned long)cam::shotsTaken(), (unsigned)cam::lastLen(),
         (unsigned long)cam::rejected(), (unsigned long)g_captureErrors);
    line("[diag] CAMERA OFF/standby between checks is expected; init is last evidence\n");
#else
    line("[diag] CAMERA local=DISABLED; frame source=remote ESP32\n");
#endif
#if ENABLE_GM67
    if (g_scanner && g_scanner->port()) {
        const bool hasData = g_scanner->rxBytes() != 0;
        line("[diag] GM67 UART%d RX=%d TX=%d baud=%d; mode=%s; evidence=%s\n",
             GM67_UART_NUM, GM67_RX_PIN, GM67_TX_PIN, GM67_BAUD,
             GM67_PASSIVE_UART_MODE ? "PASSIVE" : "HOST",
             g_barcodes ? "BARCODE_RX" : (g_scanner->configured() ? "COMMAND_ACK" :
             (hasData ? "RX_BYTES_ONLY" : "NOT_CONFIRMED")));
        line("[diag] GM67 rx_processed=%lu; rejected_bytes=%lu; queued=%d; asleep=%s\n",
             (unsigned long)g_scanner->rxBytes(), (unsigned long)g_scanner->droppedBytes(),
             g_scanner->port()->available(), g_scanner->asleep() ? "YES" : "NO");
        line("[diag] GM67 barcodes=%lu; timeouts=%lu (inside authorized scan windows)\n",
             (unsigned long)g_barcodes, (unsigned long)g_barcodeTimeouts);
    } else {
        line("[diag] GM67 UART=NOT_INITIALIZED\n");
    }
#else
    line("[diag] GM67=DISABLED\n");
#endif
#if ENABLE_LCD
    const bool lcdReady = lcd::ok();
    const bool ack = lcdReady && lcd::probe();
    line("[diag] LCD I2C_ACK=%s; driver=%s; address=0x%02X; SDA=%d SCL=%d; errors=%lu\n",
         !lcdReady ? "NOT_TESTED" : (ack ? "YES" : "NO"), lcd::ok() ? "READY" : "NOT_READY",
         lcd::address(), LCD_SDA_PIN, LCD_SCL_PIN,
         (unsigned long)lcd::errors());
    if (!lcdReady) {
        line("[diag] LCD not initialized; send LCDCHECK while idle for a fresh I2C report\n");
    } else if (!ack) {
        line("[diag] LCD adapter did NOT acknowledge the I2C address\n");
    } else {
        line("[diag] LCD adapter acknowledged I2C; check visible text on the panel\n");
    }
#else
    line("[diag] LCD=DISABLED\n");
#endif
    const bool spi = eth::hardwareResponding();
    const bool sleeping = eth::asleep();
    const char *link = sleeping ? "SLEEP" : (spi && eth::linkUp() ? "UP" : "DOWN");
    line("[diag] W5500 SPI_VERSION=%s; link=%s; IP=%s; site=%s\n",
         spi ? "OK(0x04)" : "FAIL", link,
         spi ? eth::localIP().c_str() : "UNAVAILABLE", ETH_SITE_NAME);
    line("[diag] SERVER last_op=%s; result=%s; HTTP=%d; age=%lu ms (cached evidence)\n",
         g_serverOp, !strcmp(g_serverOp, "NOT_TESTED") ? "UNKNOWN" : (g_serverOk ? "OK" : "FAIL"),
         g_httpCode, (unsigned long)(now - g_serverSince));
    line("[diag] SERVER health=TCP/TLS only; HTTP=0 means no HTTP status recorded\n");
    line("[diag] MEMORY heap=%u; PSRAM free=%u/%u; last_result=%s; saved=%lu\n",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
         (unsigned)ESP.getPsramSize(), g_outcome, (unsigned long)g_saved);
    line("[diag] === end ===\n");
}

void ready() {
    g_ready = true;
#if ENABLE_BUTTON
    g_buttonDown = btn::isDown();
#endif
    stage("idle");
    report();
}

void tick() {
    if (!g_ready) return;
#if ENABLE_BUTTON
    // Log edges on each loop; the ten-second summary never gates the button.
    const bool down = btn::isDown();
    if (down != g_buttonDown) {
        g_buttonDown = down;
        line("[diag] BUTTON GPIO%d edge=%s; raw=%s\n", BUTTON_PIN,
             down ? "DOWN" : "UP", btn::rawLevel() == 0 ? "LOW" : "HIGH");
    }
#endif
    commands(0);
    commands(1);
    if ((uint32_t)(millis() - g_lastReport) >= REPORT_MS) report();
}

}  // namespace diagnostics
#endif  // ENABLE_ETH
