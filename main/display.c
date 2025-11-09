#include "display.h"
#include <epd_driver.h>
#include <Quicksand_140.h>
#include <Quicksand_28.h>
#include <Quicksand_18.h>
#include <batt_icon.h>
#include <esp_log.h>

#define TAG "display"

// RTC memory to persist last time width across deep sleep
extern int32_t last_time_w;

// Font properties for all text rendering
static const FontProperties font_props = {
    .fg_color       = 15,
    .bg_color       = 0,
    .fallback_glyph = 0,
    .flags          = 0,
};

void
display_init(void)
{
    epd_init();
    ESP_LOGI(TAG, "Display initialized");
}

void
display_poweroff(void)
{
    epd_poweroff();
    ESP_LOGI(TAG, "Display powered off");
}

void
display_draw_icon(const void *img_ptr, int x, int y)
{
    const GFXimage *img = (const GFXimage *)img_ptr;

    Rect_t icon_area = {
        .x      = x,
        .y      = y,
        .width  = img->width,
        .height = img->height,
    };

    epd_draw_image(icon_area, (uint8_t *)img->data, BLACK_ON_WHITE);
    ESP_LOGI(TAG, "Icon displayed at (%d, %d)", x, y);
}

void
display_get_time_bounds(const char *time_str, int32_t *width, int32_t *height)
{
    int32_t x = 0, y = 0, x1, y1;
    get_text_bounds((GFXfont *)&Quicksand_140, time_str, &x, &y, &x1, &y1,
                    width, height, &font_props);
}

void
display_get_date_bounds(const char *date_str, int32_t *width, int32_t *height)
{
    int32_t x = 0, y = 0, x1, y1;
    get_text_bounds((GFXfont *)&Quicksand_28, date_str, &x, &y, &x1, &y1, width,
                    height, &font_props);
}

void
display_get_timezone_bounds(const char *timezone_str, int32_t *width,
                            int32_t *height)
{
    int32_t x = 0, y = 0, x1, y1;
    get_text_bounds((GFXfont *)&Quicksand_18, timezone_str, &x, &y, &x1, &y1,
                    width, height, &font_props);
}

int32_t
display_draw_time(const char *time_str, int32_t x, int32_t y)
{
    int32_t original_x = x;
    writeln((GFXfont *)&Quicksand_140, time_str, &x, &y, NULL);
    return x - original_x;
}

int32_t
display_draw_date(const char *date_str, int32_t x, int32_t y)
{
    int32_t original_x = x;
    writeln((GFXfont *)&Quicksand_28, date_str, &x, &y, NULL);
    return x - original_x;
}

int32_t
display_draw_timezone(const char *timezone_str, int32_t x, int32_t y)
{
    int32_t original_x = x;
    writeln((GFXfont *)&Quicksand_18, timezone_str, &x, &y, NULL);
    return x - original_x;
}

void
display_draw_time_and_date(const char *time_str, const char *date_str,
                           const char *timezone_str, bool full_clear,
                           bool show_battery_icon)
{
    // Get dimensions for all elements
    int32_t time_w, time_h;
    int32_t date_w, date_h;
    int32_t timezone_w = 0, timezone_h = 0;

    display_get_time_bounds(time_str, &time_w, &time_h);
    display_get_date_bounds(date_str, &date_w, &date_h);

    if (timezone_str != NULL) {
        display_get_timezone_bounds(timezone_str, &timezone_w, &timezone_h);
    }

    // Calculate vertical spacing and layout
    const int32_t time_to_date_spacing     = 60;
    const int32_t date_to_timezone_spacing = 30;

    int32_t total_h = time_h + time_to_date_spacing + date_h;
    if (timezone_str != NULL) {
        total_h += date_to_timezone_spacing + timezone_h;
    }

    // Calculate Y positions (baseline positions)
    int32_t time_y     = (EPD_HEIGHT - total_h) / 2 + time_h;
    int32_t date_y     = time_y + time_to_date_spacing + date_h;
    int32_t timezone_y = date_y + date_to_timezone_spacing + timezone_h;

    // Calculate X positions (centered)
    int32_t time_x     = (EPD_WIDTH - time_w) / 2;
    int32_t date_x     = (EPD_WIDTH - date_w) / 2;
    int32_t timezone_x = (EPD_WIDTH - timezone_w) / 2;

    if (full_clear) {
        // Full screen refresh
        ESP_LOGI(TAG, "Full screen refresh");
        epd_clear();

        // Draw all elements
        display_draw_time(time_str, time_x, time_y);
        display_draw_date(date_str, date_x, date_y);

        if (timezone_str != NULL) {
            display_draw_timezone(timezone_str, timezone_x, timezone_y);
        }

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

        Rect_t area = {
            .x      = min_x - 20,
            .y      = time_y - time_h - 20,
            .width  = clear_width + 40,
            .height = time_h + 40,
        };

        ESP_LOGI(TAG, "Partial refresh - time only");
        epd_clear_area_cycles(area, 1, 20);
        // epd_clear_area(area);

        display_draw_time(time_str, time_x, time_y);

        last_time_w = time_w;
    }
}
