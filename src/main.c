
#include <stdio.h>

#include "sensors.h"
#include "monitoring.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Booting Smart Plant Monitoring System...");

    /* Initialize sensors */
    ESP_ERROR_CHECK(sensors_init());

    /* Initialize monitoring state machine */
    monitor_init();

    ESP_LOGI(TAG, "Sensors and monitoring initialized.");
    ESP_LOGI(TAG, "DHT11: GPIO13");
    ESP_LOGI(TAG, "Potentiometer: GPIO34");
    ESP_LOGI(TAG, "Moisture input: SIMULATED");

    /*
     * Allow the DHT11 time to stabilize after startup
     * before taking the first reading.
     */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {

        sensor_reading_t reading;

        esp_err_t err = sensors_read(&reading);

        if (err == ESP_OK) {

            if (reading.valid) {

                ESP_LOGI(TAG,
                         "DHT11: %.1f C | RH: %.1f %% | ADC: %d | Simulated Moisture: %.1f %%",
                         reading.temp_c,
                         reading.rh_pct,
                         reading.raw_adc,
                         reading.moisture_pct);
            }
            else {

                ESP_LOGW(TAG,
                         "Invalid reading (DHT11=%d | Potentiometer=%d)",
                         reading.dht_valid,
                         reading.soil_valid);
            }

            /* Evaluate sensor data using existing monitoring logic */
            plant_event_t event;

            if (monitor_evaluate(&reading, &event)) {

                ESP_LOGI(TAG,
                         ">>> EVENT READY FOR DISPATCH: [%d] %s",
                         event.type,
                         event.message_reason);
            }
        }
        else {

            ESP_LOGE(TAG,
                     "sensors_read returned error: %s",
                     esp_err_to_name(err));
        }

        /* Read sensors every 5 seconds */
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
