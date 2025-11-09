#include "battery.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>

#define TAG "battery"

float battery_read_voltage(void)
{
    // LilyGo T5 S3: Battery voltage divider is connected to GPIO14 (ADC1_CH3)
    // Voltage divider: 100K + 100K = 2:1 ratio
    // Battery max ~4.2V, ADC reads ~2.1V max
    // ADC reference is 3.3V for ESP32-S3

    adc_oneshot_unit_handle_t   adc_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id  = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12, // Full range: 0-3.3V
    };
    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &config));

    // Read ADC value
    int adc_raw;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &adc_raw));

    // Initialize calibration
    adc_cali_handle_t               adc_cali_handle = NULL;
    adc_cali_curve_fitting_config_t cali_config     = {
            .unit_id  = ADC_UNIT_2,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    esp_err_t ret =
        adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);

    int voltage_mv = 0;
    if (ret == ESP_OK) {
        ESP_ERROR_CHECK(
            adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &voltage_mv));
        adc_cali_delete_scheme_curve_fitting(adc_cali_handle);
    }

    // Clean up
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc_handle));

    // Convert to battery voltage (multiply by 2 for voltage divider)
    float battery_voltage = (voltage_mv * 2.0f) / 1000.0f;

    ESP_LOGI(TAG, "Battery: %d mV (raw: %d) -> %.2f V", voltage_mv, adc_raw,
             battery_voltage);

    return battery_voltage;
}

bool battery_is_low(float voltage)
{
    return voltage < BATTERY_LOW_THRESHOLD;
}

bool battery_is_critical(float voltage)
{
    return voltage <= BATTERY_CRITICAL_THRESHOLD;
}
