
#include "notification_sync.h"

#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "NOTIFICATION_SYNC";

static SemaphoreHandle_t s_notification_mutex = NULL;

esp_err_t notification_sync_init(void)
{
    if (s_notification_mutex != NULL) {
        return ESP_OK;
    }

    s_notification_mutex = xSemaphoreCreateMutex();

    if (s_notification_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create notification mutex.");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Shared notification mutex initialized.");
    return ESP_OK;
}

bool notification_sync_acquire(TickType_t timeout)
{
    if (s_notification_mutex == NULL) {
        ESP_LOGE(TAG, "Notification mutex is not initialized.");
        return false;
    }

    if (xSemaphoreTake(s_notification_mutex, timeout) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for notification connection lock.");
        return false;
    }

    ESP_LOGI(TAG, "Notification connection lock acquired.");
    return true;
}

void notification_sync_release(void)
{
    if (s_notification_mutex != NULL) {
        xSemaphoreGive(s_notification_mutex);
        ESP_LOGI(TAG, "Notification connection lock released.");
    }
}
