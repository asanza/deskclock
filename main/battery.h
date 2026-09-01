#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#define BATTERY_LOW_THRESHOLD 3.4f // Warning at 3.4V (~20% capacity)
#define BATTERY_CRITICAL_THRESHOLD 3.2f // Shutdown at 3.2V

/**
 * @brief Read battery voltage
 * 
 * @return Battery voltage in volts
 */
float battery_read_voltage(void);

/**
 * @brief Check if battery is low
 * 
 * @param voltage Battery voltage
 * @return true if battery is below warning threshold
 */
bool battery_is_low(float voltage);

/**
 * @brief Check if battery is critically low
 *
 * @param voltage Battery voltage
 * @return true if battery is below critical threshold
 */
bool battery_is_critical(float voltage);

/**
 * @brief Convert battery voltage to percentage (0–100)
 *
 * Uses a linear approximation over the LiPo range 3.2 V – 4.2 V.
 */
uint8_t battery_get_percent(float voltage);

#endif // BATTERY_H
