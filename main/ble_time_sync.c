#include "ble_time_sync.h"
#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/util/util.h>


#define TAG "BLE_CLOCK"

// Discovery Service UUID (used for pairing advertising)
static const ble_uuid128_t DISCOVERY_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 
                     0x00, 0x10, 0x00, 0x00, 0x55, 0xfe, 0x00, 0x00);

// GATT Service UUIDs (phone acts as server) - marked as potentially unused for now
__attribute__((unused)) static const ble_uuid128_t CURRENT_TIME_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x05, 0x18, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t CURRENT_DATE_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x06, 0x18, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t WEATHER_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x1a, 0x18, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t EVENTS_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x07, 0x18, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t NEWS_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x08, 0x18, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t CUSTOM_TEXT_SERVICE_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x09, 0x18, 0x00, 0x00);

// Characteristic UUIDs - marked as potentially unused for now
__attribute__((unused)) static const ble_uuid128_t CURRENT_TIME_CHAR_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x2b, 0x2a, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t CURRENT_DATE_CHAR_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x08, 0x2a, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t WEATHER_CHAR_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x0d, 0x2a, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t EVENTS_CHAR_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x09, 0x2a, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t NEWS_CHAR_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x0a, 0x2a, 0x00, 0x00);

__attribute__((unused)) static const ble_uuid128_t CUSTOM_TEXT_CHAR_UUID = 
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x00, 0x00, 0x0b, 0x2a, 0x00, 0x00);

// State variables
static bool ble_initialized = false;
static bool pairing_complete = false;
static bool advertising_active = false;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t bonded_peer_addr;
static bool has_bonded_peer = false;

// NimBLE host task
static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Security and bonding callbacks
static void ble_on_sync_cb(void);
static void ble_on_reset_cb(int reason);

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);

static void
ble_store_config_init(void)
{
    ble_hs_cfg.reset_cb = ble_on_reset_cb;
    ble_hs_cfg.sync_cb = ble_on_sync_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    
    // Enable bonding
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;  // Secure connections
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

// GAP event handler for advertising/pairing
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnect; reason=%d", event->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertise complete");
        advertising_active = false;
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change event; status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "Connection encrypted; bonded=%d", desc.sec_state.bonded);
                if (desc.sec_state.bonded) {
                    pairing_complete = true;
                    has_bonded_peer = true;
                    bonded_peer_addr = desc.peer_id_addr;
                    ESP_LOGI(TAG, "Pairing complete!");
                }
            }
        }
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG, "Passkey action event");
        // With NO_IO capability, this shouldn't occur
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        ESP_LOGI(TAG, "Notification received");
        break;

    default:
        break;
    }

    return 0;
}

static void ble_on_sync_cb(void)
{
    ESP_LOGI(TAG, "BLE stack synchronized");
    
    // Make sure we have a public address
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address; rc=%d", rc);
        return;
    }
    
    // Set device address type
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address; rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Address configured, type: %d", own_addr_type);
    }
    
    // Check if we have any bonded peers from NVS
    int num_peers;
    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &num_peers);
    if (rc == 0 && num_peers > 0) {
        ESP_LOGI(TAG, "Found %d bonded peer(s) in NVS", num_peers);
        
        // Load the first bonded peer address
        struct ble_store_key_sec key_sec;
        struct ble_store_value_sec value_sec;
        
        memset(&key_sec, 0, sizeof(key_sec));
        key_sec.peer_addr = bonded_peer_addr;
        
        rc = ble_store_util_bonded_peers(&bonded_peer_addr, &num_peers, 1);
        if (rc == 0 && num_peers > 0) {
            has_bonded_peer = true;
            ESP_LOGI(TAG, "Loaded bonded peer address from NVS");
        }
    } else {
        ESP_LOGI(TAG, "No bonded peers found in NVS");
        has_bonded_peer = false;
    }
}

static void ble_on_reset_cb(int reason)
{
    ESP_LOGE(TAG, "BLE reset, reason: %d", reason);
}

// Public API implementation

bool ble_init(void)
{
    if (ble_initialized) {
        ESP_LOGW(TAG, "BLE already initialized");
        return true;
    }

    ESP_LOGI(TAG, "Initializing BLE stack");
    
    // Initialize NVS (required for BLE bonding storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Configure BLE stack for bonding BEFORE nimble_port_init
    ble_store_config_init();
    
    // Initialize NimBLE
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Initialize data path (required for advertising)
    ble_hs_cfg.sync_cb = ble_on_sync_cb;
    ble_hs_cfg.reset_cb = ble_on_reset_cb;
    
    // Start NimBLE host task
    nimble_port_freertos_init(ble_host_task);
    
    // Wait for stack to sync
    int timeout = 50; // 5 seconds
    while (!ble_hs_synced() && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }
    
    if (timeout <= 0) {
        ESP_LOGE(TAG, "BLE stack sync timeout");
        return false;
    }
    
    ble_initialized = true;
    ESP_LOGI(TAG, "BLE initialized successfully");
    return true;
}

