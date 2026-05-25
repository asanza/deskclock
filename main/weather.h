#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define WEATHER_LOCATION_LEN 20
#define WEATHER_ALERT_LEN    48

/* BLE payload is 76 bytes total — phone must request MTU >= 80. */
#define WEATHER_BLE_SIZE (8 + WEATHER_LOCATION_LEN + WEATHER_ALERT_LEN)

typedef enum {
    WEATHER_CLEAR         = 0,
    WEATHER_PARTLY_CLOUDY = 1,
    WEATHER_OVERCAST      = 2,
    WEATHER_FOG           = 3,
    WEATHER_DRIZZLE       = 4,
    WEATHER_RAIN          = 5,
    WEATHER_SNOW          = 6,
    WEATHER_STORM         = 7,
} weather_condition_t;

typedef enum {
    ALERT_NONE     = 0,
    ALERT_MINOR    = 1,
    ALERT_MODERATE = 2,
    ALERT_SEVERE   = 3,
    ALERT_EXTREME  = 4,
} weather_alert_level_t;

typedef struct {
    int8_t   temperature;                     /* Celsius                          */
    uint8_t  condition;                       /* weather_condition_t               */
    uint8_t  rain_prob_1h;                    /* 0-100 %                           */
    uint8_t  alert_level;                     /* weather_alert_level_t             */
    uint16_t sunrise_min;                     /* minutes since midnight, local TZ  */
    uint16_t sunset_min;                      /* minutes since midnight, local TZ  */
    char     location[WEATHER_LOCATION_LEN];  /* null-terminated city name         */
    char     alert_text[WEATHER_ALERT_LEN];   /* null-terminated, empty=none       */
} __attribute__((packed)) weather_data_t;

/* Load last saved weather from NVS. Returns false if nothing stored yet. */
bool weather_load(weather_data_t *out);

/* Persist weather data to NVS (also records the current timestamp). */
void weather_save(const weather_data_t *data);

/* Seconds elapsed since the last weather_save(). Returns UINT32_MAX if never saved. */
uint32_t weather_age_seconds(void);

/* Format a single display line, e.g. "Munich 18C Rain 85%"
 * or "! STORM WARNING" if alert_level >= ALERT_SEVERE. */
void weather_format_display(const weather_data_t *data, char *out, size_t len);

#endif // WEATHER_H
