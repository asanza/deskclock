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
 * @param date_str Date string (e.g., "Monday, January 1 2025")
 * @param full_clear If true, do full screen refresh; if false, partial refresh
 * @param show_battery_icon If true, show low battery warning icon
 */
void display_draw_time_and_date(const char *time_str, const char *date_str, 
                                bool full_clear, bool show_battery_icon);

/**
 * @brief Draw an icon on the display
 * 
 * @param img Pointer to GFXimage structure
 * @param x X coordinate
 * @param y Y coordinate
 */
void display_draw_icon(const void *img, int x, int y);

#endif // DISPLAY_H
