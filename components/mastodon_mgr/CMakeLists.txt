idf_component_register(
    SRCS "mastodon_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES monitoring sensors esp_http_client esp-tls notification_sync
)