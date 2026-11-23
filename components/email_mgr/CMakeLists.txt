
idf_component_register(
    SRCS "email_mgr.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        monitoring
        sensors
        esp-tls
        mbedtls
        notification_sync
)
