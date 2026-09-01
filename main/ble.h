#ifndef H_BLE_DESKCLOCK_
#define H_BLE_DESKCLOCK_

#include <driver/i2c_master.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise NimBLE and start advertising. Runs permanently as a background
 * task — call once after NVS is initialised. The BLE stack is never stopped.
 */
void ble_init(i2c_master_dev_handle_t rtc_dev);

/** True while a phone is actively connected. */
bool ble_is_connected(void);

/**
 * Update the BLE Battery Level characteristic (standard service 0x180F).
 * Sends a NOTIFY to the connected client if one is subscribed.
 * Safe to call from any FreeRTOS task.
 */
void ble_update_battery(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif // H_BLE_DESKCLOCK_
