
#include "sensors.h"
#include "monitoring.h"
#include "wifi_mgr.h"
#include "mqtt_mgr.h"
#include "discord_mgr.h"
#include "email_mgr.h"
#include "callmebot_mgr.h"
#include "mastodon_mgr.h"
#include "notification_sync.h"
#include "sensor_task.h"
#include "system_time.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define WIFI_TIME_SYNC_TIMEOUT_MS 30000
#define SENSOR_TASK_STACK_SIZE    4096
#define SENSOR_TASK_PRIORITY      5
#define SENSOR_TASK_CORE          1

void app_main(void)
{
    ESP_LOGI(TAG, "Booting Smart Plant Monitoring System...");

    ESP_ERROR_CHECK(notification_sync_init());

    /* 1. Initialize sensors and monitoring. */

    ESP_ERROR_CHECK(sensors_init());

    monitor_init();

    ESP_LOGI(TAG, "Sensors and monitoring initialized.");

    /* 2. Initialize Wi-Fi and SNTP. */

    esp_err_t wifi_err = wifi_mgr_init();

    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi manager initialized.");
        ESP_LOGI(TAG, "Waiting for Philippine system time...");

        BaseType_t time_synced = wifi_mgr_wait_for_time(
            pdMS_TO_TICKS(WIFI_TIME_SYNC_TIMEOUT_MS)
        );

        if (time_synced == pdTRUE) {
            ESP_LOGI(TAG, "System time synchronized successfully.");
            display_system_time();
        } else {
            ESP_LOGW(TAG, "SNTP synchronization timeout.");
            ESP_LOGW(TAG, "Continuing without synchronized time.");
        }
    } else {
        ESP_LOGE(
            TAG,
            "Wi-Fi initialization failed: %s",
            esp_err_to_name(wifi_err)
        );

        ESP_LOGW(TAG, "Continuing without Wi-Fi.");
    }

    /* 3. Initialize MQTT. */

    esp_err_t mqtt_err = mqtt_mgr_init();

    if (mqtt_err == ESP_OK) {
        ESP_LOGI(TAG, "MQTT manager initialized.");
    } else {
        ESP_LOGE(
            TAG,
            "MQTT initialization failed: %s",
            esp_err_to_name(mqtt_err)
        );
    }

    /* 4. Initialize Discord. */

    esp_err_t discord_err = discord_mgr_init();

    if (discord_err == ESP_OK) {
        ESP_LOGI(TAG, "Discord manager initialized.");
    } else {
        ESP_LOGE(
            TAG,
            "Discord initialization failed: %s",
            esp_err_to_name(discord_err)
        );
    }

    /* 5. Initialize Gmail. */

    esp_err_t email_err = email_mgr_init();

    if (email_err == ESP_OK) {
        ESP_LOGI(TAG, "Gmail manager initialized.");
    } else {
        ESP_LOGE(
            TAG,
            "Gmail initialization failed: %s",
            esp_err_to_name(email_err)
        );
    }

    /* 6. Initialize CallMeBot. */

    esp_err_t callmebot_err = callmebot_mgr_init();

    if (callmebot_err == ESP_OK) {
        ESP_LOGI(TAG, "CallMeBot manager initialized.");
    } else {
        ESP_LOGE(
            TAG,
            "CallMeBot initialization failed: %s",
            esp_err_to_name(callmebot_err)
        );
    }

    /* 7. Initialize Mastodon. */

    esp_err_t mastodon_err = mastodon_mgr_init();

    if (mastodon_err == ESP_OK) {
        ESP_LOGI(TAG, "Mastodon manager initialized.");
    } else {
        ESP_LOGE(
            TAG,
            "Mastodon initialization failed: %s",
            esp_err_to_name(mastodon_err)
        );
    }

    /* 8. Start sensor monitoring task on Core 1. */

    BaseType_t task_result = xTaskCreatePinnedToCore(
        sensor_monitor_task,
        "sensor_monitor",
        SENSOR_TASK_STACK_SIZE,
        NULL,
        SENSOR_TASK_PRIORITY,
        NULL,
        SENSOR_TASK_CORE
    );

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor monitoring task.");
        return;
    }

    ESP_LOGI(TAG, "Sensor monitoring task started successfully.");

    ESP_LOGI(
        TAG,
        "Automatic notification tests disabled. "
        "Waiting for real plant monitoring events."
    );

    ESP_LOGI(TAG, "Smart Plant Monitoring System is running.");
}
