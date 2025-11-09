#include "clock.h"
#include <pcf8563.h>
#include <utilities.h>
#include <esp_log.h>
#include <sys/time.h>
#include <string.h>

#define TAG "clock"

bool clock_init(i2c_master_bus_handle_t *bus_handle_out, 
                i2c_master_dev_handle_t *dev_handle_out)
{
    const i2c_master_bus_config_t i2c_config = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = BOARD_SCL,
        .sda_io_num                   = BOARD_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&i2c_config, bus_handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return false;
    }

    ret = pcf8563_init_desc(*bus_handle_out, dev_handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PCF8563: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Clock initialized");
    return true;
}

void clock_update_from_pcf8563(i2c_master_dev_handle_t dev)
{
    struct tm tm;
    bool      valid;

    esp_err_t ret = pcf8563_get_time(dev, &tm, &valid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read PCF8563: %s", esp_err_to_name(ret));
        return;
    }

    if (valid) {
        struct timeval tv;
        tv.tv_sec  = mktime(&tm);
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);

        ESP_LOGI(TAG, "Updated internal RTC: %04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, 
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        ESP_LOGW(TAG, "PCF8563 time not valid");
    }
}

void clock_set_time(i2c_master_dev_handle_t dev, struct tm *time)
{
    ESP_LOGI(TAG, "Setting RTC time to: %04d-%02d-%02d %02d:%02d:%02d",
             time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
             time->tm_hour, time->tm_min, time->tm_sec);

    esp_err_t ret = pcf8563_set_time(dev, time);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC time set successfully");
    } else {
        ESP_LOGE(TAG, "Failed to set RTC time: %s", esp_err_to_name(ret));
    }
}

void clock_get_time_strings(char *time_str, char *date_str, struct tm *tm_out)
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

uint64_t clock_calculate_sleep_time_until_next_minute(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int seconds_in_minute = tv.tv_sec % 60;
    int microseconds      = tv.tv_usec;

    return ((60 - seconds_in_minute) * 1000000ULL) - microseconds;
}
