#include "sensors.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "soil";

static adc_oneshot_unit_handle_t adc_handle = NULL;

/* Number of samples averaged per reading (ROLE.txt: 16-32). */
#define SOIL_SAMPLES 32

/* Reject readings near 0 or near full-scale: likely disconnected sensor. */
#define SOIL_ADC_MIN_VALID 100
#define SOIL_ADC_MAX_VALID 4000

esp_err_t soil_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = SENSOR_SOIL_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_11,   /* 0-3.3 V range */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(adc_handle, SENSOR_SOIL_ADC_CHAN, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ADC1_CH6 (GPIO34) configured, atten=11dB");
    return ESP_OK;
}

/**
 * @brief Average SOIL_SAMPLES raw ADC readings.
 * @return Averaged raw value, or -1 on ADC error.
 */
int soil_read_raw_averaged(void)
{
    int sum = 0;
    for (int i = 0; i < SOIL_SAMPLES; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, SENSOR_SOIL_ADC_CHAN, &raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
            return -1;
        }
        sum += raw;
        /* Small delay between samples to reduce correlated noise. */
        esp_rom_delay_us(100);
    }
    return sum / SOIL_SAMPLES;
}

/**
 * @brief Validate raw ADC: reject near-0 / near-full-scale.
 */
bool soil_raw_valid(int raw)
{
    if (raw < 0) return false;
    if (raw < SOIL_ADC_MIN_VALID || raw > SOIL_ADC_MAX_VALID) {
        ESP_LOGW(TAG, "ADC raw %d outside valid range [%d, %d]",
                 raw, SOIL_ADC_MIN_VALID, SOIL_ADC_MAX_VALID);
        return false;
    }
    return true;
}

/**
 * @brief Convert raw ADC to moisture percentage.
 *
 * Formula (ROLE.txt): clamp((raw_dry - raw) * 100 / (raw_dry - raw_wet), 0, 100)
 * Capacitive sensors: HIGHER raw = DRY, LOWER raw = WET.
 */
float soil_raw_to_pct(int raw)
{
    if (CONFIG_SOIL_RAW_DRY == CONFIG_SOIL_RAW_WET) {
        ESP_LOGE(TAG, "calibration invalid: raw_dry == raw_wet");
        return 0.0f;
    }

    float pct = (float)(CONFIG_SOIL_RAW_DRY - raw) * 100.0f
              / (float)(CONFIG_SOIL_RAW_DRY - CONFIG_SOIL_RAW_WET);

    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}