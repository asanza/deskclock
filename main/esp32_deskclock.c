#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <driver/i2c.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <epd_driver.h>
#include <Quicksand_140.h>
#include <Quicksand_28.h>
#include <pcf8563.h>
#include <batt_icon.h>
#include <bt_icon.h>
#include <utilities.h>
#include "ble_time_sync.h"

#define TAG                   "main"
#define BATTERY_LOW_THRESHOLD 3.4f // Warning at 3.4V (~20% capacity)

// RTC memory to persist across deep sleep
RTC_DATA_ATTR static int32_t last_time_w = 0;

static void
update_internal_rtc_from_pcf8563(i2c_master_dev_handle_t dev)
{
    struct tm tm;
    bool      valid;

    ESP_ERROR_CHECK(pcf8563_get_time(dev, &tm, &valid));

    if (valid) {
        struct timeval tv;
        tv.tv_sec  = mktime(&tm);
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        ESP_LOGI(
            TAG,
            "Updated internal RTC from PCF8563: %04d-%02d-%02d %02d:%02d:%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
            tm.tm_sec);
    } else {
        ESP_LOGW(TAG, "PCF8563 time not valid");
    }
}

static void
set_rtc_time(i2c_master_dev_handle_t dev, struct tm *time)
{
    ESP_LOGI(TAG, "Setting RTC time to: %04d-%02d-%02d %02d:%02d:%02d",
             time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
             time->tm_hour, time->tm_min, time->tm_sec);

    ESP_ERROR_CHECK(pcf8563_set_time(dev, time));
    ESP_LOGI(TAG, "RTC time set successfully");
}

static void
get_rtc_time(char *time_str, char *date_str, struct tm *tm_out)
{
    struct timeval tv;
    struct tm      tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    sprintf(time_str, "%02d:%02d", tm.tm_hour, tm.tm_min);

    const char *weekdays[] = { "Sunday",   "Monday", "Tuesday", "Wednesday",
                               "Thursday", "Friday", "Saturday" };
    const char *months[]   = { "January",   "February", "March",    "April",
                               "May",       "June",     "July",     "August",
                               "September", "October",  "November", "December" };

    sprintf(date_str, "%s, %s %d %d", weekdays[tm.tm_wday], months[tm.tm_mon],
            tm.tm_mday, tm.tm_year + 1900);

    if (tm_out) {
        *tm_out = tm;
    }
}

static void
draw_icon(const GFXimage *img, int x, int y)
{
    Rect_t icon_area = {
        .x = x, .y = y, .width = img->width, .height = img->height
    };

    epd_draw_image(icon_area, (uint8_t *)img->data, BLACK_ON_WHITE);
    ESP_LOGI(TAG, "Low battery icon displayed");
}

static void
draw_time_and_date(const char *time_str, const char *date_str, bool full_clear,
                   bool show_battery_icon)
{
    FontProperties props = {
        .fg_color = 15, .bg_color = 0, .fallback_glyph = 0, .flags = 0
    };

    int32_t x = 0, y = 0, x1, y1, w, time_h, date_h;

    // Calculate dimensions
    get_text_bounds((GFXfont *)&Quicksand_140, time_str, &x, &y, &x1, &y1, &w,
                    &time_h, &props);
    int32_t time_w = w;
    get_text_bounds((GFXfont *)&Quicksand_28, date_str, &x, &y, &x1, &y1, &w,
                    &date_h, &props);
    int32_t date_w = w;

    // Calculate positions
    const int32_t spacing = 60;
    int32_t       total_h = time_h + spacing + date_h;
    int32_t       time_x  = (EPD_WIDTH - time_w) / 2;
    int32_t       time_y  = (EPD_HEIGHT - total_h) / 2 + time_h;
    int32_t       date_x  = (EPD_WIDTH - date_w) / 2;
    int32_t       date_y  = time_y + spacing + date_h;

    if (full_clear) {
        // Full screen refresh
        ESP_LOGI(TAG, "Full screen refresh");
        epd_clear();

        x = time_x;
        y = time_y;
        writeln((GFXfont *)&Quicksand_140, time_str, &x, &y, NULL);

        x = date_x;
        y = date_y;
        writeln((GFXfont *)&Quicksand_28, date_str, &x, &y, NULL);

        // Draw battery icon if battery is low
        if (show_battery_icon) {
            draw_icon(&batt, 20, 20);
        }

        last_time_w = time_w;
    } else {
        // Partial refresh - only update time area
        // Calculate clear area based on previous text position to ensure full
        // cleanup
        int32_t last_time_x = (EPD_WIDTH - last_time_w) / 2;
        int32_t min_x       = (last_time_x < time_x) ? last_time_x : time_x;
        int32_t max_x       = (last_time_x + last_time_w > time_x + time_w) ?
                                  (last_time_x + last_time_w) :
                                  (time_x + time_w);
        int32_t clear_width = max_x - min_x;

        Rect_t area = { .x      = min_x - 20,
                        .y      = time_y - time_h - 20,
                        .width  = clear_width + 40,
                        .height = time_h + 40 };

        ESP_LOGI(TAG, "Partial refresh - time only");
        epd_clear_area(area);

        x = time_x;
        y = time_y;
        writeln((GFXfont *)&Quicksand_140, time_str, &x, &y, NULL);

        last_time_w = time_w;
    }
}

