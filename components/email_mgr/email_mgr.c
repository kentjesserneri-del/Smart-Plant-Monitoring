
#include "email_mgr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../config.h"

#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "notification_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define EMAIL_QUEUE_LENGTH       5
#define EMAIL_TASK_STACK_SIZE    16384
#define EMAIL_TASK_PRIORITY      3

#define SMTP_BUFFER_SIZE         512
#define SMTP_TIMEOUT_MS          15000

static const char *TAG = "EMAIL_MGR";

typedef struct {
    bool is_test;
    plant_event_t event;
    sensor_reading_t reading;
} email_message_t;

static QueueHandle_t s_email_queue = NULL;
static bool s_initialized = false;

/* ---------------------------------------------------------
 * BASE64 ENCODER
 * --------------------------------------------------------- */

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static bool base64_encode(
    const unsigned char *input,
    size_t input_length,
    char *output,
    size_t output_size
)
{
    if (input == NULL || output == NULL || output_size == 0) {
        return false;
    }

    size_t i = 0;
    size_t j = 0;

    while (i < input_length) {
        if (j + 4 >= output_size) {
            output[0] = '\0';
            return false;
        }

        uint32_t value = 0;
        size_t remaining = input_length - i;

        value |= (uint32_t)input[i] << 16;

        if (remaining > 1) {
            value |= (uint32_t)input[i + 1] << 8;
        }

        if (remaining > 2) {
            value |= (uint32_t)input[i + 2];
        }

        output[j++] = base64_table[(value >> 18) & 0x3F];
        output[j++] = base64_table[(value >> 12) & 0x3F];

        output[j++] = (remaining > 1)
            ? base64_table[(value >> 6) & 0x3F]
            : '=';

        output[j++] = (remaining > 2)
            ? base64_table[value & 0x3F]
            : '=';

        i += (remaining >= 3) ? 3 : remaining;
    }

    output[j] = '\0';
    return true;
}

/* ---------------------------------------------------------
 * TLS WRITE
 * --------------------------------------------------------- */

static bool smtp_write(esp_tls_t *tls, const char *data)
{
    if (tls == NULL || data == NULL) {
        return false;
    }

    size_t length = strlen(data);
    size_t sent = 0;

    while (sent < length) {
        int result = esp_tls_conn_write(
            tls,
            data + sent,
            length - sent
        );

        if (result <= 0) {
            ESP_LOGE(TAG, "TLS write failed: %d", result);
            return false;
        }

        sent += (size_t)result;
    }

    return true;
}

/* ---------------------------------------------------------
 * SMTP RESPONSE READER
 * --------------------------------------------------------- */

static bool smtp_read_response(
    esp_tls_t *tls,
    int expected_code
)
{
    if (tls == NULL) {
        return false;
    }

    char line[SMTP_BUFFER_SIZE];
    int response_code = 0;
    bool finished = false;

    while (!finished) {
        size_t position = 0;
        bool newline_received = false;

        while (position < sizeof(line) - 1) {
            char character;

            int result = esp_tls_conn_read(
                tls,
                &character,
                1
            );

            if (result <= 0) {
                ESP_LOGE(TAG, "TLS read failed: %d", result);
                return false;
            }

            line[position++] = character;

            if (character == '\n') {
                newline_received = true;
                break;
            }
        }

        line[position] = '\0';

        if (!newline_received) {
            ESP_LOGE(TAG, "SMTP response line exceeded buffer");
            return false;
        }

        ESP_LOGI(TAG, "SMTP: %s", line);

        if (position < 4) {
            continue;
        }

        if (response_code == 0) {
            response_code = atoi(line);
        }

        /*
         * SMTP multiline responses:
         * 250- means more lines follow.
         * 250 followed by a space means the response is complete.
         */
        if (line[3] == ' ') {
            finished = true;
        }
    }

    if (response_code != expected_code) {
        ESP_LOGE(
            TAG,
            "SMTP expected %d, received %d",
            expected_code,
            response_code
        );
        return false;
    }

    return true;
}

