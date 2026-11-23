#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* DHT11 DATA pin: ESP32 D13 (GPIO13) */
#define SENSOR_DHT_GPIO GPIO_NUM_13

/* Potentiometer ADC configuration */
#define SENSOR_SOIL_ADC_UNIT ADC_UNIT_1
#define SENSOR_SOIL_ADC_CHAN ADC_CHANNEL_6

/* Soil moisture calibration defaults */
#ifndef CONFIG_SOIL_RAW_DRY
#define CONFIG_SOIL_RAW_DRY 2800
#endif

#ifndef CONFIG_SOIL_RAW_WET
#define CONFIG_SOIL_RAW_WET 1200
#endif

typedef struct {

    float temp_c;
    float rh_pct;

    int raw_adc;
    float moisture_pct;

    bool valid;
    bool dht_valid;
    bool soil_valid;

    time_t timestamp;

} sensor_reading_t;

/* Sensor functions */

esp_err_t sensors_init(void);

esp_err_t sensors_read(sensor_reading_t *out);

void sensors_calibration_mode(void);