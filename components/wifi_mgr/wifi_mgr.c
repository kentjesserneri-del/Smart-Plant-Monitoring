
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>

#include "../../config.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* =================================================
 * CONFIGURATION
 * ================================================= */

#define WIFI_RECONNECT_INITIAL_MS    1000U
#define WIFI_RECONNECT_MAX_MS        30000U

#define WIFI_TASK_STACK_SIZE         3072
#define WIFI_TASK_PRIORITY           4

#define SNTP_SERVER                  "pool.ntp.org"

/* Unix timestamp: November 14, 2023 */
#define VALID_TIME_THRESHOLD         1700000000LL

static const char *TAG = "WIFI_MGR";

/* =================================================
 * PRIVATE VARIABLES
 * ================================================= */

static EventGroupHandle_t s_wifi_event_group = NULL;

static esp_netif_t *s_sta_netif = NULL;

static TaskHandle_t s_reconnect_task = NULL;

static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

static bool s_initialized = false;

/* =================================================
 * HELPER FUNCTIONS
 * ================================================= */

static bool system_time_is_valid(void)
{
    time_t now;
    time(&now);

    return ((int64_t)now >= VALID_TIME_THRESHOLD);
}

/*
 * Philippines timezone.
 * PHT-8 means UTC+8 in POSIX TZ notation.
 */
static void configure_timezone(void)
{
    setenv("TZ", "PHT-8", 1);
    tzset();
}

/* =================================================
 * SNTP
 * ================================================= */

static void sntp_time_sync_callback(struct timeval *tv)
{
    (void)tv;

    ESP_LOGI(TAG, "SNTP time synchronization completed");

    if (system_time_is_valid()) {

        xEventGroupSetBits(
            s_wifi_event_group,
            TIME_SYNCED_BIT
        );

        time_t now;
        struct tm utc_time;
        struct tm local_time;

        time(&now);

        gmtime_r(&now, &utc_time);
        localtime_r(&now, &local_time);

        ESP_LOGI(
            TAG,
            "System clock synchronized successfully"
        );
    }
}

static void initialize_sntp(void)
{
    configure_timezone();

    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    esp_sntp_setservername(
        0,
        SNTP_SERVER
    );

    esp_sntp_set_time_sync_notification_cb(
        sntp_time_sync_callback
    );

    esp_sntp_init();

    ESP_LOGI(
        TAG,
        "SNTP started using %s",
        SNTP_SERVER
    );
}

/* =================================================
 * RECONNECTION BACKOFF
 * ================================================= */

static uint32_t increase_backoff(uint32_t current_ms)
{
    if (current_ms >= WIFI_RECONNECT_MAX_MS / 2U) {
        return WIFI_RECONNECT_MAX_MS;
    }

    current_ms *= 2U;

    if (current_ms > WIFI_RECONNECT_MAX_MS) {
        current_ms = WIFI_RECONNECT_MAX_MS;
    }

    return current_ms;
}

/* =================================================
 * WIFI RECONNECTION TASK
 * ================================================= */

static void wifi_reconnect_task(void *pvParameters)
{
    (void)pvParameters;

    uint32_t backoff_ms = WIFI_RECONNECT_INITIAL_MS;
    bool first_attempt = true;

    ESP_LOGI(TAG, "Wi-Fi reconnect task started");

    while (1) {

        /*
         * Wait for notification from the Wi-Fi
         * event handler.
         */
        ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        /* Reset backoff after successful connection. */
        if (wifi_mgr_is_connected()) {
            backoff_ms = WIFI_RECONNECT_INITIAL_MS;
            continue;
        }

        if (!first_attempt) {
            ESP_LOGI(
                TAG,
                "Retrying Wi-Fi connection in %lu ms",
                (unsigned long)backoff_ms
            );

            vTaskDelay(pdMS_TO_TICKS(backoff_ms));

            if (wifi_mgr_is_connected()) {
                backoff_ms = WIFI_RECONNECT_INITIAL_MS;
                continue;
            }
        }

        ESP_LOGI(
            TAG,
            "Attempting Wi-Fi connection"
        );

        esp_err_t err = esp_wifi_connect();

        if (first_attempt) {
            first_attempt = false;
        } else {
            backoff_ms = increase_backoff(backoff_ms);
        }

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Connection request failed: %s",
                esp_err_to_name(err)
            );

            /*
             * Retry after the bounded delay on the next loop.
             */
            xTaskNotifyGive(s_reconnect_task);
        }
    }
}