bool ble_start_pairing_advertising(const char *device_name, uint32_t timeout_ms)
{
    if (!ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return false;
    }

    if (timeout_ms == 0) {
        timeout_ms = 60000; // Default 60 seconds
    }

    ESP_LOGI(TAG, "Starting pairing advertisement (timeout: %lu ms)", timeout_ms);
    
    // Make sure no advertising is active
    ble_gap_adv_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Use the simpler fields API but with minimal data
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    
    // Set flags
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertisement data; rc=%d", rc);
        return false;
    }
    
    // Put everything in scan response: name and UUID
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    
    rsp_fields.name = (uint8_t *)device_name;
    rsp_fields.name_len = strlen(device_name);
    rsp_fields.name_is_complete = 1;
    
    rsp_fields.uuids128 = (ble_uuid128_t *)&DISCOVERY_SERVICE_UUID;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set scan response data; rc=%d", rc);
        return false;
    }
    
    ESP_LOGI(TAG, "Advertisement configured successfully");
    
    // Start advertising
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, timeout_ms,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising; rc=%d", rc);
        return false;
    }
    
    advertising_active = true;
    pairing_complete = false;
    
    ESP_LOGI(TAG, "Advertising started, waiting for pairing...");
    
    // Wait for pairing or timeout
    uint32_t elapsed = 0;
    while (!pairing_complete && elapsed < timeout_ms && advertising_active) {
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;
    }
    
    // Stop advertising if still active
    if (advertising_active) {
        ble_gap_adv_stop();
        advertising_active = false;
    }
    
    if (pairing_complete) {
        ESP_LOGI(TAG, "Pairing successful!");
        return true;
    } else {
        ESP_LOGW(TAG, "Pairing timeout or failed");
        return false;
    }
}

void ble_stop_advertising(void)
{
    if (advertising_active) {
        ble_gap_adv_stop();
        advertising_active = false;
        ESP_LOGI(TAG, "Advertising stopped");
    }
}

// GATT client read callback context
typedef struct {
    char *buffer;
    size_t buffer_size;
    bool completed;
} gatt_read_ctx_t;

__attribute__((unused))
static int gatt_read_char_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    gatt_read_ctx_t *ctx = (gatt_read_ctx_t *)arg;
    
    if (error->status == 0 && attr != NULL) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > 0 && len < ctx->buffer_size) {
            os_mbuf_copydata(attr->om, 0, len, ctx->buffer);
            ctx->buffer[len] = '\0'; // Null-terminate UTF-8 string
            ESP_LOGI(TAG, "Read characteristic: %s", ctx->buffer);
        } else {
            ESP_LOGW(TAG, "Data too large or empty: %d bytes", len);
        }
    } else {
        ESP_LOGW(TAG, "Read failed; status=%d", error->status);
    }
    
    ctx->completed = true;
    return 0;
}

__attribute__((unused))
static bool read_characteristic(uint16_t conn_handle, const ble_uuid_t *svc_uuid,
                                 const ble_uuid_t *chr_uuid, char *buffer, size_t buffer_size)
{
    // Discover service
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, svc_uuid, NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Service not found; rc=%d", rc);
        return false;
    }
    
    // For simplicity, we'll use a blocking approach with characteristic discovery
    // In production, you'd want to implement proper async discovery
    
    // Try to read the characteristic
    gatt_read_ctx_t ctx = {
        .buffer = buffer,
        .buffer_size = buffer_size,
        .completed = false
    };
    
    // This is simplified - proper implementation would discover characteristic handle first
    // For now, we'll log that this needs to be properly implemented
    ESP_LOGW(TAG, "Characteristic read not fully implemented - needs proper discovery");
    
    return false;
}

bool ble_connect_and_read_data(ble_clock_data_t *data_out)
{
    if (!ble_initialized) {
        ESP_LOGE(TAG, "BLE not initialized");
        return false;
    }
    
    if (!has_bonded_peer) {
        ESP_LOGW(TAG, "No bonded peer available");
        return false;
    }
    
    ESP_LOGI(TAG, "Connecting to bonded phone...");
    
    // Connect to bonded peer
    struct ble_gap_conn_params conn_params;
    conn_params.scan_itvl = 0x0010;
    conn_params.scan_window = 0x0010;
    conn_params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    conn_params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    conn_params.latency = BLE_GAP_INITIAL_CONN_LATENCY;
    conn_params.supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT;
    conn_params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
    conn_params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
    
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &bonded_peer_addr, 30000,
                             &conn_params, ble_gap_event_handler, NULL);
    
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initiate connection; rc=%d", rc);
        return false;
    }
    
    // Wait for connection
    int timeout = 100; // 10 seconds
    while (conn_handle == BLE_HS_CONN_HANDLE_NONE && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
    }
    
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGE(TAG, "Connection timeout");
        return false;
    }
    
    ESP_LOGI(TAG, "Connected, reading data...");
    
    // Read all characteristics
    memset(data_out, 0, sizeof(ble_clock_data_t));
    
    // Note: Full GATT client implementation with service/characteristic discovery
    // needs to be completed. This is a placeholder that shows the structure.
    
    ESP_LOGW(TAG, "GATT client read implementation incomplete");
    
    // Disconnect
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    
    return false; // Until fully implemented
}

void ble_deinit(void)
{
    if (!ble_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing BLE");
    
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (advertising_active) {
        ble_gap_adv_stop();
    }
    
    int ret = nimble_port_stop();
    if (ret == 0) {
        nimble_port_deinit();
    }
    
    ble_initialized = false;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    advertising_active = false;
    pairing_complete = false;
}

bool ble_is_bonded(void)
{
    return has_bonded_peer;
}
