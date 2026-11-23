#include "mastodon_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../../config.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "notification_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MASTODON_QUEUE_LENGTH  5
#define MASTODON_TASK_STACK    8192
#define MASTODON_TASK_PRIORITY 3
#define HTTP_TIMEOUT_MS       15000

static const char *TAG = "MASTODON";

typedef struct {
    bool is_test;
    plant_event_t event;
    sensor_reading_t reading;
} mastodon_message_t;

static QueueHandle_t s_queue = NULL;
static bool s_initialized = false;

static const char *event_name(plant_event_type_t type)
{
    switch (type) {
        case EVENT_INITIAL_REPORT:
            return "Initial Plant Report";

        case EVENT_ALERT_DRY:
            return "Dry Soil Alert";

        case EVENT_ALERT_WET:
            return "Wet Soil Alert";

        case EVENT_ALERT_FAULT:
            return "Sensor Fault Alert";

        case EVENT_RECOVERY:
            return "Plant Recovery";

        default:
            return "Plant Monitoring Event";
    }
}

/* Escape JSON string characters. */
static bool json_escape(
    const char *input,
    char *output,
    size_t output_size
)
{
    if (!input || !output || output_size == 0) {
        return false;
    }

    size_t j = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];

        if (c == '"' || c == '\\') {
            if (j + 2 >= output_size) return false;

            output[j++] = '\\';
            output[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= output_size) return false;

            output[j++] = '\\';
            output[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= output_size) return false;

            output[j++] = '\\';
            output[j++] = 'r';
        } else if (c < 0x20) {
            continue;
        } else {
            if (j + 1 >= output_size) return false;

            output[j++] = (char)c;
        }
    }

    output[j] = '\0';

    return true;
}

static bool send_mastodon_status(const char *status_text)
{
    char escaped_status[1200];
    char json_body[1400];
    char endpoint[256];

    if (!json_escape(
            status_text,
            escaped_status,
            sizeof(escaped_status)
        )) {
        ESP_LOGE(TAG, "Could not escape Mastodon status.");
        return false;
    }

    int endpoint_length = snprintf(
        endpoint,
        sizeof(endpoint),
        "%s/api/v1/statuses",
        CONFIG_MASTODON_INSTANCE
    );

    if (endpoint_length < 0 || endpoint_length >= sizeof(endpoint)) {
        ESP_LOGE(TAG, "Mastodon endpoint is too long.");
        return false;
    }

    int body_length = snprintf(
        json_body,
        sizeof(json_body),
        "{\"status\":\"%s\",\"visibility\":\"unlisted\"}",
        escaped_status
    );

    if (body_length < 0 || body_length >= sizeof(json_body)) {
        ESP_LOGE(TAG, "Mastodon JSON body is too long.");
        return false;
    }

    esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    if (!notification_sync_acquire(pdMS_TO_TICKS(30000))) {
        return false;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Could not initialize HTTP client.");

        notification_sync_release();

        return false;
    }

    esp_http_client_set_header(
        client,
        "Authorization",
        "Bearer " CONFIG_MASTODON_TOKEN
    );

    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/json"
    );

    esp_http_client_set_post_field(
        client,
        json_body,
        body_length
    );

    esp_err_t err = esp_http_client_perform(client);

    bool success = false;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "Mastodon HTTP status: %d", status);

        if (status == 200 || status == 201) {
            success = true;
            ESP_LOGI(TAG, "Mastodon accepted the status.");
        } else {
            ESP_LOGE(TAG, "Mastodon rejected the status.");
        }
    } else {
        ESP_LOGE(
            TAG,
            "Mastodon request failed: %s",
            esp_err_to_name(err)
        );
    }

    esp_http_client_cleanup(client);

    notification_sync_release();

    return success;
}

static bool process_message(const mastodon_message_t *message)
{
    char status[1000];

    if (message->is_test) {
        snprintf(
            status,
            sizeof(status),
            "Smart Plant Monitoring System: Mastodon integration test successful. "
            "This status was sent by my ESP32 device."
        );
    } else {
        const sensor_reading_t *r = &message->reading;

        snprintf(
            status,
            sizeof(status),
            "Smart Plant Monitoring System\n\n"
            "Event: %s\n"
            "Reason: %s\n\n"
            "Temperature: %.1f C\n"
            "Air Humidity: %.1f%%\n"
            "Soil Moisture: %.1f%%\n"
            "Raw ADC: %d\n\n"
            "DHT Sensor: %s\n"
            "Soil Sensor: %s",
            event_name(message->event.type),
            message->event.message_reason,
            r->temp_c,
            r->rh_pct,
            r->moisture_pct,
            r->raw_adc,
            r->dht_valid ? "VALID" : "FAILED",
            r->soil_valid ? "VALID" : "FAILED"
        );
    }

    ESP_LOGI(TAG, "Processing Mastodon notification.");

    return send_mastodon_status(status);
}

static void mastodon_task(void *pvParameters)
{
    (void)pvParameters;

    mastodon_message_t message;

    ESP_LOGI(TAG, "Mastodon notification task started.");

    while (1) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) == pdTRUE) {
            bool success = process_message(&message);

            if (success) {
                ESP_LOGI(TAG, "Mastodon status successfully submitted.");
            } else {
                ESP_LOGE(TAG, "Mastodon status submission failed.");
            }
        }
    }
}

esp_err_t mastodon_mgr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(
        MASTODON_QUEUE_LENGTH,
        sizeof(mastodon_message_t)
    );

    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Could not create Mastodon queue.");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(
        mastodon_task,
        "mastodon_task",
        MASTODON_TASK_STACK,
        NULL,
        MASTODON_TASK_PRIORITY,
        NULL
    );

    if (result != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;

        ESP_LOGE(TAG, "Could not create Mastodon task.");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Mastodon manager initialized.");

    return ESP_OK;
}

bool mastodon_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
)
{
    if (!s_initialized || event == NULL || reading == NULL) {
        return false;
    }

    mastodon_message_t message = {
        .is_test = false,
        .event = *event,
        .reading = *reading
    };

    return xQueueSend(s_queue, &message, 0) == pdTRUE;
}

bool mastodon_mgr_send_test(void)
{
    if (!s_initialized) {
        return false;
    }

    mastodon_message_t message = {
        .is_test = true
    };

    return xQueueSend(s_queue, &message, 0) == pdTRUE;
}