
#include "sensors.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "potentiometer";

static adc_oneshot_unit_handle_t adc_handle = NULL;

#define POT_SAMPLES 32
#define POT_ADC_MAX 4095

esp_err_t soil_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = SENSOR_SOIL_ADC_UNIT,
    };

    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &adc_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC initialization failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    err = adc_oneshot_config_channel(
        adc_handle,
        SENSOR_SOIL_ADC_CHAN,
        &chan_cfg
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel configuration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Potentiometer configured on GPIO34 (ADC1_CH6)");

    return ESP_OK;
}

int soil_read_raw_averaged(void)
{
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC handle is not initialized");
        return -1;
    }

    int sum = 0;

    for (int i = 0; i < POT_SAMPLES; i++) {
        int raw = 0;

        esp_err_t err = adc_oneshot_read(
            adc_handle,
            SENSOR_SOIL_ADC_CHAN,
            &raw
        );

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s",
                     esp_err_to_name(err));
            return -1;
        }

        sum += raw;

        esp_rom_delay_us(100);
    }

    return sum / POT_SAMPLES;
}

bool soil_raw_valid(int raw)
{
    /*
     * Potentiometer can legitimately produce readings
     * near 0 and near 4095.
     */
    return (raw >= 0 && raw <= POT_ADC_MAX);
}

float soil_raw_to_pct(int raw)
{
    if (raw < 0) {
        return 0.0f;
    }

    if (raw > POT_ADC_MAX) {
        raw = POT_ADC_MAX;
    }

    /*
     * Simulation mapping:
     * 0    = 0% moisture
     * 2048 = approximately 50%
     * 4095 = 100% moisture
     */
    float moisture = ((float)raw / POT_ADC_MAX) * 100.0f;

    if (moisture < 0.0f) {
        moisture = 0.0f;
    }

    if (moisture > 100.0f) {
        moisture = 100.0f;
    }

    return moisture;
}
