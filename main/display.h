#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the e-paper display
 */
void display_init(void);

/**
 * @brief Power off the display to save power
 */
void display_poweroff(void);

/**
 * @brief Draw time and date on the display
 * 
 * @param time_str Time string (e.g., "14:30")
 * @param date_str Date string (e.g., "Monday, January 1 2024")
 * @param timezone_str Optional timezone string (e.g., "Beijing 00:00 | Quito 00:00"), can be NULL
 * @param full_clear If true, do full screen refresh; if false, partial refresh
 * @param show_battery_icon If true, show battery icon in corner
 */
void display_draw_time_and_date(const char *time_str, const char *date_str,
                                const char *timezone_str, bool full_clear, 
                                bool show_battery_icon);

/**
 * @brief Draw main time display
 * 
 * @param time_str Time string to display
 * @param x X position to draw at
 * @param y Y position (baseline) to draw at
 * @return Width of the rendered text
 */
int32_t display_draw_time(const char *time_str, int32_t x, int32_t y);

/**
 * @brief Draw date display
 * 
 * @param date_str Date string to display
 * @param x X position to draw at
 * @param y Y position (baseline) to draw at
 * @return Width of the rendered text
 */
int32_t display_draw_date(const char *date_str, int32_t x, int32_t y);

/**
 * @brief Draw timezone information line
 * 
 * @param timezone_str Timezone string to display
 * @param x X position to draw at
 * @param y Y position (baseline) to draw at
 * @return Width of the rendered text
 */
int32_t display_draw_timezone(const char *timezone_str, int32_t x, int32_t y);

/**
 * @brief Calculate dimensions for time text
 * 
 * @param time_str Time string
 * @param width Output: text width
 * @param height Output: text height
 */
void display_get_time_bounds(const char *time_str, int32_t *width, int32_t *height);

/**
 * @brief Calculate dimensions for date text
 * 
 * @param date_str Date string
 * @param width Output: text width
 * @param height Output: text height
 */
void display_get_date_bounds(const char *date_str, int32_t *width, int32_t *height);

/**
 * @brief Calculate dimensions for timezone text
 * 
 * @param timezone_str Timezone string
 * @param width Output: text width
 * @param height Output: text height
 */
void display_get_timezone_bounds(const char *timezone_str, int32_t *width, int32_t *height);

/**
 * @brief Draw an icon at specified position
 * 
 * @param img_ptr Pointer to GFXimage structure
 * @param x X coordinate
 * @param y Y coordinate
 */
void display_draw_icon(const void *img_ptr, int x, int y);

/**
 * @brief Display an error message centered on the screen
 * 
 * Clears the entire screen and displays the error message in the center.
 * 
 * @param str Error message to display
 */
void display_draw_error(const char *str);

#endif // DISPLAY_H
