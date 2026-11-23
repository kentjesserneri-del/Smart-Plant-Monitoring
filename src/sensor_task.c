#include "sensor_task.h"

#include "sensors.h"
#include "monitoring.h"
#include "mqtt_mgr.h"
#include "discord_mgr.h"
#include "email_mgr.h"
#include "callmebot_mgr.h"
#include "mastodon_mgr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensor_task";

void sensor_monitor_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Sensor monitoring task started on Core 1.");

    ESP_LOGI(TAG, "DHT11: GPIO13");
    ESP_LOGI(TAG, "Potentiometer: GPIO34");
    ESP_LOGI(TAG, "Moisture input: SIMULATED");

    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1)
    {
        sensor_reading_t reading;

        esp_err_t err = sensors_read(&reading);

        if (err == ESP_OK)
        {
            if (reading.valid)
            {
                ESP_LOGI(
                    TAG,
                    "DHT11: %.1f C | RH: %.1f %% | ADC: %d | Simulated Moisture: %.1f%%",
                    reading.temp_c,
                    reading.rh_pct,
                    reading.raw_adc,
                    reading.moisture_pct
                );

                if (!mqtt_mgr_publish_reading(&reading))
                {
                    ESP_LOGW(TAG, "MQTT reading queue unavailable or full.");
                }
            }
            else
            {
                ESP_LOGW(
                    TAG,
                    "Reading INVALID (DHT11=%d | Potentiometer=%d)",
                    reading.dht_valid,
                    reading.soil_valid
                );
            }

            plant_event_t event;

            if (monitor_evaluate(&reading, &event))
            {
                ESP_LOGI(
                    TAG,
                    "PLANT EVENT: Type=%d | Moisture=%.1f%%",
                    event.type,
                    reading.moisture_pct
                );

                if (!discord_mgr_send_event(&event, &reading))
                {
                    ESP_LOGW(TAG, "Discord event could not be queued.");
                }

                if (!email_mgr_send_event(&event, &reading))
                {
                    ESP_LOGW(TAG, "Gmail event could not be queued.");
                }

                if (!callmebot_mgr_send_event(&event, &reading))
                {
                    ESP_LOGW(TAG, "CallMeBot event could not be queued.");
                }

                if (!mastodon_mgr_send_event(&event, &reading))
                {
                    ESP_LOGW(TAG, "Mastodon event could not be queued.");
                }
            }
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Sensor reading failed: %s",
                esp_err_to_name(err)
            );
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}