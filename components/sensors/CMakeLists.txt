idf_component_register(
    SRCS "sensors.c" "soil.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc esp_timer esp-tls
)