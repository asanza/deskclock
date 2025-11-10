#ifndef BLE_TIME_SYNC_H
#define BLE_TIME_SYNC_H

#include <time.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Clock data received from phone's GATT server
 */
typedef struct {
    char current_time[64];    // UTF-8 string: Current time
    char current_date[64];    // UTF-8 string: Current date
    char weather[256];        // UTF-8 string: Weather information
    char events[512];         // UTF-8 string: Calendar events
    char news[512];           // UTF-8 string: News headlines
    char custom_text[256];    // UTF-8 string: Custom text
    bool valid;               // True if data was successfully retrieved
} ble_clock_data_t;

/**
 * @brief Initialize BLE stack
 * 
 * Must be called before any other BLE functions.
 * 
 * @return true if initialization was successful, false otherwise
 */
bool ble_init(void);

/**
 * @brief Start advertising for pairing with discovery service
 * 
 * Advertises the device with the discovery service UUID (0000FE55-0000-1000-8000-00805f9b34fb)
 * to be discovered by the phone app. Waits for pairing or timeout (60 seconds).
 * 
 * @param device_name User-friendly device name (e.g., "DeskClock")
 * @param timeout_ms Timeout in milliseconds (0 = no timeout, default: 60000)
 * @return true if successfully paired, false on timeout or error
 */
bool ble_start_pairing_advertising(const char *device_name, uint32_t timeout_ms);

/**
 * @brief Connect to phone's GATT server and read all data
 * 
 * After successful pairing, connects to the phone as a GATT client and reads
 * all available data services (time, date, weather, events, news, custom text).
 * Requires an encrypted, bonded connection.
 * 
 * @param data_out Pointer to structure to store retrieved data
 * @return true if data was successfully retrieved, false otherwise
 */
bool ble_connect_and_read_data(ble_clock_data_t *data_out);

/**
 * @brief Stop BLE advertising
 * 
 * Stops any ongoing advertising.
 */
void ble_stop_advertising(void);

/**
 * @brief Deinitialize BLE stack
 * 
 * Cleanup BLE resources. Should be called when BLE is no longer needed.
 */
void ble_deinit(void);

/**
 * @brief Check if device is currently paired/bonded
 * 
 * @return true if bonded with a phone, false otherwise
 */
bool ble_is_bonded(void);

/**
 * @brief Clear all stored bonding information (bonds, keys, CCCDs)
 *
 * Removes all security-related entries from the NimBLE persistent store.
 * Use this when you explicitly want to force the device to forget the
 * previously paired phone and allow a fresh pairing. After calling this,
 * call ble_start_pairing_advertising() again to initiate a new pairing.
 *
 * @return true if deletion calls were issued (even if entries didn't exist), false if BLE not initialized
 */
bool ble_clear_bonds(void);

#endif // BLE_TIME_SYNC_H
