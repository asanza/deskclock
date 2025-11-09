#include "display.h"
#include <epd_driver.h>
#include <Quicksand_140.h>
#include <Quicksand_28.h>
#include <batt_icon.h>
#include <esp_log.h>

#define TAG "display"

// RTC memory to persist last time width across deep sleep
extern int32_t last_time_w;

void display_init(void)
{
    epd_init();
    ESP_LOGI(TAG, "Display initialized");
}

void display_poweroff(void)
{
    epd_poweroff();
    ESP_LOGI(TAG, "Display powered off");
}

void display_draw_icon(const void *img_ptr, int x, int y)
{
    const GFXimage *img = (const GFXimage *)img_ptr;
    Rect_t icon_area = {
        .x = x, .y = y, .width = img->width, .height = img->height
    };

    epd_draw_image(icon_area, (uint8_t *)img->data, BLACK_ON_WHITE);
    ESP_LOGI(TAG, "Icon displayed at (%d, %d)", x, y);
}

void display_draw_time_and_date(const char *time_str, const char *date_str, 
                                bool full_clear, bool show_battery_icon)
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
            display_draw_icon(&batt, 20, 20);
        }

        last_time_w = time_w;
    } else {
        // Partial refresh - only update time area
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
