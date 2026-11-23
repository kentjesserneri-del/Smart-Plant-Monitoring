
idf_component_register(
    SRCS
        "mqtt_mgr.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        mqtt
        sensors
)