/* ---------------------------------------------------------
 * SMTP COMMAND
 * --------------------------------------------------------- */

static bool smtp_command(
    esp_tls_t *tls,
    const char *command,
    int expected_code
)
{
    if (!smtp_write(tls, command)) {
        return false;
    }

    return smtp_read_response(tls, expected_code);
}

/* ---------------------------------------------------------
 * EVENT NAME
 * --------------------------------------------------------- */

static const char *get_event_name(int event_type)
{
    switch (event_type) {
        case EVENT_INITIAL_REPORT:
            return "INITIAL PLANT REPORT";

        case EVENT_ALERT_DRY:
            return "LOW SOIL MOISTURE ALERT";

        case EVENT_ALERT_WET:
            return "HIGH SOIL MOISTURE ALERT";

        case EVENT_ALERT_FAULT:
            return "SENSOR FAULT ALERT";

        case EVENT_RECOVERY:
            return "PLANT CONDITION RECOVERED";

        default:
            return "PLANT MONITORING EVENT";
    }
}

/* ---------------------------------------------------------
 * EMAIL BODY
 * --------------------------------------------------------- */

static void create_email_body(
    const email_message_t *message,
    char *body,
    size_t body_size
)
{
    if (message == NULL || body == NULL || body_size == 0) {
        return;
    }

    if (message->is_test) {
        snprintf(
            body,
            body_size,
            "Smart Plant Monitoring System\r\n"
            "\r\n"
            "Gmail notification test successful.\r\n"
            "\r\n"
            "Your ESP32 successfully connected to "
            "Gmail SMTP using encrypted TLS.\r\n"
        );
        return;
    }

    const sensor_reading_t *reading = &message->reading;

    time_t now = time(NULL);
    struct tm local_time;

    char timestamp[64] = "Time unavailable";

    if (now > 1700000000 &&
        localtime_r(&now, &local_time) != NULL) {
        strftime(
            timestamp,
            sizeof(timestamp),
            "%Y-%m-%d %I:%M:%S %p PHT",
            &local_time
        );
    }

    snprintf(
        body,
        body_size,
        "SMART PLANT MONITORING SYSTEM\r\n"
        "================================\r\n"
        "\r\n"
        "EVENT: %s\r\n"
        "REASON: %s\r\n"
        "TIME: %s\r\n"
        "\r\n"
        "SENSOR READINGS\r\n"
        "--------------------------------\r\n"
        "Temperature: %.1f C\r\n"
        "Air Humidity: %.1f %%\r\n"
        "Soil Moisture: %.1f %%\r\n"
        "Raw ADC: %d\r\n"
        "\r\n"
        "SENSOR STATUS\r\n"
        "--------------------------------\r\n"
        "DHT Sensor: %s\r\n"
        "Soil Sensor: %s\r\n"
        "Overall Reading: %s\r\n"
        "\r\n"
        "This is an automatic notification "
        "from your ESP32 plant monitoring device.\r\n",
        get_event_name(message->event.type),
        message->event.message_reason,
        timestamp,
        reading->temp_c,
        reading->rh_pct,
        reading->moisture_pct,
        reading->raw_adc,
        reading->dht_valid ? "VALID" : "FAILED",
        reading->soil_valid ? "VALID" : "FAILED",
        reading->valid ? "VALID" : "INVALID"
    );
}

/* ---------------------------------------------------------
 * SEND EMAIL THROUGH GMAIL SMTP
 * --------------------------------------------------------- */