static uint64_t
calculate_sleep_time_until_next_minute(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int seconds_in_minute = tv.tv_sec % 60;
    int microseconds      = tv.tv_usec;

    return ((60 - seconds_in_minute) * 1000000ULL) - microseconds;
}

static float
read_battery_voltage(void)
{
    // LilyGo T5 S3: Battery voltage divider is connected to GPIO14 (ADC1_CH3)
    // Voltage divider: 100K + 100K = 2:1 ratio
    // Battery max ~4.2V, ADC reads ~2.1V max
    // ADC reference is 3.3V for ESP32-S3

    adc_oneshot_unit_handle_t   adc_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id  = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12, // Full range: 0-3.3V
    };
    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &config));

    // Read ADC value
    int adc_raw;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &adc_raw));

    // Initialize calibration
    adc_cali_handle_t               adc_cali_handle = NULL;
    adc_cali_curve_fitting_config_t cali_config     = {
            .unit_id  = ADC_UNIT_2,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    esp_err_t ret =
        adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);

    int voltage_mv = 0;
    if (ret == ESP_OK) {
        ESP_ERROR_CHECK(
            adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &voltage_mv));
        adc_cali_delete_scheme_curve_fitting(adc_cali_handle);
    }

    // Clean up
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc_handle));

    // Convert to battery voltage (multiply by 2 for voltage divider)
    float battery_voltage = (voltage_mv * 2.0f) / 1000.0f;

    ESP_LOGI(TAG, "Battery: %d mV (raw: %d) -> %.2f V", voltage_mv, adc_raw,
             battery_voltage);

    return battery_voltage;
}

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
    
    // Initialize BLE
    if (!ble_init()) {
        ESP_LOGE(TAG, "Failed to initialize BLE");
        return;
    }
    
    // Start pairing advertising with 60 second timeout
    if (ble_start_pairing_advertising("DeskClock", 60000)) {
        ESP_LOGI(TAG, "Successfully paired with phone!");
        
        // Optionally read data immediately after pairing
        ble_clock_data_t data;
        if (ble_connect_and_read_data(&data)) {
            ESP_LOGI(TAG, "Data received from phone");
            ESP_LOGI(TAG, "  Time: %s", data.current_time);
            ESP_LOGI(TAG, "  Date: %s", data.current_date);
            ESP_LOGI(TAG, "  Weather: %s", data.weather);
        }
    } else {
        ESP_LOGW(TAG, "Pairing failed or timed out");
    }
    
    // Cleanup
    ble_deinit();
}

void
app_main(void)
{
    // Initialize I2C bus and PCF8563 RTC
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;

    const i2c_master_bus_config_t i2c_config = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = BOARD_SCL,
        .sda_io_num                   = BOARD_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_config, &bus_handle));
    ESP_ERROR_CHECK(pcf8563_init_desc(bus_handle, &dev_handle));

    // Check battery level - shutdown if critically low
    float battery_voltage = read_battery_voltage();
    ESP_LOGI(TAG, "Battery voltage: %.2fV", battery_voltage);

    if (battery_voltage <= 3.2f) {
        ESP_LOGW(TAG, "Battery critically low - shutting down");
        esp_deep_sleep_start();
    }

    // Initialize e-paper display
    epd_init();

    // Check if button was pressed for pairing
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    esp_reset_reason_t reset_reason = esp_reset_reason();
    
    bool button_pressed = false;
    if (reset_reason == ESP_RST_EXT) {
        ESP_LOGI(TAG, "Reset button pressed");
        button_pressed = true;
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        if (esp_sleep_get_ext1_wakeup_status() & (1ULL << BUTTON_1)) {
            ESP_LOGI(TAG, "Woke up from button 1 press");
            wait_for_button_release();
            button_pressed = true;
        }
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Power-on reset");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Woke up from timer");
    }

    // Sync internal RTC from external RTC chip
    update_internal_rtc_from_pcf8563(dev_handle);

    // Get current time and format for display
    char      time_str[16];
    char      date_str[64];
    struct tm current_time;
    get_rtc_time(time_str, date_str, &current_time);

    // Update display (full refresh every 5 minutes, partial otherwise)
    bool full_clear        = (current_time.tm_min % 5 == 0);
    bool show_battery_icon = (battery_voltage < BATTERY_LOW_THRESHOLD);
    draw_time_and_date(time_str, date_str, full_clear, show_battery_icon);
    epd_poweroff();

    // Handle BLE pairing AFTER display is done
    if (button_pressed) {
        handle_button_pairing();
    }

    // Configure button wakeup (GPIO must be RTC-capable with pullup)
    rtc_gpio_init(BUTTON_1);
    rtc_gpio_set_direction(BUTTON_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(BUTTON_1);
    rtc_gpio_pulldown_dis(BUTTON_1);
    rtc_gpio_hold_dis(BUTTON_1);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_1, ESP_EXT1_WAKEUP_ANY_LOW);

    // Configure timer wakeup
    uint64_t sleep_time_us = calculate_sleep_time_until_next_minute();
    esp_sleep_enable_timer_wakeup(sleep_time_us);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    ESP_LOGI(TAG, "Entering deep sleep for %llu us", sleep_time_us);
    esp_deep_sleep_start();
}