/* =================================================
 * WIFI EVENT HANDLER
 * ================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {

        switch (event_id) {

            case WIFI_EVENT_STA_START:

                ESP_LOGI(
                    TAG,
                    "Wi-Fi station started"
                );

                if (s_reconnect_task != NULL) {
                    xTaskNotifyGive(s_reconnect_task);
                }

                break;

            case WIFI_EVENT_STA_DISCONNECTED:

                ESP_LOGW(
                    TAG,
                    "Wi-Fi disconnected"
                );

                xEventGroupClearBits(
                    s_wifi_event_group,
                    WIFI_CONNECTED_BIT
                );

                if (s_reconnect_task != NULL) {
                    xTaskNotifyGive(s_reconnect_task);
                }

                break;

            default:
                break;
        }
    }

    else if (
        event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {

        ESP_LOGI(
            TAG,
            "Wi-Fi connected and IP address obtained"
        );

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        if (s_reconnect_task != NULL) {
            xTaskNotifyGive(s_reconnect_task);
        }
    }
}

/* =================================================
 * WIFI INITIALIZATION
 * ================================================= */

esp_err_t wifi_mgr_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    /* ---------------------------------------------
     * 1. Validate Wi-Fi credentials
     * --------------------------------------------- */

    if (
        strlen(CONFIG_WIFI_SSID) == 0 ||
        strlen(CONFIG_WIFI_SSID) >= 32 ||
        strlen(CONFIG_WIFI_PASSWORD) >= 64
    ) {

        ESP_LOGE(
            TAG,
            "Invalid Wi-Fi credentials"
        );

        return ESP_ERR_INVALID_ARG;
    }

    /* ---------------------------------------------
     * 2. Initialize NVS
     * --------------------------------------------- */

    err = nvs_flash_init();

    if (
        err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {

        ESP_LOGW(
            TAG,
            "NVS recovery required; erasing NVS partition"
        );

        err = nvs_flash_erase();

        if (err != ESP_OK) {
            return err;
        }

        err = nvs_flash_init();
    }

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "NVS initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /* ---------------------------------------------
     * 3. Create event group
     * --------------------------------------------- */

    s_wifi_event_group = xEventGroupCreate();

    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* ---------------------------------------------
     * 4. Initialize TCP/IP stack
     * --------------------------------------------- */

    err = esp_netif_init();

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        return err;
    }

    /* ---------------------------------------------
     * 5. Create default event loop
     * --------------------------------------------- */

    err = esp_event_loop_create_default();

    if (
        err != ESP_OK &&
        err != ESP_ERR_INVALID_STATE
    ) {
        return err;
    }

    /* ---------------------------------------------
     * 6. Create Wi-Fi station interface
     * --------------------------------------------- */

    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (s_sta_netif == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create Wi-Fi station interface"
        );

        return ESP_FAIL;
    }

    /* ---------------------------------------------
     * 7. Initialize Wi-Fi driver
     * --------------------------------------------- */

    wifi_init_config_t wifi_init_cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&wifi_init_cfg);

    if (err != ESP_OK) {
        return err;
    }

    /* ---------------------------------------------
     * 8. Register event handlers
     * --------------------------------------------- */

    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        &s_wifi_handler
    );

    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL,
        &s_ip_handler
    );

    if (err != ESP_OK) {
        return err;
    }

    /* ---------------------------------------------
     * 9. Configure station credentials
     * --------------------------------------------- */

    wifi_config_t wifi_config = {0};

    memcpy(
        wifi_config.sta.ssid,
        CONFIG_WIFI_SSID,
        strlen(CONFIG_WIFI_SSID)
    );

    memcpy(
        wifi_config.sta.password,
        CONFIG_WIFI_PASSWORD,
        strlen(CONFIG_WIFI_PASSWORD)
    );

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);

    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    );

    if (err != ESP_OK) {
        return err;
    }

    /* ---------------------------------------------
     * 10. Create reconnect task
     * --------------------------------------------- */

    BaseType_t task_result = xTaskCreate(
        wifi_reconnect_task,
        "wifi_reconnect",
        WIFI_TASK_STACK_SIZE,
        NULL,
        WIFI_TASK_PRIORITY,
        &s_reconnect_task
    );

    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* ---------------------------------------------
     * 11. Start Wi-Fi
     * --------------------------------------------- */

    err = esp_wifi_start();

    if (err != ESP_OK) {
        return err;
    }

    /* ---------------------------------------------
     * 12. Initialize SNTP
     * --------------------------------------------- */

    initialize_sntp();

    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Wi-Fi manager initialized successfully"
    );

    return ESP_OK;
}

/* =================================================
 * PUBLIC ACCESSORS
 * ================================================= */

EventGroupHandle_t wifi_mgr_get_event_group(void)
{
    return s_wifi_event_group;
}

bool wifi_mgr_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(
        s_wifi_event_group
    );

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_mgr_is_time_synced(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(
        s_wifi_event_group
    );

    return (bits & TIME_SYNCED_BIT) != 0;
}

BaseType_t wifi_mgr_wait_for_time(
    TickType_t timeout_ticks
)
{
    if (s_wifi_event_group == NULL) {
        return pdFALSE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        TIME_SYNCED_BIT,
        pdFALSE,
        pdTRUE,
        timeout_ticks
    );

    return (bits & TIME_SYNCED_BIT)
        ? pdTRUE
        : pdFALSE;
}
