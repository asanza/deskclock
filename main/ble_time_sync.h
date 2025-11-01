#ifndef BLE_TIME_SYNC_H
#define BLE_TIME_SYNC_H

#include <time.h>
#include <stdbool.h>

/**
 * @brief Synchronize time using BLE Current Time Service (NimBLE)
 * 
 * Scans for nearby BLE devices advertising the Current Time Service (0x1805),
 * connects to the first available device, reads the Current Time characteristic
 * (0x2A2B), and returns the time in the provided tm structure.
 * 
 * @param time_out Pointer to tm structure to store the retrieved time
 * @return true if time was successfully retrieved, false otherwise
 */
bool ble_sync_time(struct tm *time_out);

#endif // BLE_TIME_SYNC_H
