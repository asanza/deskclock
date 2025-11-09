#include <stdio.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <utilities.h>
#include "ble_time_sync.h"
#include "clock.h"
#include "display.h"
#include "battery.h"

#define TAG "main"

// RTC memory to persist across deep sleep
RTC_DATA_ATTR int32_t last_time_w = 0;

static void
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
    
    while (gpio_get_level(BUTTON_1) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Debounce delay and cleanup
    vTaskDelay(pdMS_TO_TICKS(200));
    rtc_gpio_deinit(BUTTON_1);
}

static void
handle_button_pairing(void)
{
    ESP_LOGI(TAG, "Button 1 pressed - starting BLE pairing mode");
    
    if (!ble_init()) {
        ESP_LOGE(TAG, "Failed to initialize BLE");
        return;
    }
    
    // Start pairing advertising with 60 second timeout
    if (ble_start_pairing_advertising("DeskClock", 60000)) {
        ESP_LOGI(TAG, "Successfully paired with phone!");
        ESP_LOGI(TAG, "Bond should now be stored in NVS");
    } else {
        ESP_LOGW(TAG, "Pairing failed or timed out");
    }
    
    ble_deinit();
}

static void
handle_bonded_device_sync(void)
{
    if (!ble_init()) {
        ESP_LOGE(TAG, "Failed to initialize BLE");
        return;
    }
    
    if (ble_is_bonded()) {
        ESP_LOGI(TAG, "Bonded device found, connecting to read data...");
        
        ble_clock_data_t data;
        if (ble_connect_and_read_data(&data)) {
            ESP_LOGI(TAG, "Data received from phone");
            ESP_LOGI(TAG, "  Time: %s", data.current_time);
            ESP_LOGI(TAG, "  Date: %s", data.current_date);
            ESP_LOGI(TAG, "  Weather: %s", data.weather);
            
            // TODO: Update display with received data
            // TODO: Update RTC if time data is received
        } else {
            ESP_LOGW(TAG, "Failed to read data from bonded device");
        }
    } else {
        ESP_LOGI(TAG, "No bonded device, skipping BLE connection");
    }
    
    ble_deinit();
}

static void
configure_deep_sleep(void)
{
    // Configure button wakeup
    rtc_gpio_init(BUTTON_1);
    rtc_gpio_set_direction(BUTTON_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BUTTON_1);
    rtc_gpio_pulldown_dis(BUTTON_1);
    rtc_gpio_hold_dis(BUTTON_1);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_1, ESP_EXT1_WAKEUP_ANY_LOW);

    // Configure timer wakeup
    uint64_t sleep_time_us = clock_calculate_sleep_time_until_next_minute();
    esp_sleep_enable_timer_wakeup(sleep_time_us);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    ESP_LOGI(TAG, "Entering deep sleep for %llu us", sleep_time_us);
}

void
app_main(void)
{
    // Initialize clock and RTC
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;

    // Check battery level
    float battery_voltage = battery_read_voltage();
    ESP_LOGI(TAG, "Battery voltage: %.2fV", battery_voltage);

    if (battery_is_critical(battery_voltage)) {
        ESP_LOGW(TAG, "Battery critically low - shutting down");
        esp_deep_sleep_start();
    }

    // Initialize display
    display_init();

    if (!clock_init(&bus_handle, &dev_handle)) {
        ESP_LOGE(TAG, "Failed to initialize clock");
        display_draw_error("RTC Error");
        esp_deep_sleep_start();
    }

    // Determine wakeup reason
    esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "Reset reason: %d", reset_reason);
    ESP_LOGI(TAG, "Wakeup reason: %d", wakeup_reason);
    
    bool button_pressed = false;
    bool reset_button_pressed = false;

    if (reset_reason == ESP_RST_EXT || reset_reason == ESP_RST_POWERON ) {
        ESP_LOGI(TAG, "Reset button pressed");
        reset_button_pressed = true;
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        if (esp_sleep_get_ext1_wakeup_status() & (1ULL << BUTTON_1)) {
            ESP_LOGI(TAG, "Woke up from button 1 press");
            button_pressed = true;
        }
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Power-on reset");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Woke up from timer");
    }

    // Sync internal RTC from external RTC
    clock_update_from_pcf8563(dev_handle);

    // Get current time and format for display
    char      time_str[16];
    char      date_str[64];
    struct tm current_time;
    clock_get_time_strings(time_str, date_str, &current_time);

    // Update display
    bool full_clear = (current_time.tm_min % 30 == 0) || reset_button_pressed;
    bool show_battery_icon = battery_is_low(battery_voltage);
    display_draw_time_and_date(time_str, date_str, NULL, full_clear, show_battery_icon);
    display_poweroff();

    // Configure and enter deep sleep
    configure_deep_sleep();
    esp_deep_sleep_start();
}
