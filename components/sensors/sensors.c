#include "sensors.h"

#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <time.h>
#include <stdint.h>

static const char *TAG = "sensors";

/* Functions provided by soil.c */
extern esp_err_t soil_init(void);
extern int soil_read_raw_averaged(void);
extern bool soil_raw_valid(int raw);
extern float soil_raw_to_pct(int raw);

/* DHT11 timing configuration */
#define DHT_START_LOW_US       20000U
#define DHT_RELEASE_HIGH_US    30U
#define DHT_RESPONSE_TIMEOUT  200U
#define DHT_BIT_TIMEOUT       100U
#define DHT_BIT_THRESHOLD_US   40U
#define DHT_MAX_ATTEMPTS        3

static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

/* =========================================================
   DHT11 GPIO INITIALIZATION
   ========================================================= */

static esp_err_t dht_init(void)
{
    esp_err_t err = gpio_reset_pin(SENSOR_DHT_GPIO);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_direction(SENSOR_DHT_GPIO, GPIO_MODE_INPUT);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_pull_mode(SENSOR_DHT_GPIO, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Native DHT11 initialized on GPIO%d",
             SENSOR_DHT_GPIO);

    ESP_LOGI(TAG, "DHT11 idle DATA level: %d",
             gpio_get_level(SENSOR_DHT_GPIO));

    return ESP_OK;
}

/* =========================================================
   WAIT FOR GPIO LEVEL WITH MICROSECOND TIMEOUT
   ========================================================= */

static bool dht_wait_level(
    int expected_level,
    uint32_t timeout_us,
    uint32_t ticks_per_us)
{
    uint32_t start = esp_cpu_get_cycle_count();
    uint32_t timeout_cycles = timeout_us * ticks_per_us;

    while (gpio_get_level(SENSOR_DHT_GPIO) != expected_level) {

        uint32_t elapsed =
            esp_cpu_get_cycle_count() - start;

        if (elapsed >= timeout_cycles) {
            return false;
        }
    }

    return true;
}

/* =========================================================
   READ ONE COMPLETE DHT11 FRAME
   ========================================================= */

static bool dht_read_once(float *temperature, float *humidity)
{
    if (temperature == NULL || humidity == NULL) {
        return false;
    }

    uint8_t data[5] = {0};

    uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();

    if (ticks_per_us == 0) {
        ESP_LOGE(TAG, "Unable to determine CPU ticks per microsecond");
        return false;
    }

    int response_failure = 0;
    bool success = true;

    /*
     * STEP 1:
     * ESP32 sends the DHT11 start signal.
     */

    gpio_set_direction(SENSOR_DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_DHT_GPIO, 0);

    esp_rom_delay_us(DHT_START_LOW_US);

    /*
     * STEP 2:
     * Release the DATA line.
     * Interrupts are disabled during the timing-critical response.
     */

    portENTER_CRITICAL(&dht_mux);

    gpio_set_direction(SENSOR_DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_DHT_GPIO, 1);

    esp_rom_delay_us(DHT_RELEASE_HIGH_US);

    gpio_set_direction(SENSOR_DHT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SENSOR_DHT_GPIO, GPIO_PULLUP_ONLY);

    /*
     * STEP 3:
     * Detect the DHT11 response:
     * LOW -> HIGH -> LOW
     */

    if (!dht_wait_level(
            0,
            DHT_RESPONSE_TIMEOUT,
            ticks_per_us)) {

        response_failure = 1;
        success = false;

    } else if (!dht_wait_level(
                   1,
                   DHT_RESPONSE_TIMEOUT,
                   ticks_per_us)) {

        response_failure = 2;
        success = false;

    } else if (!dht_wait_level(
                   0,
                   DHT_RESPONSE_TIMEOUT,
                   ticks_per_us)) {

        response_failure = 3;
        success = false;
    }

    /*
     * STEP 4:
     * Read all 40 bits from the DHT11.
     */

    for (int bit = 0; success && bit < 40; bit++) {

        /* Wait for the beginning of the HIGH pulse. */

        if (!dht_wait_level(
                1,
                DHT_BIT_TIMEOUT,
                ticks_per_us)) {

            success = false;
            break;
        }

        uint32_t high_start = esp_cpu_get_cycle_count();

        /* Wait for the HIGH pulse to finish. */

        if (!dht_wait_level(
                0,
                DHT_BIT_TIMEOUT,
                ticks_per_us)) {

            success = false;
            break;
        }

        uint32_t high_cycles =
            esp_cpu_get_cycle_count() - high_start;

        uint32_t high_width_us =
            high_cycles / ticks_per_us;

        /* Shift the received bit into its corresponding byte. */

        data[bit / 8] <<= 1;

        if (high_width_us > DHT_BIT_THRESHOLD_US) {
            data[bit / 8] |= 1;
        }
    }

    /* Always restore the DATA pin to input mode. */

    gpio_set_direction(SENSOR_DHT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SENSOR_DHT_GPIO, GPIO_PULLUP_ONLY);

    portEXIT_CRITICAL(&dht_mux);

    /*
     * STEP 5:
     * Report the exact response failure.
     */

    if (!success) {

        if (response_failure == 1) {
            ESP_LOGW(TAG, "DHT response LOW missing on GPIO%d",
                     SENSOR_DHT_GPIO);

        } else if (response_failure == 2) {
            ESP_LOGW(TAG, "DHT response HIGH missing on GPIO%d",
                     SENSOR_DHT_GPIO);

        } else if (response_failure == 3) {
            ESP_LOGW(TAG, "DHT response LOW end missing on GPIO%d",
                     SENSOR_DHT_GPIO);

        } else {
            ESP_LOGW(TAG, "DHT data pulse timeout on GPIO%d",
                     SENSOR_DHT_GPIO);
        }

        return false;
    }

    /*
     * STEP 6:
     * Validate the checksum.
     */

    uint8_t checksum =
        (uint8_t)(data[0] + data[1] + data[2] + data[3]);

    if (checksum != data[4]) {

        ESP_LOGW(
            TAG,
            "DHT checksum error: received=%u expected=%u",
            data[4],
            checksum
        );

        return false;
    }

    /* Reject an empty frame. */

    if (data[0] == 0 &&
        data[1] == 0 &&
        data[2] == 0 &&
        data[3] == 0 &&
        data[4] == 0) {

        ESP_LOGW(TAG, "DHT returned an empty frame");
        return false;
    }

    /*
     * STEP 7:
     * Convert DHT11 data bytes.
     *
     * Byte 0 = Humidity integer
     * Byte 1 = Humidity decimal
     * Byte 2 = Temperature integer
     * Byte 3 = Temperature decimal
     * Byte 4 = Checksum
     */

    *humidity =
        (float)data[0] + ((float)data[1] / 10.0f);

    *temperature =
        (float)data[2] + ((float)data[3] / 10.0f);

    if (*temperature < 0.0f || *temperature > 50.0f ||
        *humidity < 0.0f || *humidity > 100.0f) {

        ESP_LOGW(TAG, "DHT values outside expected range");
        return false;
    }

    ESP_LOGI(
        TAG,
        "Native DHT11: %.1f C | Humidity: %.1f %%",
        *temperature,
        *humidity
    );

    return true;
}

