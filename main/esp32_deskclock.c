#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <utilities.h>
#include "nvs_flash.h"
#include "clock.h"
#include "display.h"
#include "battery.h"
#include "ble.h"
#include "weather.h"

#define TAG "main"

static i2c_master_dev_handle_t s_rtc_dev;
static TaskHandle_t            s_display_task  = NULL;
static volatile float          s_battery_voltage = 4.2f;

#define NOTIFY_BUTTON (1u << 0)

/* ---------- button ISR ---------- */

static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_display_task, NOTIFY_BUTTON, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---------- display helpers ---------- */

static void do_display_update(bool full_clear)
{
    char time_str[16], date_str[64];
    struct tm current_time;
    clock_get_time_strings(time_str, date_str, &current_time);

    /* Weather: show live data while connected, "--" when disconnected */
    char weather_str[64] = "--";
    if (ble_is_connected()) {
        weather_data_t wx;
        if (weather_load(&wx)) {
            weather_format_display(&wx, weather_str, sizeof(weather_str));
        }
    }

    /* Full-clear every 30 minutes to prevent e-ink ghosting */
    bool clear = full_clear || (current_time.tm_min % 30 == 0);

    display_draw_time_and_date(time_str, date_str, weather_str,
                               clear,
                               battery_is_low(s_battery_voltage),
                               ble_is_connected());
    display_poweroff();
}

/* ---------- tasks ---------- */

static void display_task(void *arg)
{
    do_display_update(true);

    while (1) {
        /* Sleep precisely until the next minute boundary */
        uint64_t sleep_us = clock_calculate_sleep_time_until_next_minute();
        TickType_t wait_ticks = pdMS_TO_TICKS(sleep_us / 1000);

        uint32_t notification = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification, wait_ticks);

        /* Button press forces a full clear; normal minute tick does not */
        bool full_clear = (notification & NOTIFY_BUTTON) != 0;
        do_display_update(full_clear);
    }
}

static void battery_task(void *arg)
{
    while (1) {
        float v = battery_read_voltage();
        s_battery_voltage = v;
        uint8_t pct = battery_get_percent(v);
        ble_update_battery(pct);
        ESP_LOGI(TAG, "Battery %.2fV %d%%", v, pct);

        if (battery_is_critical(v)) {
            ESP_LOGW(TAG, "Battery critical — deep sleep until charged");
            display_draw_error("Low battery");
            display_poweroff();
            /* No timer wakeup — wake only on button press once charged */
            rtc_gpio_init(BUTTON_1);
            rtc_gpio_set_direction(BUTTON_1, RTC_GPIO_MODE_INPUT_ONLY);
            rtc_gpio_pullup_en(BUTTON_1);
            esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_1, ESP_EXT1_WAKEUP_ANY_LOW);
            esp_deep_sleep_start();
        }

        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    }
}

/* ---------- app_main ---------- */

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* Display first so we can show error / low-battery messages early */
    display_init();

    /* Early critical battery check before starting the full system */
    s_battery_voltage = battery_read_voltage();
    if (battery_is_critical(s_battery_voltage)) {
        ESP_LOGW(TAG, "Battery critical on boot — deep sleep");
        display_draw_error("Low battery");
        display_poweroff();
        rtc_gpio_init(BUTTON_1);
        rtc_gpio_set_direction(BUTTON_1, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(BUTTON_1);
        esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_1, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    }

    /* Dynamic frequency scaling: CPU runs at 10 MHz when idle, 80 MHz when active.
     * Light sleep is disabled — it breaks BLE advertising on ESP32-S3 without an
     * accurate 32 kHz external sleep clock. Requires CONFIG_PM_ENABLE=y. */
    esp_pm_config_t pm = {
        .max_freq_mhz       = 80,
        .min_freq_mhz       = 10,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm);

    /* RTC — read PCF8563 once to seed system time; after that settimeofday()
     * keeps it current whenever the phone writes the time characteristic */
    i2c_master_bus_handle_t bus_handle;
    if (!clock_init(&bus_handle, &s_rtc_dev)) {
        ESP_LOGE(TAG, "RTC init failed");
        display_draw_error("RTC Error");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    clock_update_from_pcf8563(s_rtc_dev);

    /* BLE starts advertising immediately in the background */
    ble_init(s_rtc_dev);

    /* Tasks — create display task before installing button ISR */
    xTaskCreate(display_task, "display", 4096, NULL, 5, &s_display_task);
    xTaskCreate(battery_task, "battery", 4096, NULL, 3, NULL);

    /* Button: falling edge → notify display task for immediate refresh */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_1,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }
    gpio_isr_handler_add(BUTTON_1, button_isr, NULL);
}
