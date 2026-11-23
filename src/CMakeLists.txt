
idf_component_register(
    SRCS
        "main.c"
        "sensor_task.c"
        "system_time.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        sensors
        monitoring
        wifi_mgr
        mqtt_mgr
        discord_mgr
        email_mgr
        callmebot_mgr
        mastodon_mgr
        notification_sync
)
