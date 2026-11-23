
idf_component_register(
    SRCS
        "main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        sensors
        monitoring
        wifi_mgr
        mqtt_mgr
)
