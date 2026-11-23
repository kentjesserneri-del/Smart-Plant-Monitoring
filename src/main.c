
#include <stdio.h>

#include "sensors.h"
#include "monitoring.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/*
 * Sensor monitoring task
 *
 * Runs on Core 1 to isolate the timing-sensitive
 * DHT11 communication from Wi-Fi activity.
 */
static void sensor_monitor_task(void *pvParameters)
{
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

                /*
                 * Queue the reading for MQTT.
                 * This does not wait for network transmission.
                 */
                if (!mqtt_mgr_publish_reading(&reading))
                {
                    ESP_LOGW(
                        TAG,
                        "MQTT reading queue unavailable or full."
                    );
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

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting Smart Plant Monitoring System...");

    /*
     * Initialize existing sensors.
     */
    ESP_ERROR_CHECK(sensors_init());

    monitor_init();

    ESP_LOGI(TAG, "Sensors and monitoring initialized.");

    /*
     * Initialize Wi-Fi.
     */
    esp_err_t wifi_err = wifi_mgr_init();

    if (wifi_err == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi manager initialized.");
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi initialization failed: %s",
            esp_err_to_name(wifi_err)
        );

        ESP_LOGW(TAG, "Sensor monitoring will continue.");
    }

    /*
     * Initialize MQTT independently.
     * The MQTT client handles its own connection attempts.
     */
    esp_err_t mqtt_err = mqtt_mgr_init();

    if (mqtt_err == ESP_OK)
    {
        ESP_LOGI(TAG, "MQTT manager initialized.");
    }
    else
    {
        ESP_LOGE(
            TAG,
            "MQTT initialization failed: %s",
            esp_err_to_name(mqtt_err)
        );

        ESP_LOGW(
            TAG,
            "Sensor monitoring will continue without MQTT."
        );
    }

    /*
     * Run sensor monitoring on Core 1.
     */
    BaseType_t task_result = xTaskCreatePinnedToCore(
        sensor_monitor_task,
        "sensor_monitor",
        4096,
        NULL,
        5,
        NULL,
        1
    );

    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create sensor monitoring task.");
    }
}
