
#include "mqtt_mgr.h"

#include "../../config.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdint.h>

static const char *TAG = "MQTT_MGR";

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_QUEUE_LENGTH  4
#define MQTT_TASK_STACK    4096
#define MQTT_TASK_PRIORITY 4

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static QueueHandle_t s_reading_queue = NULL;
static EventGroupHandle_t s_mqtt_events = NULL;

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
)
{
    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to mqtty broker.");

            xEventGroupSetBits(
                s_mqtt_events,
                MQTT_CONNECTED_BIT
            );

            /*
             * Publish retained ONLINE status.
             * The broker will retain this as the latest status.
             */
            esp_mqtt_client_enqueue(
                s_mqtt_client,
                CONFIG_MQTT_TOPIC_STATUS,
                "online",
                0,
                1,
                1,
                true
            );

            ESP_LOGI(TAG, "Online status queued.");
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from MQTT broker.");

            xEventGroupClearBits(
                s_mqtt_events,
                MQTT_CONNECTED_BIT
            );
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(
                TAG,
                "Message acknowledged. ID=%d",
                event->msg_id
            );
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT transport or broker error.");

            if (event->error_handle != NULL)
            {
                ESP_LOGE(
                    TAG,
                    "MQTT error type: %d",
                    event->error_handle->error_type
                );

                ESP_LOGE(
                    TAG,
                    "TLS error: %s",
                    esp_err_to_name(
                        event->error_handle->esp_tls_last_esp_err
                    )
                );
            }
            break;

        default:
            break;
    }
}

static void mqtt_publisher_task(void *pvParameters)
{
    sensor_reading_t reading;

    ESP_LOGI(TAG, "MQTT publisher task started.");

    while (1)
    {
        if (xQueueReceive(
                s_reading_queue,
                &reading,
                portMAX_DELAY
            ) != pdTRUE)
        {
            continue;
        }

        EventBits_t bits = xEventGroupGetBits(s_mqtt_events);

        if (!(bits & MQTT_CONNECTED_BIT))
        {
            ESP_LOGW(
                TAG,
                "MQTT disconnected; dropping queued reading."
            );
            continue;
        }

        char payload[24];

        /* Temperature */
        snprintf(
            payload,
            sizeof(payload),
            "%.1f",
            reading.temp_c
        );

        int msg_id = esp_mqtt_client_enqueue(
            s_mqtt_client,
            CONFIG_MQTT_TOPIC_TEMP,
            payload,
            0,
            1,
            1,
            true
        );

        if (msg_id < 0)
        {
            ESP_LOGW(TAG, "Temperature enqueue failed: %d", msg_id);
        }

        /* Humidity */
        snprintf(
            payload,
            sizeof(payload),
            "%.1f",
            reading.rh_pct
        );

        msg_id = esp_mqtt_client_enqueue(
            s_mqtt_client,
            CONFIG_MQTT_TOPIC_HUMIDITY,
            payload,
            0,
            1,
            1,
            true
        );

        if (msg_id < 0)
        {
            ESP_LOGW(TAG, "Humidity enqueue failed: %d", msg_id);
        }

        /* Simulated soil moisture */
        snprintf(
            payload,
            sizeof(payload),
            "%.1f",
            reading.moisture_pct
        );

        msg_id = esp_mqtt_client_enqueue(
            s_mqtt_client,
            CONFIG_MQTT_TOPIC_MOISTURE,
            payload,
            0,
            1,
            1,
            true
        );

        if (msg_id < 0)
        {
            ESP_LOGW(TAG, "Moisture enqueue failed: %d", msg_id);
        }
        else
        {
            ESP_LOGI(TAG, "Sensor readings queued for MQTT.");
        }
    }
}

esp_err_t mqtt_mgr_init(void)
{
    if (s_mqtt_client != NULL)
    {
        return ESP_OK;
    }

    if (CONFIG_MQTT_USERNAME[0] == '\0' ||
        CONFIG_MQTT_PASSWORD[0] == '\0')
    {
        ESP_LOGE(TAG, "MQTT credentials are empty.");
        return ESP_ERR_INVALID_ARG;
    }

    s_reading_queue = xQueueCreate(
        MQTT_QUEUE_LENGTH,
        sizeof(sensor_reading_t)
    );

    if (s_reading_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create MQTT reading queue.");
        return ESP_ERR_NO_MEM;
    }

    s_mqtt_events = xEventGroupCreate();

    if (s_mqtt_events == NULL)
    {
        ESP_LOGE(TAG, "Failed to create MQTT event group.");
        vQueueDelete(s_reading_queue);
        s_reading_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t mqtt_config = {
        .broker = {
            .address = {
                .uri = CONFIG_MQTT_BROKER_URI,
            },
            .verification = {
                .crt_bundle_attach = esp_crt_bundle_attach,
            },
        },

        .credentials = {
            .username = CONFIG_MQTT_USERNAME,
            .client_id = CONFIG_MQTT_CLIENT_ID,

            .authentication = {
                .password = CONFIG_MQTT_PASSWORD,
            },
        },

        .session = {
            .keepalive = 60,

            /*
             * LAST WILL AND TESTAMENT
             *
             * If the ESP32 unexpectedly loses its MQTT
             * connection, the broker publishes "offline".
             *
             * QoS 1 and retained = true ensure subscribers
             * can receive the latest device status.
             */
            .last_will = {
                .topic = CONFIG_MQTT_TOPIC_STATUS,
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = 1,
            },
        },

        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms = 10000,
        },

        .task = {
            .priority = 4,
            .stack_size = 4096,
        },

        .buffer = {
            .size = 1024,
        },
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);

    if (s_mqtt_client == NULL)
    {
        ESP_LOGE(TAG, "MQTT client initialization failed.");

        vEventGroupDelete(s_mqtt_events);
        vQueueDelete(s_reading_queue);

        s_mqtt_events = NULL;
        s_reading_queue = NULL;

        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(
        s_mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "MQTT event registration failed.");
        return err;
    }

    BaseType_t task_result = xTaskCreate(
        mqtt_publisher_task,
        "mqtt_publisher",
        MQTT_TASK_STACK,
        NULL,
        MQTT_TASK_PRIORITY,
        NULL
    );

    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "MQTT publisher task creation failed.");
        return ESP_ERR_NO_MEM;
    }

    err = esp_mqtt_client_start(s_mqtt_client);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "MQTT client start failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(TAG, "MQTT client started.");
    ESP_LOGI(TAG, "MQTT Last Will configured: offline.");

    return ESP_OK;
}

bool mqtt_mgr_publish_reading(const sensor_reading_t *reading)
{
    if (reading == NULL || !reading->valid)
    {
        return false;
    }

    if (s_reading_queue == NULL)
    {
        return false;
    }

    /*
     * Zero wait: sensor task never waits for MQTT.
     */
    return xQueueSend(
        s_reading_queue,
        reading,
        0
    ) == pdTRUE;
}

bool mqtt_mgr_is_connected(void)
{
    if (s_mqtt_events == NULL)
    {
        return false;
    }

    return (
        xEventGroupGetBits(s_mqtt_events) &
        MQTT_CONNECTED_BIT
    ) != 0;
}
