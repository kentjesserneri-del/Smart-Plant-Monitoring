
#include "monitoring.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "monitoring";

/* Monitoring configuration */
#define DRY_THRESHOLD_PCT   30.0f
#define WET_THRESHOLD_PCT   75.0f
#define HYSTERESIS_PCT      5.0f

/* Two consecutive readings for faster detection */
#define CONFIRM_SAMPLES     2

/* Zero cooldown: confirmed state changes can notify immediately */
#define COOLDOWN_SECONDS    0

typedef enum {
    STATE_UNKNOWN = 0,
    STATE_NORMAL,
    STATE_DRY,
    STATE_WET,
    STATE_FAULT
} plant_state_t;

static plant_state_t current_state = STATE_UNKNOWN;
static plant_state_t candidate_state = STATE_UNKNOWN;

static int candidate_count = 0;
static int64_t last_notification_time = 0;

static bool initial_report_sent = false;
static bool demo_trigger_flag = false;

void monitor_init(void)
{
    current_state = STATE_UNKNOWN;
    candidate_state = STATE_UNKNOWN;
    candidate_count = 0;

    last_notification_time = 0;
    initial_report_sent = false;
    demo_trigger_flag = false;

    ESP_LOGI(
        TAG,
        "Monitor initialized. Dry < %.1f%%, Wet > %.1f%%",
        DRY_THRESHOLD_PCT,
        WET_THRESHOLD_PCT
    );

    ESP_LOGI(
        TAG,
        "Confirmation samples: %d | Notification cooldown: %d seconds",
        CONFIRM_SAMPLES,
        COOLDOWN_SECONDS
    );
}

void monitor_trigger_demo(void)
{
    demo_trigger_flag = true;

    ESP_LOGI(
        TAG,
        "Demo trigger activated (bypasses cooldown)"
    );
}

/*
 * Determine the state of one sensor reading using hysteresis.
 */
static plant_state_t classify_reading(
    const sensor_reading_t *reading
)
{
    if (!reading->valid) {
        return STATE_FAULT;
    }

    float moisture = reading->moisture_pct;

    if (current_state == STATE_NORMAL) {

        if (moisture < (DRY_THRESHOLD_PCT - HYSTERESIS_PCT)) {
            return STATE_DRY;
        }

        if (moisture > (WET_THRESHOLD_PCT + HYSTERESIS_PCT)) {
            return STATE_WET;
        }

    } else if (current_state == STATE_DRY) {

        /*
         * Recover from DRY when moisture rises above 35%.
         */
        if (moisture > (DRY_THRESHOLD_PCT + HYSTERESIS_PCT)) {
            return STATE_NORMAL;
        }

    } else if (current_state == STATE_WET) {

        /*
         * Recover from WET when moisture drops below 70%.
         */
        if (moisture < (WET_THRESHOLD_PCT - HYSTERESIS_PCT)) {
            return STATE_NORMAL;
        }

    } else {

        /*
         * Unknown or FAULT:
         * Establish the initial state using raw thresholds.
         */
        if (moisture < DRY_THRESHOLD_PCT) {
            return STATE_DRY;
        }

        if (moisture > WET_THRESHOLD_PCT) {
            return STATE_WET;
        }

        return STATE_NORMAL;
    }

    return current_state;
}

/*
 * Check whether an event should be emitted.
 */
static bool should_emit(plant_event_type_t type)
{
    /*
     * Demo trigger bypasses cooldown.
     */
    if (demo_trigger_flag) {
        demo_trigger_flag = false;
        return true;
    }

    /*
     * Initial status report is sent once.
     */
    if (type == EVENT_INITIAL_REPORT) {
        return !initial_report_sent;
    }

    /*
     * Recovery notifications are never blocked by cooldown.
     */
    if (type == EVENT_RECOVERY) {
        return true;
    }

    /*
     * Cooldown is configurable.
     * Currently zero, allowing confirmed state changes immediately.
     */
    if (COOLDOWN_SECONDS > 0) {

        int64_t now = esp_timer_get_time() / 1000000;

        if (
            last_notification_time != 0 &&
            (now - last_notification_time) < COOLDOWN_SECONDS
        ) {
            ESP_LOGD(
                TAG,
                "Cooldown active. Skipping event type %d",
                type
            );

            return false;
        }
    }

    return true;
}

bool monitor_evaluate(
    const sensor_reading_t *reading,
    plant_event_t *out_event
)
{
    if (reading == NULL || out_event == NULL) {
        return false;
    }

    plant_state_t observed_state = classify_reading(reading);

    /*
     * State confirmation:
     * The same observed state must appear twice consecutively.
     */
    if (observed_state == candidate_state) {
        candidate_count++;
    } else {
        candidate_state = observed_state;
        candidate_count = 1;
    }

    /*
     * Wait until the candidate state is confirmed.
     */
    if (
        candidate_count < CONFIRM_SAMPLES ||
        candidate_state == current_state
    ) {
        return false;
    }

    plant_state_t previous_state = current_state;

    current_state = candidate_state;
    candidate_count = 0;

    plant_event_type_t event_type = EVENT_NONE;

    /*
     * Determine the event.
     */
    if (
        !initial_report_sent &&
        current_state != STATE_FAULT &&
        previous_state == STATE_UNKNOWN
    ) {
        event_type = EVENT_INITIAL_REPORT;

        snprintf(
            out_event->message_reason,
            sizeof(out_event->message_reason),
            "Initial Status Report"
        );
    }
    else if (current_state == STATE_FAULT) {

        event_type = EVENT_ALERT_FAULT;

        snprintf(
            out_event->message_reason,
            sizeof(out_event->message_reason),
            "Sensor Fault Detected"
        );
    }
    else if (current_state == STATE_DRY) {

        event_type = EVENT_ALERT_DRY;

        snprintf(
            out_event->message_reason,
            sizeof(out_event->message_reason),
            "Soil is too DRY"
        );
    }
    else if (current_state == STATE_WET) {

        event_type = EVENT_ALERT_WET;

        snprintf(
            out_event->message_reason,
            sizeof(out_event->message_reason),
            "Soil is too WET"
        );
    }
    else if (
        current_state == STATE_NORMAL &&
        (
            previous_state == STATE_DRY ||
            previous_state == STATE_WET ||
            previous_state == STATE_FAULT
        )
    ) {
        event_type = EVENT_RECOVERY;

        snprintf(
            out_event->message_reason,
            sizeof(out_event->message_reason),
            "Recovery to NORMAL"
        );
    }

    /*
     * Mark the initial report as handled after the first
     * confirmed valid state, even if that state follows a fault.
     */
    if (current_state != STATE_FAULT) {
        initial_report_sent = true;
    }

    /*
     * Send the confirmed event.
     */
    if (
        event_type != EVENT_NONE &&
        should_emit(event_type)
    ) {
        out_event->type = event_type;

        last_notification_time =
            esp_timer_get_time() / 1000000;

        ESP_LOGI(
            TAG,
            "EVENT GENERATED: Type=%d, Reason=%s",
            event_type,
            out_event->message_reason
        );

        return true;
    }

    return false;
}
