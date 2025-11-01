#include "ble_time_sync.h"
#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/util/util.h>

#define TAG "BLE_TIME_SYNC"

// Current Time Service UUIDs
#define CTS_SERVICE_UUID        0x1805
#define CTS_CURRENT_TIME_UUID   0x2A2B

// BLE scan parameters
#define BLE_SCAN_DURATION_MS    10000  // 10 seconds

// State variables
static bool scan_complete = false;
static bool time_received = false;
static struct tm received_time;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t cts_chr_val_handle = 0;

static void parse_current_time(const uint8_t *data, uint16_t len, struct tm *time_out)
{
    if (len < 10) {
        ESP_LOGE(TAG, "Invalid Current Time data length: %d", len);
        return;
    }

    // Parse Current Time characteristic (10 bytes)
    uint16_t year = data[0] | (data[1] << 8);
    uint8_t month = data[2];
    uint8_t day = data[3];
    uint8_t hour = data[4];
    uint8_t minute = data[5];
    uint8_t second = data[6];
    uint8_t day_of_week = data[7];

    ESP_LOGI(TAG, "Received time: %04d-%02d-%02d %02d:%02d:%02d (DoW: %d)",
             year, month, day, hour, minute, second, day_of_week);

    // Convert to tm structure
    memset(time_out, 0, sizeof(struct tm));
    time_out->tm_year = year - 1900;  // tm_year is years since 1900
    time_out->tm_mon = month - 1;     // tm_mon is 0-11
    time_out->tm_mday = day;
    time_out->tm_hour = hour;
    time_out->tm_min = minute;
    time_out->tm_sec = second;
    time_out->tm_wday = (day_of_week == 7) ? 0 : day_of_week;  // Convert Sunday from 7 to 0
    time_out->tm_isdst = -1;
}

static int ble_gatt_chr_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Read characteristic value, len: %d", OS_MBUF_PKTLEN(attr->om));
        
        // Parse the current time data
        parse_current_time(attr->om->om_data, OS_MBUF_PKTLEN(attr->om), &received_time);
        time_received = true;
        
        // Disconnect after reading
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        ESP_LOGE(TAG, "Failed to read characteristic: %d", error->status);
    }
    
    return 0;
}

static int ble_gatt_chr_discover_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                      const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0) {
        // Check if this is the Current Time characteristic
        if (ble_uuid_u16(&chr->uuid.u) == CTS_CURRENT_TIME_UUID) {
            ESP_LOGI(TAG, "Found Current Time characteristic");
            cts_chr_val_handle = chr->val_handle;
            
            // Read the characteristic
            int rc = ble_gattc_read(conn_handle, cts_chr_val_handle, ble_gatt_chr_read_cb, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to read characteristic: %d", rc);
            }
        }
    }
    
    return 0;
}

static int ble_gatt_svc_discover_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                      const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0) {
        // Check if this is the Current Time Service
        if (ble_uuid_u16(&service->uuid.u) == CTS_SERVICE_UUID) {
            ESP_LOGI(TAG, "Found Current Time Service");
            
            // Discover characteristics
            ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                     ble_gatt_chr_discover_cb, NULL);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Service discovery complete");
    }
    
    return 0;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            // Check if device advertises Current Time Service
            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            if (rc == 0) {
                // Check for CTS in service UUIDs
                for (int i = 0; i < fields.num_uuids16; i++) {
                    if (ble_uuid_u16(&fields.uuids16[i].u) == CTS_SERVICE_UUID) {
                        ESP_LOGI(TAG, "Found device with Current Time Service");
                        
                        // Stop scanning and connect
                        ble_gap_disc_cancel();
                        
                        struct ble_gap_conn_params conn_params = {0};
                        conn_params.scan_itvl = 0x0010;
                        conn_params.scan_window = 0x0010;
                        conn_params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
                        conn_params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
                        conn_params.latency = BLE_GAP_INITIAL_CONN_LATENCY;
                        conn_params.supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT;
                        conn_params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
                        conn_params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
                        
                        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 30000,
                                            &conn_params, ble_gap_event_cb, NULL);
                        if (rc != 0) {
                            ESP_LOGE(TAG, "Failed to connect: %d", rc);
                        }
                        
                        return 0;
                    }
                }
            }
            break;
        }
        
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "Scan complete");
            scan_complete = true;
            break;
            
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected to device");
                conn_handle = event->connect.conn_handle;
                
                // Start service discovery
                ble_gattc_disc_all_svcs(event->connect.conn_handle, ble_gatt_svc_discover_cb, NULL);
            } else {
                ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
                scan_complete = true;
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected from device");
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            scan_complete = true;
            break;
            
        default:
            break;
    }
    
    return 0;
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync_cb(void)
{
    // Start scanning
    struct ble_gap_disc_params disc_params = {0};
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;
    
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_SCAN_DURATION_MS, &disc_params,
                          ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
        scan_complete = true;
    } else {
        ESP_LOGI(TAG, "Started BLE scan for Current Time Service");
    }
}

static void ble_on_reset_cb(int reason)
{
    ESP_LOGE(TAG, "BLE reset, reason: %d", reason);
}

bool ble_sync_time(struct tm *time_out)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Starting BLE time synchronization with NimBLE");
    
    // Initialize NVS (required for BLE)
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Reset state
    scan_complete = false;
    time_received = false;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    cts_chr_val_handle = 0;
    memset(&received_time, 0, sizeof(received_time));
    
    // Initialize NimBLE
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Set callbacks
    ble_hs_cfg.sync_cb = ble_on_sync_cb;
    ble_hs_cfg.reset_cb = ble_on_reset_cb;
    
    // Start NimBLE host task
    nimble_port_freertos_init(ble_host_task);
    
    // Wait for scan to complete or timeout (max 30 seconds)
    int timeout = 3000 / 100;  // 30 seconds in 100ms increments
    while (!scan_complete && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }
    
    bool success = false;
    if (time_received) {
        ESP_LOGI(TAG, "Successfully received time via BLE");
        memcpy(time_out, &received_time, sizeof(struct tm));
        success = true;
    } else if (timeout <= 0) {
        ESP_LOGW(TAG, "BLE time sync timeout");
    } else {
        ESP_LOGW(TAG, "No device with Current Time Service found");
    }
    
    // Cleanup
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait for disconnect
    }
    
    ret = nimble_port_stop();
    if (ret == 0) {
        nimble_port_deinit();
    }
    
    ESP_LOGI(TAG, "BLE sync complete, success: %d", success);
    return success;
}
