#include "ble.h"
#include "clock.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <sys/time.h>
#include <string.h>

#define TAG            "ble"
#define DEVICE_NAME    "DeskClock"
#define BLE_TIMEOUT_MS 60000

/*
 * Custom DeskClock GATT service
 *
 * Service UUID:          12345678-1234-1234-1234-123456789001
 * Time characteristic:   12345678-1234-1234-1234-123456789002
 *
 * The companion app writes the current UTC Unix timestamp as a
 * 4-byte little-endian uint32 to the time characteristic.
 */

/* BLE_UUID128_INIT expects bytes in little-endian order. */
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x01, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static const ble_uuid128_t chr_time_uuid =
    BLE_UUID128_INIT(0x02, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12,
                     0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static i2c_master_dev_handle_t s_rtc_dev;
static SemaphoreHandle_t       s_done_sem;
static TimerHandle_t           s_timeout_timer;
static uint8_t                 s_own_addr_type;

static int gap_event(struct ble_gap_event *event, void *arg);

/* ---------- helpers ---------- */

static void signal_done(void)
{
    xSemaphoreGive(s_done_sem);
}

static void timeout_cb(TimerHandle_t t)
{
    ESP_LOGI(TAG, "BLE timeout, going back to sleep");
    signal_done();
}

/* ---------- GATT ---------- */

static int chr_write_time(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(uint32_t)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint32_t utc_secs = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &utc_secs, sizeof(utc_secs), NULL);

    /* Update ESP32 internal RTC */
    struct timeval tv = { .tv_sec = (time_t)utc_secs, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* Persist to PCF8563 (stores UTC) */
    struct tm tm_utc;
    gmtime_r(&tv.tv_sec, &tm_utc);
    clock_set_time(s_rtc_dev, &tm_utc);

    ESP_LOGI(TAG, "Time synced: %lu UTC", (unsigned long)utc_secs);
    signal_done();
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &chr_time_uuid.u,
                .access_cb = chr_write_time,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---------- GAP ---------- */

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags             = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name              = (uint8_t *)DEVICE_NAME;
    fields.name_len          = strlen(DEVICE_NAME);
    fields.name_is_complete  = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                      &adv_params, gap_event, NULL);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected");
            xTimerStop(s_timeout_timer, 0);
        } else {
            advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected");
        signal_done();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
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
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------- public ---------- */

void start_ble(i2c_master_dev_handle_t rtc_dev)
{
    s_rtc_dev  = rtc_dev;
    s_done_sem = xSemaphoreCreateBinary();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);
    ble_svc_gap_device_name_set(DEVICE_NAME);

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    s_timeout_timer = xTimerCreate("ble_to", pdMS_TO_TICKS(BLE_TIMEOUT_MS),
                                   pdFALSE, NULL, timeout_cb);
    xTimerStart(s_timeout_timer, 0);

    nimble_port_freertos_init(ble_host_task);

    /* Block until time is written, device disconnects, or timeout fires */
    xSemaphoreTake(s_done_sem, portMAX_DELAY);

    vTaskDelay(pdMS_TO_TICKS(300)); /* let pending BLE ops finish */
    nimble_port_stop();
    vTaskDelay(pdMS_TO_TICKS(100)); /* let ble_host_task exit */

    xTimerDelete(s_timeout_timer, portMAX_DELAY);
    vSemaphoreDelete(s_done_sem);

    ESP_LOGI(TAG, "BLE session ended");
}
