
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/*
 * ESP-IDF v5.x
 *
 * Event-group flags:
 *
 * WIFI_CONNECTED_BIT:
 *     ESP32 successfully obtained an IP address.
 *
 * TIME_SYNCED_BIT:
 *     SNTP synchronized the system clock.
 */

#define WIFI_CONNECTED_BIT (1U << 0)
#define TIME_SYNCED_BIT    (1U << 1)

/**
 * @brief Initialize Wi-Fi station mode and SNTP.
 *
 * This function starts Wi-Fi asynchronously.
 * It does not wait for a successful connection.
 *
 * NVS initialization is handled internally.
 *
 * @return ESP_OK on successful initialization.
 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief Get the Wi-Fi manager event group.
 */
EventGroupHandle_t wifi_mgr_get_event_group(void);

/**
 * @brief Check whether Wi-Fi is connected.
 */
bool wifi_mgr_is_connected(void);

/**
 * @brief Check whether system time is synchronized.
 */
bool wifi_mgr_is_time_synced(void);

/**
 * @brief Wait for SNTP synchronization.
 *
 * @param timeout_ticks Maximum waiting time.
 * @return pdTRUE if time was synchronized.
 */
BaseType_t wifi_mgr_wait_for_time(TickType_t timeout_ticks);

#endif
