#include <stdio.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
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

static uint32_t
wait_for_button_release(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << BUTTON_1),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    uint32_t press_duration_ms = 0;
    while (gpio_get_level(BUTTON_1) == 0 && press_duration_ms <= 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        press_duration_ms += 10;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    rtc_gpio_deinit(BUTTON_1);

    return press_duration_ms;
}

static void
configure_deep_sleep(void)
{
    rtc_gpio_init(BUTTON_1);
    rtc_gpio_set_direction(BUTTON_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BUTTON_1);
    rtc_gpio_pulldown_dis(BUTTON_1);
    rtc_gpio_hold_dis(BUTTON_1);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_1, ESP_EXT1_WAKEUP_ANY_LOW);

    uint64_t sleep_time_us = clock_calculate_sleep_time_until_next_minute();
    esp_sleep_enable_timer_wakeup(sleep_time_us);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    ESP_LOGI(TAG, "Entering deep sleep for %llu us", sleep_time_us);
}

void
app_main(void)
{
    /* NVS must be initialised before weather_load() or start_ble() */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    float battery_voltage = battery_read_voltage();
    ESP_LOGI(TAG, "Battery voltage: %.2fV", battery_voltage);

    if (battery_is_critical(battery_voltage)) {
        ESP_LOGW(TAG, "Battery critically low - shutting down");
        esp_deep_sleep_start();
    }

    display_init();

    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    if (!clock_init(&bus_handle, &dev_handle)) {
        ESP_LOGE(TAG, "Failed to initialize clock");
        display_draw_error("RTC Error");
        esp_deep_sleep_start();
    }

    esp_reset_reason_t      reset_reason  = esp_reset_reason();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "Reset reason: %d  Wakeup reason: %d", reset_reason, wakeup_reason);

    bool     button_pressed       = false;
    bool     button_long_pressed  = false;
    bool     reset_button_pressed = false;
    uint32_t press_duration_ms    = 0;

    if (reset_reason == ESP_RST_EXT || reset_reason == ESP_RST_POWERON) {
        reset_button_pressed = true;
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        if (esp_sleep_get_ext1_wakeup_status() & (1ULL << BUTTON_1)) {
            button_pressed     = true;
            press_duration_ms  = wait_for_button_release();
            ESP_LOGI(TAG, "Button held for %lu ms", press_duration_ms);
            if (press_duration_ms >= 2000) {
                button_long_pressed = true;
                ESP_LOGI(TAG, "Long press detected");
            }
        }
    }

    /* ---- Sync time and get current minute ---- */
    char      time_str[16];
    char      date_str[64];
    struct tm current_time;
    bool      show_battery_icon = battery_is_low(battery_voltage);

    clock_update_from_pcf8563(dev_handle);
    clock_get_time_strings(time_str, date_str, &current_time);

    /* Hourly automatic sync: fire on the first minute of each UTC hour.
     * Only on timer wakeup to avoid triggering on button or power-on. */
    bool hourly_sync = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER)
                       && (current_time.tm_min == 0);

    bool do_ble = button_long_pressed || hourly_sync;

    if (do_ble) {
        uint32_t ble_timeout = button_long_pressed ? 60000 : 45000;
        ESP_LOGI(TAG, "%s BLE sync (timeout %lus)",
                 hourly_sync ? "Hourly" : "Manual", (unsigned long)(ble_timeout / 1000));

        display_draw_time_and_date(time_str, date_str, NULL,
                                   false, show_battery_icon, true);
        display_poweroff();

        start_ble(dev_handle, ble_timeout);

        /* Re-read time and weather after sync */
        clock_update_from_pcf8563(dev_handle);
        clock_get_time_strings(time_str, date_str, &current_time);
    }

    /* ---- Build weather display string ---- */
    char weather_str[64] = "";
    weather_data_t wx;
    if (weather_load(&wx)) {
        uint32_t age = weather_age_seconds();
        if (age < 7200) {
            weather_format_display(&wx, weather_str, sizeof(weather_str));
        } else if (wx.sunrise_min > 0 || wx.sunset_min > 0) {
            snprintf(weather_str, sizeof(weather_str), "^%02u:%02u  v%02u:%02u",
                     wx.sunrise_min / 60, wx.sunrise_min % 60,
                     wx.sunset_min  / 60, wx.sunset_min  % 60);
        } else {
            snprintf(weather_str, sizeof(weather_str), "--");
        }
    }

    /* ---- Update display ---- */
    bool full_clear = (current_time.tm_min % 30 == 0)
                      || reset_button_pressed
                      || do_ble;

    display_draw_time_and_date(time_str, date_str,
                               weather_str[0] ? weather_str : NULL,
                               full_clear, show_battery_icon, false);
    display_poweroff();

    configure_deep_sleep();
    esp_deep_sleep_start();
}