static bool smtp_send_email(const email_message_t *message)
{
    if (message == NULL) {
        return false;
    }

    if (!notification_sync_acquire(pdMS_TO_TICKS(30000))) {
        ESP_LOGE(TAG, "Could not acquire shared TLS lock.");
        return false;
    }

    bool success = false;

    /*
     * These buffers are local to the email task.
     * The email task now has a 16 KB stack to accommodate
     * TLS, SMTP processing, and these buffers.
     */
    char encoded_user[256];
    char encoded_password[256];
    char command[512];
    char body[1800];
    char email[2600];
    char subject[128];

    esp_tls_t *tls = esp_tls_init();

    if (tls == NULL) {
        ESP_LOGE(TAG, "Could not initialize TLS.");
        notification_sync_release();
        return false;
    }

    esp_tls_cfg_t tls_config = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SMTP_TIMEOUT_MS
    };

    ESP_LOGI(
        TAG,
        "Connecting to Gmail SMTP: %s:%d",
        CONFIG_GMAIL_SMTP_HOST,
        CONFIG_GMAIL_SMTP_PORT
    );

    int connection_result = esp_tls_conn_new_sync(
        CONFIG_GMAIL_SMTP_HOST,
        strlen(CONFIG_GMAIL_SMTP_HOST),
        CONFIG_GMAIL_SMTP_PORT,
        &tls_config,
        tls
    );

    if (connection_result != 1) {
        ESP_LOGE(
            TAG,
            "Gmail TLS connection failed: %d",
            connection_result
        );
        goto cleanup;
    }

    ESP_LOGI(TAG, "Gmail TLS connection established");

    /* Server greeting */
    if (!smtp_read_response(tls, 220)) {
        goto cleanup;
    }

    /* EHLO */
    if (!smtp_command(
            tls,
            "EHLO esp32-plant-monitor\r\n",
            250
        )) {
        goto cleanup;
    }

    /* AUTH LOGIN */
    if (!smtp_command(tls, "AUTH LOGIN\r\n", 334)) {
        goto cleanup;
    }

    /* Encode Gmail username */
    if (!base64_encode(
            (const unsigned char *)CONFIG_GMAIL_USER,
            strlen(CONFIG_GMAIL_USER),
            encoded_user,
            sizeof(encoded_user)
        )) {
        ESP_LOGE(TAG, "Could not encode Gmail username");
        goto cleanup;
    }

    snprintf(
        command,
        sizeof(command),
        "%s\r\n",
        encoded_user
    );

    if (!smtp_command(tls, command, 334)) {
        goto cleanup;
    }

    /* Encode Gmail app password */
    if (!base64_encode(
            (const unsigned char *)CONFIG_GMAIL_APP_PASS,
            strlen(CONFIG_GMAIL_APP_PASS),
            encoded_password,
            sizeof(encoded_password)
        )) {
        ESP_LOGE(TAG, "Could not encode Gmail app password");
        goto cleanup;
    }

    snprintf(
        command,
        sizeof(command),
        "%s\r\n",
        encoded_password
    );

    if (!smtp_command(tls, command, 235)) {
        ESP_LOGE(TAG, "Gmail authentication failed");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Gmail authentication successful");

    /* MAIL FROM */
    snprintf(
        command,
        sizeof(command),
        "MAIL FROM:<%s>\r\n",
        CONFIG_GMAIL_USER
    );

    if (!smtp_command(tls, command, 250)) {
        goto cleanup;
    }

    /* RCPT TO */
    snprintf(
        command,
        sizeof(command),
        "RCPT TO:<%s>\r\n",
        CONFIG_GMAIL_TO
    );

    if (!smtp_command(tls, command, 250)) {
        goto cleanup;
    }

    /* DATA */
    if (!smtp_command(tls, "DATA\r\n", 354)) {
        goto cleanup;
    }

    if (message->is_test) {
        snprintf(
            subject,
            sizeof(subject),
            "ESP32 Plant Monitoring - Gmail Test"
        );
    } else {
        snprintf(
            subject,
            sizeof(subject),
            "Plant Alert: %s",
            get_event_name(message->event.type)
        );
    }

    create_email_body(
        message,
        body,
        sizeof(body)
    );

    int email_length = snprintf(
        email,
        sizeof(email),
        "From: Smart Plant Monitor <%s>\r\n"
        "To: <%s>\r\n"
        "Subject: %s\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n"
        "%s"
        "\r\n.\r\n",
        CONFIG_GMAIL_USER,
        CONFIG_GMAIL_TO,
        subject,
        body
    );

    if (email_length < 0 ||
        (size_t)email_length >= sizeof(email)) {
        ESP_LOGE(TAG, "Email content buffer overflow");
        goto cleanup;
    }

    if (!smtp_write(tls, email)) {
        goto cleanup;
    }

    if (!smtp_read_response(tls, 250)) {
        goto cleanup;
    }

    ESP_LOGI(TAG, "Gmail accepted the email");

    /* QUIT */
    if (!smtp_command(tls, "QUIT\r\n", 221)) {
        ESP_LOGW(TAG, "SMTP QUIT was not acknowledged");
    }

    success = true;

cleanup:
    esp_tls_conn_destroy(tls);
    notification_sync_release();

    if (success) {
        ESP_LOGI(TAG, "Email sent successfully");
    } else {
        ESP_LOGE(TAG, "Email delivery failed");
    }

    return success;
}

