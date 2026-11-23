
#include "system_time.h"

#include <time.h>

#include "esp_log.h"

static const char *TAG = "system_time";

/*
 * Display synchronized Philippine date and time.
 */
void display_system_time(void)
{
    time_t now;
    struct tm local_time;

    time(&now);

    localtime_r(&now, &local_time);

    char date_buffer[20];
    char time_buffer[20];

    strftime(
        date_buffer,
        sizeof(date_buffer),
        "%Y-%m-%d",
        &local_time
    );

    strftime(
        time_buffer,
        sizeof(time_buffer),
        "%H:%M:%S",
        &local_time
    );

    ESP_LOGI(TAG, "Current Philippine Date: %s", date_buffer);
    ESP_LOGI(TAG, "Current Philippine Time: %s", time_buffer);
}