/* =========================================================
   DHT11 READ WITH RETRIES
   ========================================================= */

static bool dht_read(float *temperature, float *humidity)
{
    for (int attempt = 0; attempt < DHT_MAX_ATTEMPTS; attempt++) {

        if (dht_read_once(temperature, humidity)) {

            if (attempt > 0) {
                ESP_LOGI(TAG, "DHT11 succeeded on retry %d",
                         attempt + 1);
            }

            return true;
        }

        ESP_LOGW(
            TAG,
            "DHT11 attempt %d/%d failed",
            attempt + 1,
            DHT_MAX_ATTEMPTS
        );

        if (attempt < DHT_MAX_ATTEMPTS - 1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGE(TAG, "DHT11 failed after all attempts");

    return false;
}

/* =========================================================
   INITIALIZE ALL SENSORS
   ========================================================= */

esp_err_t sensors_init(void)
{
    ESP_LOGI(TAG, "Initializing sensors");

    /* Initialize potentiometer ADC on GPIO34. */

    esp_err_t err = soil_init();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Potentiometer initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /* Initialize native DHT11 on GPIO13. */

    err = dht_init();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DHT11 initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(TAG, "Potentiometer configured on GPIO34");
    ESP_LOGI(TAG, "DHT11 configured on GPIO13");

    return ESP_OK;
}

/* =========================================================
   READ ALL SENSORS
   ========================================================= */

esp_err_t sensors_read(sensor_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    /* ---- DHT11 temperature and humidity ---- */

    float temperature = 0.0f;
    float humidity = 0.0f;

    out->dht_valid = dht_read(&temperature, &humidity);

    if (out->dht_valid) {

        out->temp_c = temperature;
        out->rh_pct = humidity;

    } else {

        ESP_LOGW(TAG, "DHT11 reading unavailable");
    }

    /* ---- Potentiometer / simulated soil moisture ---- */

    int raw = soil_read_raw_averaged();

    out->raw_adc = raw;
    out->soil_valid = soil_raw_valid(raw);

    if (out->soil_valid) {

        out->moisture_pct = soil_raw_to_pct(raw);

        ESP_LOGI(
            TAG,
            "Potentiometer: ADC=%d | Simulated Moisture=%.1f%%",
            raw,
            out->moisture_pct
        );

    } else {

        ESP_LOGW(TAG, "Potentiometer ADC invalid");
    }

    /* ---- Timestamp ---- */

    out->timestamp = time(NULL);

    /* ---- Overall validity ---- */

    out->valid = out->dht_valid && out->soil_valid;

    if (!out->valid) {

        ESP_LOGW(
            TAG,
            "Reading INVALID (DHT11=%d | Potentiometer=%d)",
            out->dht_valid,
            out->soil_valid
        );
    }

    return ESP_OK;
}

/* =========================================================
   POTENTIOMETER CALIBRATION MODE
   ========================================================= */

void sensors_calibration_mode(void)
{
    ESP_LOGI(TAG, "=== POTENTIOMETER CALIBRATION MODE ===");
    ESP_LOGI(TAG, "Turn the potentiometer and observe ADC values.");

    for (;;) {

        int raw = soil_read_raw_averaged();

        if (raw >= 0) {

            ESP_LOGI(
                TAG,
                "Raw ADC = %d | Valid = %d",
                raw,
                soil_raw_valid(raw)
            );

        } else {

            ESP_LOGE(TAG, "ADC read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}