#include "display.h"
#include <epd_driver.h>
#include <Quicksand_140.h>
#include <Quicksand_28.h>
#include <Quicksand_18.h>
#include <batt_icon.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>

#define TAG "display"

// Global framebuffer
static uint8_t *framebuffer = NULL;

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
    
    // Allocate framebuffer: EPD_WIDTH / 2 * EPD_HEIGHT bytes
    // Each pixel is 4 bits (half byte)
    size_t fb_size = EPD_WIDTH / 2 * EPD_HEIGHT;
    framebuffer = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer (%d bytes)", fb_size);
        return;
    }
    
    // Clear framebuffer
    memset(framebuffer, 0xFF, fb_size);
    
    ESP_LOGI(TAG, "Display initialized with framebuffer (%d bytes)", fb_size);
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
    writeln((GFXfont *)&Quicksand_140, time_str, &x, &y, framebuffer);
    return x - original_x;
}

int32_t
display_draw_date(const char *date_str, int32_t x, int32_t y)
{
    int32_t original_x = x;
    writeln((GFXfont *)&Quicksand_28, date_str, &x, &y, framebuffer);
    return x - original_x;
}

int32_t
display_draw_timezone(const char *timezone_str, int32_t x, int32_t y)
{
    int32_t original_x = x;
    writeln((GFXfont *)&Quicksand_18, timezone_str, &x, &y, framebuffer);
    return x - original_x;
}

void
display_draw_time_and_date(const char *time_str, const char *date_str,
                           const char *timezone_str, bool full_clear,
                           bool show_battery_icon)
{
    // Cached maximum possible time width for clearing partial refresh area.
    // We use a representative widest string composed of the widest digit glyphs.
    int32_t max_time_w = 0;
    int32_t max_time_h = 0;
    static const char *WIDEST_TIME_STR = "88:88"; // Digits '8' typically widest
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

    display_get_time_bounds(WIDEST_TIME_STR, &max_time_w, &max_time_h);
    ESP_LOGI(TAG, "Computed max time bounds: w=%d h=%d for '%s'", max_time_w, max_time_h, WIDEST_TIME_STR);

    if (full_clear) {
        // Full screen refresh
        ESP_LOGI(TAG, "Full screen refresh");
        
        // Clear framebuffer
        memset(framebuffer, 0xFF, EPD_WIDTH / 2 * EPD_HEIGHT);
        
        // Draw all elements to framebuffer
        display_draw_time(time_str, time_x, time_y);
        display_draw_date(date_str, date_x, date_y);

        if (timezone_str != NULL) {
            display_draw_timezone(timezone_str, timezone_x, timezone_y);
        }

        // Draw battery icon if battery is low
        if (show_battery_icon) {
            display_draw_icon(&batt, 20, 20);
        }

        // Clear display and write framebuffer
        epd_clear_area_cycles(epd_full_screen(), 2, 20);
        epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    } else {
        // Partial refresh - clear a fixed maximum area for any time change to avoid ghosting
        // Center position for widest time string
        int32_t max_time_x = (EPD_WIDTH - max_time_w) / 2;

        // Use max_time_h (should match time_h, but we rely on widest precomputed metrics)
        Rect_t area = {
            .x      = max_time_x - 40,
            .y      = time_y - max_time_h - 20,
            .width  = max_time_w + 80,
            .height = max_time_h + 40,
        };

        ESP_LOGI(TAG, "Partial refresh - time only (fixed max area)");

        // Clear only the time area in framebuffer (rest stays from previous full draw)
        epd_fill_rect(area.x, area.y, area.width, area.height, 0xFF, framebuffer);

        // Draw new time to framebuffer at absolute position
        display_draw_time(time_str, time_x, time_y);

        // Perform partial update cycles on that area then push new framebuffer
        epd_clear_area_cycles(area, 1, 20);
        epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    }
}

void display_draw_error(const char *str)
{
    ESP_LOGI(TAG, "Drawing error message: %s", str);
    
    // Clear framebuffer
    memset(framebuffer, 0xFF, EPD_WIDTH / 2 * EPD_HEIGHT);

    // Get text dimensions
    int32_t width, height;
    display_get_date_bounds(str, &width, &height);

    // Calculate centered position
    int32_t x = (EPD_WIDTH - width) / 2;
    int32_t y = (EPD_HEIGHT / 2) + (height / 2);

    // Draw the error text to framebuffer using date font (Quicksand_28)
    display_draw_date(str, x, y);
    
    // Clear display and write framebuffer
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
}
