#include "ble.h"
#include "clock.h"
#include "weather.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <sys/time.h>
#include <string.h>

#define TAG         "ble"
#define DEVICE_NAME "DeskClock"

/*
 * Advertising interval units: 0.625 ms.
 * 1280 ms / 0.625 ms = 2048 units — slow enough to be power-friendly while
 * still letting the phone connect within a few seconds.
 */
#define ADV_ITVL_SLOW_UNITS 2048

/*
 * Connection interval units: 1.25 ms.
 * 500 ms = 400 units, 1000 ms = 800 units.
 * Slow interval keeps average radio-on time low once connected.
 */
#define CONN_ITVL_MIN_UNITS 400
#define CONN_ITVL_MAX_UNITS 800

/* Supervision timeout units: 10 ms.  10 000 ms = 1000 units. */
#define CONN_SUPERVISION_TIMEOUT 1000

/* Custom DeskClock service + characteristics
 *   Service:  12345678-1234-1234-1234-123456789001
 *   Weather:  12345678-1234-1234-1234-123456789002  (Write)
 *   Time:     12345678-1234-1234-1234-123456789003  (Write)
 *   Uptime:   12345678-1234-1234-1234-123456789005  (Read)
 */
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x01, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t chr_weather_uuid =
    BLE_UUID128_INIT(0x02, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t chr_time_uuid =
    BLE_UUID128_INIT(0x03, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t chr_uptime_uuid =
    BLE_UUID128_INIT(0x05, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

/* Standard Battery Service 0x180F / Battery Level 0x2A19 */
static const ble_uuid16_t batt_svc_uuid = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t batt_lvl_uuid = BLE_UUID16_INIT(0x2A19);

static i2c_master_dev_handle_t s_rtc_dev;
static uint8_t                 s_own_addr_type;
static volatile bool           s_connected   = false;
static uint16_t                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t                 s_battery_level = 100;
static uint16_t                s_batt_lvl_handle = 0;

static int gap_event(struct ble_gap_event *event, void *arg);

/* ---------- GATT callbacks ---------- */

static int chr_write_weather(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(weather_data_t)) {
        ESP_LOGW(TAG, "Weather write: bad length %d (expected %d)",
                 OS_MBUF_PKTLEN(ctxt->om), (int)sizeof(weather_data_t));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    weather_data_t data;
    ble_hs_mbuf_to_flat(ctxt->om, &data, sizeof(data), NULL);
    weather_save(&data);
    return 0;
}

static int chr_write_time(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(uint32_t)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint32_t utc_secs = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &utc_secs, sizeof(utc_secs), NULL);

    struct timeval tv = { .tv_sec = (time_t)utc_secs, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    struct tm tm_utc;
    gmtime_r(&tv.tv_sec, &tm_utc);
    clock_set_time(s_rtc_dev, &tm_utc);

    ESP_LOGI(TAG, "Time synced: %lu UTC", (unsigned long)utc_secs);
    return 0;
}

static int chr_read_uptime(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint32_t uptime_secs = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    os_mbuf_append(ctxt->om, &uptime_secs, sizeof(uptime_secs));
    return 0;
}

static int chr_read_battery(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    os_mbuf_append(ctxt->om, &s_battery_level, sizeof(s_battery_level));
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &chr_weather_uuid.u,
                .access_cb = chr_write_weather,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid      = &chr_time_uuid.u,
                .access_cb = chr_write_time,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid      = &chr_uptime_uuid.u,
                .access_cb = chr_read_uptime,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    /* Standard Battery Service — exposes battery level to phone OS */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &batt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &batt_lvl_uuid.u,
                .access_cb  = chr_read_battery,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_batt_lvl_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---------- GAP / advertising ---------- */

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)DEVICE_NAME;
    fields.name_len         = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = ADV_ITVL_SLOW_UNITS;
    adv_params.itvl_max  = ADV_ITVL_SLOW_UNITS + 32; /* +20 ms jitter */

    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                      &adv_params, gap_event, NULL);
    ESP_LOGI(TAG, "Advertising");
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected   = true;
            ESP_LOGI(TAG, "Connected handle=%d", s_conn_handle);

            /* Ask the phone for a slow connection interval to save power. */
            struct ble_gap_upd_params params = {
                .itvl_min            = CONN_ITVL_MIN_UNITS,
                .itvl_max            = CONN_ITVL_MAX_UNITS,
                .latency             = 0,
                .supervision_timeout = CONN_SUPERVISION_TIMEOUT,
                .min_ce_len          = 0,
                .max_ce_len          = 0,
            };
            ble_gap_update_params(s_conn_handle, &params);
        } else {
            ESP_LOGW(TAG, "Connect failed status=%d", event->connect.status);
            advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected reason=%d", event->disconnect.reason);
        s_connected   = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection params updated");
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- public ---------- */

void ble_init(i2c_master_dev_handle_t rtc_dev)
{
    s_rtc_dev = rtc_dev;

    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);
    ble_svc_gap_device_name_set(DEVICE_NAME);

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE initialised");
}

bool ble_is_connected(void)
{
    return s_connected;
}

void ble_update_battery(uint8_t percent)
{
    s_battery_level = percent;

    if (!s_connected || s_batt_lvl_handle == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(&percent, sizeof(percent));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_batt_lvl_handle, om);
    }
}
