#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "monitoring.h"
#include "sensors.h"

esp_err_t callmebot_mgr_init(void);

bool callmebot_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
);

bool callmebot_mgr_send_test(void);