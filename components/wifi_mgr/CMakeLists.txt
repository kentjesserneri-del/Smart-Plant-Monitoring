
idf_component_register(
    SRCS
        "wifi_mgr.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "../../"

    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        lwip
)