/* ---------------------------------------------------------
 * EMAIL TASK
 * --------------------------------------------------------- */

static void email_task(void *parameter)
{
    (void)parameter;

    ESP_LOGI(TAG, "Gmail notification task started");

    email_message_t message;

    while (true) {
        if (xQueueReceive(
                s_email_queue,
                &message,
                portMAX_DELAY
            ) == pdTRUE) {

            ESP_LOGI(TAG, "Processing Gmail notification");

            bool sent = smtp_send_email(&message);

            if (!sent) {
                ESP_LOGE(
                    TAG,
                    "Notification could not be delivered"
                );
            }

            /*
             * This reports the remaining stack space after
             * the TLS operation, helping verify stack usage.
             */
            ESP_LOGI(
                TAG,
                "Email task stack high-water mark: %u words",
                (unsigned int)uxTaskGetStackHighWaterMark(NULL)
            );

            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

/* ---------------------------------------------------------
 * INITIALIZATION
 * --------------------------------------------------------- */

esp_err_t email_mgr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_email_queue = xQueueCreate(
        EMAIL_QUEUE_LENGTH,
        sizeof(email_message_t)
    );

    if (s_email_queue == NULL) {
        ESP_LOGE(TAG, "Could not create email queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(
        email_task,
        "email_task",
        EMAIL_TASK_STACK_SIZE,
        NULL,
        EMAIL_TASK_PRIORITY,
        NULL
    );

    if (result != pdPASS) {
        vQueueDelete(s_email_queue);
        s_email_queue = NULL;

        ESP_LOGE(TAG, "Could not create email task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Gmail manager initialized");
    ESP_LOGI(TAG, "SMTP host: %s", CONFIG_GMAIL_SMTP_HOST);
    ESP_LOGI(TAG, "SMTP port: %d", CONFIG_GMAIL_SMTP_PORT);
    ESP_LOGI(
        TAG,
        "Email task stack: %d bytes",
        EMAIL_TASK_STACK_SIZE
    );

    return ESP_OK;
}

/* ---------------------------------------------------------
 * QUEUE MONITORING EVENT
 * --------------------------------------------------------- */

bool email_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
)
{
    if (!s_initialized ||
        event == NULL ||
        reading == NULL) {
        return false;
    }

    email_message_t message = {0};

    message.is_test = false;
    message.event = *event;
    message.reading = *reading;

    if (xQueueSend(
            s_email_queue,
            &message,
            0
        ) != pdTRUE) {
        ESP_LOGW(
            TAG,
            "Email queue full; event was not queued"
        );
        return false;
    }

    ESP_LOGI(
        TAG,
        "Gmail notification queued: %s",
        get_event_name(event->type)
    );

    return true;
}

/* ---------------------------------------------------------
 * QUEUE TEST EMAIL
 * --------------------------------------------------------- */

bool email_mgr_send_test(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Gmail manager is not initialized");
        return false;
    }

    email_message_t message = {0};
    message.is_test = true;

    if (xQueueSend(
            s_email_queue,
            &message,
            0
        ) != pdTRUE) {
        ESP_LOGW(TAG, "Email queue full");
        return false;
    }

    ESP_LOGI(TAG, "Gmail test email queued");
    return true;
}
