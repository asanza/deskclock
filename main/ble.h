#ifndef H_BLE_DESKCLOCK_
#define H_BLE_DESKCLOCK_

#include <driver/i2c_master.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start BLE advertising and wait for a companion app to sync time + weather.
 * The app must write the weather characteristic first, then the time
 * characteristic (which triggers shutdown). Blocks until time is written,
 * the connection is lost, or timeout_ms elapses.
 *
 * @param rtc_dev     PCF8563 device handle used to persist the new time.
 * @param timeout_ms  Advertising timeout. Use 60000 for manual sync,
 *                    45000 for the automatic hourly sync.
 */
void start_ble(i2c_master_dev_handle_t rtc_dev, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // H_BLE_DESKCLOCK_
