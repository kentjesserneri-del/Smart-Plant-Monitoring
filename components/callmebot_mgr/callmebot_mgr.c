
#include "callmebot_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../../config.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "notification_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CALLMEBOT_QUEUE_LENGTH  5
#define CALLMEBOT_TASK_STACK    16384
#define CALLMEBOT_TASK_PRIORITY 3
#define HTTP_TIMEOUT_MS         15000

#define CALLMEBOT_TEXT_SIZE     512
#define CALLMEBOT_ENCODED_SIZE  1200
#define CALLMEBOT_URL_SIZE      1800

static const char *TAG = "CALLMEBOT";

typedef struct {
    bool is_test;
    plant_event_t event;
    sensor_reading_t reading;
} callmebot_message_t;

static QueueHandle_t s_queue = NULL;
static bool s_initialized = false;

/* Convert text into URL-safe characters. */
static bool url_encode(
    const char *input,
    char *output,
    size_t output_size
)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!input || !output || output_size == 0) {
        return false;
    }

    size_t j = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];

        bool safe =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            if (j + 1 >= output_size) {
                return false;
            }

            output[j++] = (char)c;
        } else {
            if (j + 3 >= output_size) {
                return false;
            }

            output[j++] = '%';
            output[j++] = hex[c >> 4];
            output[j++] = hex[c & 0x0F];
        }
    }

    output[j] = '\0';

    return true;
}

static const char *event_name(plant_event_type_t type)
{
    switch (type) {
        case EVENT_INITIAL_REPORT:
            return "INITIAL PLANT REPORT";

        case EVENT_ALERT_DRY:
            return "DRY SOIL ALERT";

        case EVENT_ALERT_WET:
            return "WET SOIL ALERT";

        case EVENT_ALERT_FAULT:
            return "SENSOR FAULT ALERT";

        case EVENT_RECOVERY:
            return "PLANT RECOVERY";

        default:
            return "PLANT MONITORING EVENT";
    }
}

static bool send_callmebot_message(const char *message)
{
    bool success = false;

    /* Allocate large buffers from heap instead of task stack. */
    char *encoded_text = malloc(CALLMEBOT_ENCODED_SIZE);
    char *url = malloc(CALLMEBOT_URL_SIZE);

    if (encoded_text == NULL || url == NULL) {
        ESP_LOGE(TAG, "Not enough heap for CallMeBot buffers.");

        free(encoded_text);
        free(url);

        return false;
    }

    if (!url_encode(
            message,
            encoded_text,
            CALLMEBOT_ENCODED_SIZE
        )) {
        ESP_LOGE(TAG, "Message URL encoding failed.");

        free(encoded_text);
        free(url);

        return false;
    }

    int written = snprintf(
        url,
        CALLMEBOT_URL_SIZE,
        "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
        CONFIG_CALLMEBOT_PHONE,
        encoded_text,
        CONFIG_CALLMEBOT_API_KEY
    );

    if (written < 0 || written >= CALLMEBOT_URL_SIZE) {
        ESP_LOGE(TAG, "CallMeBot URL is too long.");

        free(encoded_text);
        free(url);

        return false;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    if (!notification_sync_acquire(pdMS_TO_TICKS(30000))) {
        free(encoded_text);
        free(url);
        return false;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG, "Could not initialize HTTP client.");

        notification_sync_release();

        free(encoded_text);
        free(url);

        return false;
    }

    ESP_LOGI(TAG, "Starting CallMeBot HTTPS request.");

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "CallMeBot HTTP status: %d", status);

        if (status >= 200 && status < 300) {
            success = true;
            ESP_LOGI(TAG, "CallMeBot accepted the request.");
        } else {
            ESP_LOGE(TAG, "CallMeBot rejected the request.");
        }
    } else {
        ESP_LOGE(
            TAG,
            "CallMeBot request failed: %s",
            esp_err_to_name(err)
        );
    }

    esp_http_client_cleanup(client);

    notification_sync_release();

    free(encoded_text);
    free(url);

    return success;
}

static bool process_message(const callmebot_message_t *message)
{
    /* Keep the message buffer off the task stack. */
    char *text = malloc(CALLMEBOT_TEXT_SIZE);

    if (text == NULL) {
        ESP_LOGE(TAG, "Could not allocate notification message.");
        return false;
    }

    if (message->is_test) {
        snprintf(
            text,
            CALLMEBOT_TEXT_SIZE,
            "Smart Plant Monitoring System\n"
            "CallMeBot WhatsApp integration test successful.\n"
            "Your ESP32 successfully sent a test notification."
        );
    } else {
        const sensor_reading_t *r = &message->reading;

        snprintf(
            text,
            CALLMEBOT_TEXT_SIZE,
            "SMART PLANT MONITORING SYSTEM\n"
            "Event: %s\n"
            "Reason: %s\n"
            "Temperature: %.1f C\n"
            "Air Humidity: %.1f%%\n"
            "Soil Moisture: %.1f%%\n"
            "Raw ADC: %d\n"
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

    ESP_LOGI(TAG, "Processing WhatsApp notification.");

    bool result = send_callmebot_message(text);

    free(text);

    return result;
}

static void callmebot_task(void *pvParameters)
{
    (void)pvParameters;

    callmebot_message_t message;

    ESP_LOGI(
        TAG,
        "CallMeBot notification task started. Stack: %d bytes",
        CALLMEBOT_TASK_STACK
    );

    while (1) {
        if (xQueueReceive(
                s_queue,
                &message,
                portMAX_DELAY
            ) == pdTRUE) {

            ESP_LOGI(TAG, "Processing queued notification.");

            bool success = process_message(&message);

            if (success) {
                ESP_LOGI(
                    TAG,
                    "WhatsApp notification request accepted."
                );
            } else {
                ESP_LOGE(
                    TAG,
                    "WhatsApp notification failed."
                );
            }

            UBaseType_t remaining_stack =
                uxTaskGetStackHighWaterMark(NULL);

            ESP_LOGI(
                TAG,
                "CallMeBot remaining minimum stack: %u bytes",
                (unsigned int)remaining_stack
            );

            ESP_LOGI(
                TAG,
                "Free heap after notification: %lu bytes",
                (unsigned long)esp_get_free_heap_size()
            );
        }
    }
}

esp_err_t callmebot_mgr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(
        CALLMEBOT_QUEUE_LENGTH,
        sizeof(callmebot_message_t)
    );

    if (s_queue == NULL) {
        ESP_LOGE(TAG, "Could not create CallMeBot queue.");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(
        callmebot_task,
        "callmebot_task",
        CALLMEBOT_TASK_STACK,
        NULL,
        CALLMEBOT_TASK_PRIORITY,
        NULL
    );

    if (result != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;

        ESP_LOGE(TAG, "Could not create CallMeBot task.");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "CallMeBot manager initialized.");

    return ESP_OK;
}

bool callmebot_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
)
{
    if (!s_initialized || event == NULL || reading == NULL) {
        return false;
    }

    callmebot_message_t message = {
        .is_test = false,
        .event = *event,
        .reading = *reading
    };

    return xQueueSend(
        s_queue,
        &message,
        0
    ) == pdTRUE;
}

bool callmebot_mgr_send_test(void)
{
    if (!s_initialized) {
        return false;
    }

    callmebot_message_t message = {
        .is_test = true
    };

    return xQueueSend(
        s_queue,
        &message,
        0
    ) == pdTRUE;
}
