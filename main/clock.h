#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <time.h>
#include <driver/i2c_master.h>

/**
 * @brief Initialize the I2C bus and PCF8563 RTC
 * 
 * @param bus_handle_out Pointer to store bus handle
 * @param dev_handle_out Pointer to store device handle
 * @return true if successful, false otherwise
 */
bool clock_init(i2c_master_bus_handle_t *bus_handle_out, 
                i2c_master_dev_handle_t *dev_handle_out);

/**
 * @brief Update internal RTC from PCF8563 external RTC
 * 
 * @param dev Device handle
 */
void clock_update_from_pcf8563(i2c_master_dev_handle_t dev);

/**
 * @brief Set the PCF8563 RTC time
 * 
 * @param dev Device handle
 * @param time Time structure to set
 */
void clock_set_time(i2c_master_dev_handle_t dev, struct tm *time);

/**
 * @brief Get current time and format as strings
 * 
 * @param time_str Buffer for time string (min 16 bytes)
 * @param date_str Buffer for date string (min 64 bytes)
 * @param tm_out Optional pointer to store tm structure
 */
void clock_get_time_strings(char *time_str, char *date_str, struct tm *tm_out);

/**
 * @brief Calculate microseconds until next minute boundary
 * 
 * @return Microseconds to sleep
 */
uint64_t clock_calculate_sleep_time_until_next_minute(void);

#endif // CLOCK_H
