
#ifndef MQTT_MGR_H
#define MQTT_MGR_H

#include "esp_err.h"
#include "sensors.h"
#include <stdbool.h>

esp_err_t mqtt_mgr_init(void);

/*
 * Copies a sensor reading into a bounded queue.
 * Does not wait for network communication.
 */
bool mqtt_mgr_publish_reading(const sensor_reading_t *reading);

bool mqtt_mgr_is_connected(void);

#endif
