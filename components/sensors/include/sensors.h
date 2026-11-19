#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/* Hardware pin mapping (matches ROLE.txt) */
#define SENSOR_DHT_GPIO        GPIO_NUM_13
#define SENSOR_SOIL_ADC_UNIT   ADC_UNIT_1
#define SENSOR_SOIL_ADC_CHAN   ADC_CHANNEL_6   /* GPIO34 = ADC1_CH6 */

/* Soil calibration defaults (override via config.h or NVS later) */
#ifndef CONFIG_SOIL_RAW_DRY
#define CONFIG_SOIL_RAW_DRY    2800
#endif
#ifndef CONFIG_SOIL_RAW_WET
#define CONFIG_SOIL_RAW_WET    1200
#endif

/**
 * @brief One complete sensor reading.
 *
 * `valid` is false if ANY sensor failed validation (DHT22 error, ADC out of
 * range, stale timestamp). A false `valid` must NEVER be reported as NORMAL
 * by the monitoring layer.
 */
typedef struct {
    float    temp_c;        /* Celsius, validated -40..80 */
    float    rh_pct;        /* Relative humidity %, validated 0..100 */
    int      raw_adc;       /* Raw ADC value, 0..4095 */
    float    moisture_pct;  /* Calibrated 0..100 % */
    bool     valid;         /* true only if all sensors passed validation */
    bool     dht_valid;     /* DHT22-specific validity */
    bool     soil_valid;    /* ADC-specific validity */
    time_t   timestamp;     /* Unix time at read completion */
} sensor_reading_t;

/**
 * @brief Initialize DHT22 GPIO and ADC oneshot unit.
 * @return ESP_OK on success.
 */
esp_err_t sensors_init(void);

/**
 * @brief Read and validate both sensors once.
 *
 * Calls dht_read_float_data() and adc_oneshot_read(), applies validation
 * rules (range checks, disconnected-sensor detection), and fills `out`.
 *
 * @param out  Must not be NULL.
 * @return ESP_OK even if a sensor fails; check `out->valid` and the
 *         per-sensor flags. ESP_ERR_INVALID_ARG only on NULL pointer.
 */
esp_err_t sensors_read(sensor_reading_t *out);

/**
 * @brief Blocking calibration log mode.
 *
 * Prints averaged raw ADC values every 2 seconds indefinitely. Use this to
 * record raw_dry (air / bone-dry soil) and raw_wet (water-saturated soil)
 * for YOUR specific sensor. Call from app_main() only when calibrating.
 */
void sensors_calibration_mode(void);