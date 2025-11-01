#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <driver/i2c.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <epd_driver.h>
#include <Quicksand_140.h>
#include <Quicksand_28.h>
#include <pcf8563.h>
#include "ble_time_sync.h"

#define TAG "main"

// RTC memory to persist across deep sleep
RTC_DATA_ATTR static int last_hour = -1;
RTC_DATA_ATTR static int last_day = -1;
RTC_DATA_ATTR static int last_minute = -1;
RTC_DATA_ATTR static int32_t last_time_w = 0;
RTC_DATA_ATTR static int32_t last_date_w = 0;

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

    // Get time from ESP32's internal RTC
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    // Format time string (HH:MM)
    sprintf(time_str, "%02d:%02d", tm.tm_hour, tm.tm_min);

    // Format date string
    const char *weekdays[] = {
        "Sunday",   "Monday", "Tuesday",  "Wednesday",
        "Thursday", "Friday", "Saturday",
    };
    const char *months[] = {
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December",
    };

    sprintf(date_str, "%s, %s %d %d", weekdays[tm.tm_wday], months[tm.tm_mon],
            tm.tm_mday, tm.tm_year + 1900);

    // Copy tm struct if requested
    if (tm_out) {
        *tm_out = tm;
    }
}

static void
determine_refresh_type(const struct tm *current_time, bool *full_refresh, 
                       bool *refresh_hour, bool *refresh_date)
{
    *full_refresh = false;
    *refresh_hour = false;
    *refresh_date = false;

    ESP_LOGI(TAG, "Current: day=%d hour=%d min=%d | Last: day=%d hour=%d min=%d",
             current_time->tm_mday, current_time->tm_hour, current_time->tm_min,
             last_day, last_hour, last_minute);

    // Check if this is the very first boot (no state saved)
    if (last_day == -1) {
        *full_refresh = true;
        last_day = current_time->tm_mday;
        last_hour = current_time->tm_hour;
        last_minute = current_time->tm_min;
        ESP_LOGI(TAG, "Full refresh triggered (first boot)");
    }
    // Check if day changed (midnight passed)
    else if (last_day != current_time->tm_mday) {
        *full_refresh = true;
        last_day = current_time->tm_mday;
        last_hour = current_time->tm_hour;
        last_minute = current_time->tm_min;
        ESP_LOGI(TAG, "Full refresh triggered (new day)");
    }
    // Check if hour changed
    else if (last_hour != current_time->tm_hour) {
        *refresh_hour = true;
        *refresh_date = true;
        last_hour = current_time->tm_hour;
        last_minute = current_time->tm_min;
        ESP_LOGI(TAG, "Hour changed - refreshing time and date");
    }
    // Only minute changed (or same minute - no refresh needed)
    else {
        if (last_minute != current_time->tm_min) {
            last_minute = current_time->tm_min;
            ESP_LOGI(TAG, "Minute changed - refreshing time only");
            // Note: all flags remain false - this triggers minute-only refresh
        } else {
            ESP_LOGI(TAG, "Same minute - no refresh needed");
        }
    }
}

static void
calculate_text_dimensions(const char *time_str, const char *date_str,
                         const FontProperties *props,
                         int32_t *time_w, int32_t *time_h,
                         int32_t *date_w, int32_t *date_h)
{
    int32_t x = 0, y = 0;
    int32_t x1, y1, w;

    get_text_bounds((GFXfont *)&Quicksand_140, time_str, &x, &y, &x1, &y1, &w, time_h, props);
    *time_w = w;
    
    get_text_bounds((GFXfont *)&Quicksand_28, date_str, &x, &y, &x1, &y1, &w, date_h, props);
    *date_w = w;
}

static void
calculate_positions(int32_t time_w, int32_t time_h, int32_t date_h,
                   int32_t *time_x, int32_t *time_y,
                   int32_t *date_x, int32_t *date_y)
{
    const int32_t spacing = 60;
    int32_t total_h = time_h + spacing + date_h;

    *time_x = (EPD_WIDTH - time_w) / 2;
    *time_y = (EPD_HEIGHT - total_h) / 2 + time_h;
    *date_x = (EPD_WIDTH - *date_x) / 2;  // Will be recalculated with actual date width
    *date_y = *time_y + spacing + date_h;
}

static void
draw_full_screen(const char *time_str, const char *date_str,
                int32_t time_x, int32_t time_y,
                int32_t date_x, int32_t date_y,
                int32_t time_w, int32_t date_w)
{
    ESP_LOGI(TAG, "Performing full screen refresh");
    epd_clear();

    int32_t x = time_x;
    int32_t y = time_y;
    writeln((GFXfont *)&Quicksand_140, time_str, &x, &y, NULL);

    x = date_x;
    y = date_y;
    writeln((GFXfont *)&Quicksand_28, date_str, &x, &y, NULL);

    // Store current dimensions for next partial refresh
    last_time_w = time_w;
    last_date_w = date_w;
}

static void
clear_and_draw_area(const char *text, const GFXfont *font,
                   int32_t text_x, int32_t text_y,
                   int32_t text_w, int32_t text_h,
                   int32_t last_w, const char *area_name)
{
    // Use maximum of old and new width to handle variable-width fonts
    int32_t max_w = (last_w > text_w) ? last_w : text_w;
    int32_t clear_x = (EPD_WIDTH - max_w) / 2;

    Rect_t area = {
        .x = clear_x - 20,
        .y = text_y - text_h - 20,
        .width = max_w + 40,
        .height = text_h + 40
    };

    ESP_LOGI(TAG, "Clearing %s area: x=%d, y=%d, w=%d, h=%d (old_w=%d, new_w=%d)",
             area_name, area.x, area.y, area.width, area.height, last_w, text_w);
    epd_clear_area(area);

    int32_t x = text_x;
    int32_t y = text_y;
    writeln(font, text, &x, &y, NULL);
}

