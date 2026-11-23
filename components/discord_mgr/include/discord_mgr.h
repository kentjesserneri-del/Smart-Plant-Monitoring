
#pragma once

#include "esp_err.h"
#include "monitoring.h"
#include "sensors.h"
#include <stdbool.h>

/* Initialize the Discord notification queue and task. */
esp_err_t discord_mgr_init(void);

/* Queue a one-time Discord connectivity test. */
bool discord_mgr_send_test(void);

/* Queue a plant event for Discord delivery. */
bool discord_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
);
