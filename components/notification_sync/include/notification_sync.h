
#ifndef NOTIFICATION_SYNC_H
#define NOTIFICATION_SYNC_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t notification_sync_init(void);

bool notification_sync_acquire(TickType_t timeout);

void notification_sync_release(void);

#endif
