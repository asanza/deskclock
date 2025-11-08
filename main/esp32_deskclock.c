#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <driver/i2c.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <epd_driver.h>
#include <Quicksand_140.h>
#include <Quicksand_28.h>
#include <pcf8563.h>
#include "ble_time_sync.h"

#define TAG "main"

// RTC memory to persist across deep sleep
RTC_DATA_ATTR static int32_t last_time_w = 0;

static void
update_internal_rtc_from_pcf8563(i2c_master_dev_handle_t dev)
{
    struct tm tm;
    bool valid;

    ESP_ERROR_CHECK(pcf8563_get_time(dev, &tm, &valid));

    if (valid) {
        struct timeval tv;
        tv.tv_sec = mktime(&tm);
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        ESP_LOGI(TAG, "Updated internal RTC from PCF8563: %04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
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
    struct tm tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    sprintf(time_str, "%02d:%02d", tm.tm_hour, tm.tm_min);

    const char *weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char *months[] = {"January", "February", "March", "April", "May", "June",
                           "July", "August", "September", "October", "November", "December"};

    sprintf(date_str, "%s, %s %d %d", weekdays[tm.tm_wday], months[tm.tm_mon],
            tm.tm_mday, tm.tm_year + 1900);

    if (tm_out) {
        *tm_out = tm;
    }
}

static void
draw_time_and_date(const char *time_str, const char *date_str, bool full_clear)
{
    FontProperties props = {.fg_color = 15, .bg_color = 0, .fallback_glyph = 0, .flags = 0};
    
    int32_t x = 0, y = 0, x1, y1, w, time_h, date_h;

    // Calculate dimensions
    get_text_bounds((GFXfont *)&Quicksand_140, time_str, &x, &y, &x1, &y1, &w, &time_h, &props);
    int32_t time_w = w;
    get_text_bounds((GFXfont *)&Quicksand_28, date_str, &x, &y, &x1, &y1, &w, &date_h, &props);
    int32_t date_w = w;

    // Calculate positions
    const int32_t spacing = 60;
    int32_t total_h = time_h + spacing + date_h;
    int32_t time_x = (EPD_WIDTH - time_w) / 2;
    int32_t time_y = (EPD_HEIGHT - total_h) / 2 + time_h;
    int32_t date_x = (EPD_WIDTH - date_w) / 2;
    int32_t date_y = time_y + spacing + date_h;

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
        
        last_time_w = time_w;
    } else {
        // Partial refresh - only update time area
        int32_t max_w = (last_time_w > time_w) ? last_time_w : time_w;
        int32_t clear_x = (EPD_WIDTH - max_w) / 2;

        Rect_t area = {
            .x = clear_x - 20,
            .y = time_y - time_h - 20,
            .width = max_w + 40,
            .height = time_h + 40
        };

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
    int microseconds = tv.tv_usec;

    return ((60 - seconds_in_minute) * 1000000ULL) - microseconds;
}

void
app_main(void)
{
    // Initialize I2C and PCF8563
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;

    const i2c_master_bus_config_t i2c_mst_config = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = BOARD_SCL,
        .sda_io_num                   = BOARD_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));
    ESP_ERROR_CHECK(pcf8563_init_desc(bus_handle, &dev_handle));

    // Initialize e-paper display
    epd_init();

    // Handle BLE time sync on first boot only
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "First boot - attempting BLE time sync");
        struct tm received_time;
        if (ble_sync_time(&received_time)) {
            ESP_LOGI(TAG, "BLE time sync successful");
            set_rtc_time(dev_handle, &received_time);
        }
    }

    // Update internal RTC from PCF8563
    update_internal_rtc_from_pcf8563(dev_handle);

    // Get current time
    char time_str[16];
    char date_str[64];
    struct tm current_time;
    get_rtc_time(time_str, date_str, &current_time);

    // Simple refresh logic: full refresh every 10 minutes, partial otherwise
    bool full_clear = (current_time.tm_min % 10 == 0);
    draw_time_and_date(time_str, date_str, full_clear);

    epd_poweroff();

    // Sleep until next minute
    uint64_t sleep_time_us = calculate_sleep_time_until_next_minute();
    ESP_LOGI(TAG, "Sleeping for %llu microseconds", sleep_time_us);
    
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_timer_wakeup(sleep_time_us);

    esp_deep_sleep_start();
}
