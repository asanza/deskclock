#include "weather.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG          "weather"
#define NVS_NS       "deskclock"
#define NVS_KEY      "weather"
#define NVS_KEY_TS   "wx_ts"

bool weather_load(weather_data_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = sizeof(weather_data_t);
    esp_err_t ret = nvs_get_blob(h, NVS_KEY, out, &len);
    nvs_close(h);
    return (ret == ESP_OK && len == sizeof(weather_data_t));
}

void weather_save(const weather_data_t *data)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write");
        return;
    }
    nvs_set_blob(h, NVS_KEY, data, sizeof(weather_data_t));

    time_t now;
    time(&now);
    nvs_set_u32(h, NVS_KEY_TS, (uint32_t)now);

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Weather saved: %s %d°C cond=%d rain=%d%% sr=%d ss=%d alert=%d",
             data->location, data->temperature, data->condition,
             data->rain_prob_1h, data->sunrise_min, data->sunset_min,
             data->alert_level);
}

uint32_t weather_age_seconds(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return UINT32_MAX;

    uint32_t saved_ts = 0;
    esp_err_t ret = nvs_get_u32(h, NVS_KEY_TS, &saved_ts);
    nvs_close(h);

    if (ret != ESP_OK || saved_ts == 0) return UINT32_MAX;

    time_t now;
    time(&now);
    if ((time_t)saved_ts > now) return 0; /* clock jumped back */
    return (uint32_t)(now - (time_t)saved_ts);
}

static const char *condition_str(uint8_t c)
{
    switch ((weather_condition_t)c) {
    case WEATHER_CLEAR:          return "Clear";
    case WEATHER_PARTLY_CLOUDY:  return "Pt cloudy";
    case WEATHER_OVERCAST:       return "Overcast";
    case WEATHER_FOG:            return "Fog";
    case WEATHER_DRIZZLE:        return "Drizzle";
    case WEATHER_RAIN:           return "Rain";
    case WEATHER_SNOW:           return "Snow";
    case WEATHER_STORM:          return "Storm";
    default:                     return "";
    }
}

void weather_format_display(const weather_data_t *data, char *out, size_t len)
{
    if (data->alert_level >= ALERT_SEVERE) {
        const char *txt = data->alert_text[0] ? data->alert_text : "WEATHER ALERT";
        snprintf(out, len, "! %s", txt);
        return;
    }

    /* Base: "Munich 18C Partly cloudy" */
    int used = snprintf(out, len, "%s %d\xc2\xb0""C %s",
                        data->location,
                        (int)data->temperature,
                        condition_str(data->condition));

    /* Append rain probability if notable */
    if (data->rain_prob_1h >= 50 && used < (int)len - 8) {
        snprintf(out + used, len - used, " %d%%", data->rain_prob_1h);
    }
}