static void
draw_partial_hour_change(const char *time_str, const char *date_str,
                        int32_t time_x, int32_t time_y, int32_t time_w, int32_t time_h,
                        int32_t date_x, int32_t date_y, int32_t date_w, int32_t date_h)
{
    // Clear and redraw time area
    clear_and_draw_area(time_str, (GFXfont *)&Quicksand_140,
                       time_x, time_y, time_w, time_h, last_time_w, "time");

    // Clear and redraw date area
    clear_and_draw_area(date_str, (GFXfont *)&Quicksand_28,
                       date_x, date_y, date_w, date_h, last_date_w, "date");

    // Store current dimensions for next refresh
    last_time_w = time_w;
    last_date_w = date_w;
}

static void
draw_partial_minute_change(const char *time_str,
                          int32_t time_x, int32_t time_y,
                          int32_t time_w, int32_t time_h)
{
    // Clear and redraw time area only
    clear_and_draw_area(time_str, (GFXfont *)&Quicksand_140,
                       time_x, time_y, time_w, time_h, last_time_w, "time");

    // Store current time width for next refresh
    last_time_w = time_w;
}

static void
update_display(const char *time_str, const char *date_str,
              bool full_refresh, bool refresh_hour,
              int32_t time_x, int32_t time_y, int32_t time_w, int32_t time_h,
              int32_t date_x, int32_t date_y, int32_t date_w, int32_t date_h)
{
    ESP_LOGI(TAG, "update_display: full=%d, hour=%d", full_refresh, refresh_hour);
    
    if (full_refresh) {
        draw_full_screen(time_str, date_str, time_x, time_y, date_x, date_y, time_w, date_w);
    } else if (refresh_hour) {
        draw_partial_hour_change(time_str, date_str, time_x, time_y, time_w, time_h,
                                date_x, date_y, date_w, date_h);
    } else {
        draw_partial_minute_change(time_str, time_x, time_y, time_w, time_h);
    }
}

static uint64_t
calculate_sleep_time_until_next_minute(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int seconds_in_minute = tv.tv_sec % 60;
    int microseconds = tv.tv_usec;

    // Time remaining until next minute in microseconds
    return ((60 - seconds_in_minute) * 1000000ULL) - microseconds;
}

static void
handle_time_sync(i2c_master_dev_handle_t dev_handle, esp_sleep_wakeup_cause_t wakeup_reason)
{
    // Only attempt BLE sync on first boot (not after deep sleep)
    if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "First boot - attempting BLE time sync");
        struct tm received_time;
        if (ble_sync_time(&received_time)) {
            ESP_LOGI(TAG, "BLE time sync successful, updating PCF8563");
            set_rtc_time(dev_handle, &received_time);
        } else {
            ESP_LOGW(TAG, "BLE time sync failed or not available");
        }
    } else {
        ESP_LOGI(TAG, "Woke from deep sleep");
    }
}

void
app_main(void)
{
    ESP_LOGI(TAG, "=== Wakeup - RTC state: day=%d hour=%d min=%d ===", last_day, last_hour, last_minute);
    
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

    // Handle time synchronization (BLE on first boot)
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    handle_time_sync(dev_handle, wakeup_reason);

    // Update internal RTC from PCF8563 on every wakeup
    update_internal_rtc_from_pcf8563(dev_handle);

    // Get current time and date from internal RTC
    char time_str[16];
    char date_str[64];
    struct tm current_time;
    get_rtc_time(time_str, date_str, &current_time);

    // Determine what type of refresh is needed
    bool full_refresh, refresh_hour, refresh_date;
    determine_refresh_type(&current_time, &full_refresh, &refresh_hour, &refresh_date);

    // Calculate text dimensions
    FontProperties props = {
        .fg_color = 15, .bg_color = 0, .fallback_glyph = 0, .flags = 0
    };

    int32_t time_w, time_h, date_w, date_h;
    calculate_text_dimensions(time_str, date_str, &props, &time_w, &time_h, &date_w, &date_h);

    // Calculate positions for centered text
    int32_t time_x, time_y, date_x, date_y;
    calculate_positions(time_w, time_h, date_h, &time_x, &time_y, &date_x, &date_y);
    date_x = (EPD_WIDTH - date_w) / 2;  // Recalculate with actual date width

    // Update the display based on refresh type
    update_display(time_str, date_str, full_refresh, refresh_hour,
                  time_x, time_y, time_w, time_h,
                  date_x, date_y, date_w, date_h);

    epd_poweroff();

    // Calculate sleep time and enter deep sleep
    uint64_t sleep_time_us = calculate_sleep_time_until_next_minute();
    ESP_LOGI(TAG, "Stored state before sleep: day=%d hour=%d min=%d", last_day, last_hour, last_minute);
    ESP_LOGI(TAG, "Going to deep sleep for %llu microseconds (until next minute)...", sleep_time_us);
    
    // Keep RTC memory powered during deep sleep
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_timer_wakeup(sleep_time_us);

    esp_deep_sleep_start();
}
