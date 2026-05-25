#ifndef H_BLE_DESKCLOCK_
#define H_BLE_DESKCLOCK_

#include <driver/i2c_master.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start BLE advertising and wait for a companion app to sync the time.
 * Blocks until time is written by the app, the connection is lost, or
 * the 60-second timeout expires. Returns after NimBLE is shut down.
 *
 * @param rtc_dev  PCF8563 device handle used to persist the new time.
 */
void start_ble(i2c_master_dev_handle_t rtc_dev);

#ifdef __cplusplus
}
#endif

#endif // H_BLE_DESKCLOCK_
