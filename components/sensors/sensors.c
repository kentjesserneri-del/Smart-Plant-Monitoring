#include "sensors.h"
#include "dht.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "sensors";

/* External functions from soil.c */
extern esp_err_t soil_init(void);
extern int soil_read_raw_averaged(void);
extern bool soil_raw_valid(int raw);
extern float soil_raw_to_pct(int raw);

esp_err_t sensors_init(void)
{
    ESP_LOGI(TAG, "initializing sensors");

    esp_err_t err = soil_init();
    if (err != ESP_OK) return err;

    /*
     * DHT22 has no separate init call in esp-idf-lib/dht v1.3.0.
     * The first dht_read_float_data() performs the start pulse.
     * GPIO13 is a normal output-capable GPIO (not input-only 34-39).
     */
    ESP_LOGI(TAG, "DHT22 configured on GPIO%d", SENSOR_DHT_GPIO);
    return ESP_OK;
}

esp_err_t sensors_read(sensor_reading_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    /* ---- DHT22 read ---- */
    float temp = 0.0f, rh = 0.0f;
    esp_err_t dht_err = dht_read_float_data(DHT_TYPE_AM2301, SENSOR_DHT_GPIO,
                                            &rh, &temp);
    out->dht_valid = (dht_err == ESP_OK);

    if (out->dht_valid) {
        /* Validate physical ranges (ROLE.txt: -40..80 C, 0..100 %RH) */
        if (temp < -40.0f || temp > 80.0f) {
            ESP_LOGW(TAG, "DHT22 temp %.1f out of range", temp);
            out->dht_valid = false;
        } else if (rh < 0.0f || rh > 100.0f) {
            ESP_LOGW(TAG, "DHT22 RH %.1f out of range", rh);
            out->dht_valid = false;
        } else {
            out->temp_c = temp;
            out->rh_pct = rh;
        }
    } else {
        ESP_LOGW(TAG, "DHT22 read failed: %s", esp_err_to_name(dht_err));
    }

    /* ---- Soil ADC read ---- */
    int raw = soil_read_raw_averaged();
    out->raw_adc = raw;
    out->soil_valid = soil_raw_valid(raw);

    if (out->soil_valid) {
        out->moisture_pct = soil_raw_to_pct(raw);
    } else {
        ESP_LOGW(TAG, "soil ADC invalid, moisture_pct not computed");
    }

    /* ---- Timestamp ---- */
    out->timestamp = time(NULL);

    /* ---- Overall validity ---- */
    out->valid = out->dht_valid && out->soil_valid;

    if (!out->valid) {
        ESP_LOGW(TAG, "reading INVALID (dht=%d soil=%d)",
                 out->dht_valid, out->soil_valid);
    }

    return ESP_OK;
}

void sensors_calibration_mode(void)
{
    ESP_LOGI(TAG, "=== CALIBRATION MODE ===");
    ESP_LOGI(TAG, "Record raw_dry (air/bone-dry) and raw_wet (saturated).");
    ESP_LOGI(TAG, "Press Ctrl+] to exit.");

    for (;;) {
        int raw = soil_read_raw_averaged();
        if (raw >= 0) {
            ESP_LOGI(TAG, "raw_adc = %d  (valid=%d)",
                     raw, soil_raw_valid(raw));
        } else {
            ESP_LOGE(TAG, "ADC read failed in calibration mode");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}