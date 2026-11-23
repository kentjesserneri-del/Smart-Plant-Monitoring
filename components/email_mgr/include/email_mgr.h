
#ifndef EMAIL_MGR_H
#define EMAIL_MGR_H

#include "esp_err.h"
#include "monitoring.h"
#include "sensors.h"
#include <stdbool.h>

/*
 * Initialize the Gmail notification manager.
 * Creates a FreeRTOS queue and email task.
 */
esp_err_t email_mgr_init(void);

/*
 * Queue a monitoring event for Gmail notification.
 * This does not send email from the sensor task itself.
 */
bool email_mgr_send_event(
    const plant_event_t *event,
    const sensor_reading_t *reading
);

/* Queue a one-time test email. */
bool email_mgr_send_test(void);

#endif
