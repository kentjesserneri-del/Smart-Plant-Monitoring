#pragma once

#include <stdbool.h>
#include "sensors.h"

/* Event types produced by the monitor */
typedef enum {
    EVENT_NONE = 0,
    EVENT_INITIAL_REPORT,
    EVENT_ALERT_DRY,
    EVENT_ALERT_WET,
    EVENT_ALERT_FAULT,
    EVENT_RECOVERY
} plant_event_type_t;

/* Structure sent to the notify_task */
typedef struct {
    plant_event_type_t type;
    char message_reason[64];
} plant_event_t;

/**
 * @brief Initialize the monitoring logic (thresholds, state).
 */
void monitor_init(void);

/**
 * @brief Evaluate a new sensor reading.
 * @param reading Pointer to the latest valid/invalid sensor reading.
 * @param out_event Pointer to store the generated event (if any).
 * @return true if an event was generated, false otherwise.
 */
bool monitor_evaluate(const sensor_reading_t *reading, plant_event_t *out_event);

/**
 * @brief Force an event bypass (used for demo triggers).
 */
void monitor_trigger_demo(void);