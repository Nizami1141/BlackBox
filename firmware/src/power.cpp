#include "power.h"

#include <driver/rtc_io.h>
#include <esp_pm.h>
#include <esp_sleep.h>

#include "config.h"

namespace {
bool g_isActive = true;
}

namespace power {

void begin() {
#if POWER_MODE == 0
    Serial.println("[pwr] управление питанием выключено (POWER_MODE 0)");
    return;
#else
    // Стартуем на пониженной частоте: до первого нажатия кнопки делать
    // нечего, а 240 МГц греют заметно.
    setCpuFrequencyMhz(CPU_MHZ_IDLE);
    g_isActive = false;
    Serial.printf("[pwr] режим %d: покой %d МГц, работа %d МГц%s\n",
                  POWER_MODE, CPU_MHZ_IDLE, CPU_MHZ_ACTIVE,
                  POWER_MODE == 2 ? ", лёгкий сон между проверками" : "");
#endif
}

void active() {
#if POWER_MODE
    if (g_isActive) return;
    setCpuFrequencyMhz(CPU_MHZ_ACTIVE);
    g_isActive = true;
    // Небольшая пауза: после смены частоты перенастраиваются делители UART
    // и SPI, и первая же операция сразу после вызова иногда портится.
    delay(5);
#endif
}

void idle() {
#if POWER_MODE
    if (!g_isActive) return;
    setCpuFrequencyMhz(CPU_MHZ_IDLE);
    g_isActive = false;
    delay(5);
#endif
}

uint32_t cpuMhz() { return getCpuFrequencyMhz(); }

bool sleepUntilButton(int buttonPin, uint32_t timeoutMs) {
#if POWER_MODE < 2
    // В режимах 0 и 1 не спим, просто отдаём процессор планировщику.
    delay(timeoutMs > 50 ? 50 : timeoutMs);
    return digitalRead(buttonPin) == LOW;
#else
    // Кнопка замыкает пин на землю, значит будит НИЗКИЙ уровень.
    // ext0 работает только на RTC-выводах; у ESP32-S3 это GPIO 0..21,
    // и BUTTON_PIN должен быть из этого диапазона.
    if (rtc_gpio_is_valid_gpio((gpio_num_t)buttonPin)) {
        rtc_gpio_pullup_en((gpio_num_t)buttonPin);
        rtc_gpio_pulldown_dis((gpio_num_t)buttonPin);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)buttonPin, 0);
    } else {
        Serial.printf("[pwr] GPIO %d не RTC-вывод, лёгкий сон по кнопке "
                      "невозможен — работаю без сна\n", buttonPin);
        delay(50);
        return digitalRead(buttonPin) == LOW;
    }

    // Таймер как страховка: даже если кнопку никто не нажмёт, надо
    // просыпаться и проверять линк, иначе пост «пропадёт» для сервера.
    esp_sleep_enable_timer_wakeup((uint64_t)timeoutMs * 1000ULL);

    Serial.flush();          // иначе хвост строки потеряется в момент засыпания
    esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    return cause == ESP_SLEEP_WAKEUP_EXT0;
#endif
}

}  // namespace power
