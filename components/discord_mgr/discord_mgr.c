
#include "discord_mgr.h"
#include "../../config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "notification_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DISCORD_QUEUE_LENGTH       5
#define DISCORD_TASK_STACK_SIZE    6144
#define DISCORD_TASK_PRIORITY      3
#define DISCORD_HTTP_TIMEOUT_MS    15000

static const char *TAG = "discord_mgr";

typedef struct {
    bool is_test;
    plant_event_t event;
    sensor_reading_t reading;
} discord_message_t;

static QueueHandle_t discord_queue = NULL;

static const char *get_event_title(plant_event_type_t type)
{
    switch (type) {
        case EVENT_INITIAL_REPORT:
            return "Plant Monitoring - Initial Report";

        case EVENT_ALERT_DRY:
            return "PLANT ALERT - SOIL TOO DRY";

        case EVENT_ALERT_WET:
            return "PLANT ALERT - SOIL TOO WET";

        case EVENT_ALERT_FAULT:
            return "PLANT ALERT - SENSOR FAULT";

        case EVENT_RECOVERY:
            return "PLANT STATUS - RECOVERED";

        default:
            return "Plant Monitoring Event";
    }
}

/*
 * Build the JSON body for either a test message or a plant event.
 */
static void build_discord_payload(
    const discord_message_t *message,
    char *payload,
    size_t payload_size
)
{
    if (message->is_test) {
        snprintf(
            payload,
            payload_size,
            "{\"content\":\"**Smart Plant Monitoring System**\\n"
            "Discord webhook test successful.\\n"
            "Your ESP32 has successfully reached this Discord channel "
            "using HTTPS.\"}"
        );

        return;
    }

    char timestamp_buffer[32] = "Time unavailable";

    time_t now = message->reading.timestamp;

    if (now <= 0) {
        time(&now);
    }

    if (now > 0) {
        struct tm local_time;

        if (localtime_r(&now, &local_time) != NULL) {
            strftime(
                timestamp_buffer,
                sizeof(timestamp_buffer),
                "%Y-%m-%d %H:%M:%S PHT",
                &local_time
            );
        }
    }

    const char *title = get_event_title(message->event.type);

    if (message->reading.valid) {
        snprintf(
            payload,
            payload_size,
            "{\"content\":\"**%s**\\n"
            "Reason: %s\\n"
            "Temperature: %.1f C\\n"
            "Humidity: %.1f %%\\n"
            "Soil Moisture: %.1f %%\\n"
            "Raw ADC: %d\\n"
            "Time: %s\"}",
            title,
            message->event.message_reason,
            message->reading.temp_c,
            message->reading.rh_pct,
            message->reading.moisture_pct,
            message->reading.raw_adc,
            timestamp_buffer
        );
    } else {
        snprintf(
            payload,
            payload_size,
            "{\"content\":\"**%s**\\n"
            "Reason: %s\\n"
            "DHT11 valid: %s\\n"
            "Soil sensor valid: %s\\n"
            "Time: %s\"}",
            title,
            message->event.message_reason,
            message->reading.dht_valid ? "Yes" : "No",
            message->reading.soil_valid ? "Yes" : "No",
            timestamp_buffer
        );
    }
}

/*
 * Send one HTTPS POST request to the Discord webhook.
 */
static esp_err_t discord_send_http(
    const discord_message_t *message
)
{
#if !CONFIG_DISCORD_ENABLED

    ESP_LOGW(TAG, "Discord notifications are disabled.");
    return ESP_ERR_NOT_SUPPORTED;

#else

    char payload[512];

    build_discord_payload(
        message,
        payload,
        sizeof(payload)
    );

    esp_http_client_config_t config = {
        .url = CONFIG_DISCORD_WEBHOOK_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DISCORD_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    if (!notification_sync_acquire(pdMS_TO_TICKS(30000))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client.");

        notification_sync_release();

        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json"
    );

    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(
            client,
            payload,
            strlen(payload)
        );
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Connecting to Discord webhook...");
        err = esp_http_client_perform(client);
    }

    if (err == ESP_OK) {
        int status_code =
            esp_http_client_get_status_code(client);

        if (status_code == 204 || status_code == 200) {
            ESP_LOGI(
                TAG,
                "Discord message accepted. HTTP status: %d",
                status_code
            );
        } else {
            ESP_LOGE(
                TAG,
                "Discord returned HTTP status: %d",
                status_code
            );

            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(
            TAG,
            "Discord HTTPS request failed: %s",
            esp_err_to_name(err)
        );
    }

    esp_http_client_cleanup(client);

    notification_sync_release();

    return err;

#endif
}

/*
 * Dedicated notification task.
 * HTTPS requests run here, never inside the sensor task.
 */
static void discord_notification_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Discord notification task started.");

    discord_message_t message;

    while (1) {
        if (xQueueReceive(
                discord_queue,
                &message,
                portMAX_DELAY
            ) == pdTRUE) {

            if (message.is_test) {
                ESP_LOGI(TAG, "Sending one-time Discord test...");
            } else {
                ESP_LOGI(
                    TAG,
                    "Sending plant event: %s",
                    get_event_title(message.event.type)
                );
            }

            esp_err_t err = discord_send_http(&message);

            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Discord message was not delivered."
                );
            }
        }
    }
}

/*
 * Initialize Discord queue and task.
 */
esp_err_t discord_mgr_init(void)
{
#if !CONFIG_DISCORD_ENABLED

    ESP_LOGW(TAG, "Discord notifications are disabled.");
    return ESP_OK;

#endif

    if (discord_queue != NULL) {
        ESP_LOGW(TAG, "Discord manager already initialized.");
        return ESP_OK;
    }

    if (CONFIG_DISCORD_WEBHOOK_URL[0] == '\0') {
        ESP_LOGE(TAG, "Discord webhook URL is empty.");
        return ESP_ERR_INVALID_ARG;
    }

    discord_queue = xQueueCreate(
        DISCORD_QUEUE_LENGTH,
        sizeof(discord_message_t)
    );

    if (discord_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create Discord queue.");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result = xTaskCreate(
        discord_notification_task,
        "discord_notify",
        DISCORD_TASK_STACK_SIZE,
        NULL,
        DISCORD_TASK_PRIORITY,
        NULL
    );

    if (task_result != pdPASS) {
        vQueueDelete(discord_queue);
        discord_queue = NULL;

        ESP_LOGE(TAG, "Failed to create Discord task.");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Discord manager initialized.");

    return ESP_OK;
}

/*
 * Queue exactly one test message when called once.
 */
bool discord_mgr_send_test(void)
{
    if (discord_queue == NULL) {
        ESP_LOGW(TAG, "Discord queue is not initialized.");
        return false;
    }

    discord_message_t message = {0};

    message.is_test = true;

    if (xQueueSend(discord_queue, &message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discord queue full. Test was not queued.");
        return false;
    }

    ESP_LOGI(TAG, "One-time Discord test queued.");

    return true;
}

/*
 * Queue a plant event without performing network activity here.
 */
bool discord_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
)
{
    if (event == NULL || reading == NULL) {
        return false;
    }

    if (discord_queue == NULL) {
        ESP_LOGW(TAG, "Discord queue is not initialized.");
        return false;
    }

    discord_message_t message = {0};

    message.is_test = false;
    message.event = *event;
    message.reading = *reading;

    if (xQueueSend(discord_queue, &message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Discord queue full. Event was not queued.");
        return false;
    }

    ESP_LOGI(TAG, "Plant event queued for Discord.");

    return true;
}
