#include <stdio.h>
#include "sensors.h"
#include "monitoring.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Booting...");

    ESP_ERROR_CHECK(sensors_init());
    monitor_init();

    ESP_LOGI(TAG, "Sensors and monitoring initialized.");

    while (1) {
        sensor_reading_t reading;
        esp_err_t err = sensors_read(&reading);

        if (err == ESP_OK) {
            if (reading.valid) {
                ESP_LOGI(TAG, "Temp: %.1f C | RH: %.1f %% | Raw ADC: %d | Moisture: %.1f %%",
                         reading.temp_c, reading.rh_pct, reading.raw_adc, reading.moisture_pct);
            } else {
                ESP_LOGW(TAG, "Read invalid (dht_valid=%d soil_valid=%d)",
                         reading.dht_valid, reading.soil_valid);
            }

            plant_event_t event;
            if (monitor_evaluate(&reading, &event)) {
                ESP_LOGI(TAG, ">>> EVENT READY FOR DISPATCH: [%d] %s",
                         event.type, event.message_reason);
            }
        } else {
            ESP_LOGE(TAG, "sensors_read returned error: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}