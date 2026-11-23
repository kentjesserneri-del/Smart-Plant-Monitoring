#pragma once

// ---- MQTT Broker ----
#define CONFIG_MQTT_BROKER_URI      "mqtts://broker.mqtty.com:8883"
#define CONFIG_MQTT_USERNAME        "YOUR_MQTT_USERNAME"
#define CONFIG_MQTT_PASSWORD        "YOUR_MQTT_PASSWORD"
#define CONFIG_MQTT_CLIENT_ID       "bca152-plant01-esp32"

// ---- MQTT Topics ----
#define CONFIG_MQTT_TOPIC_TEMP      "bca152/plant01/temperature"
#define CONFIG_MQTT_TOPIC_HUMIDITY  "bca152/plant01/humidity"
#define CONFIG_MQTT_TOPIC_MOISTURE  "bca152/plant01/moisture"
#define CONFIG_MQTT_TOPIC_STATUS    "bca152/plant01/status"
// ---- Wi-Fi ----
#define CONFIG_WIFI_SSID         "YOUR_WIFI_SSID"
#define CONFIG_WIFI_PASSWORD     "YOUR_WIFI_PASSWORD"

// ---- Discord Webhook (replaces Telegram) ----
// Get it from Discord: Channel Settings → Integrations → Webhooks → New Webhook → Copy URL
// WARNING: The URL itself is the credential. Keep it secret.
#define CONFIG_DISCORD_WEBHOOK_URL "https://discord.com/api/webhooks/YOUR_WEBHOOK_ID/YOUR_WEBHOOK_TOKEN"

// ---- Gmail SMTP ----
// Requires 2-Step Verification + App Password
#define CONFIG_GMAIL_SMTP_HOST   "smtp.gmail.com"
#define CONFIG_GMAIL_SMTP_PORT   465
#define CONFIG_GMAIL_USER        "YOUR_EMAIL@example.com"
#define CONFIG_GMAIL_APP_PASS    "YOUR_GMAIL_APP_PASSWORD"
#define CONFIG_GMAIL_TO          "YOUR_EMAIL@example.com"

// ---- WhatsApp (CallMeBot fallback, simpler & free) ----
// Send "I allow callmebot to send me messages" to the CallMeBot number on WhatsApp
#define CONFIG_CALLMEBOT_PHONE   "YOUR_PHONE_WITH_COUNTRY_CODE"
#define CONFIG_CALLMEBOT_API_KEY "YOUR_CALLMEBOT_API_KEY"

// ---- Mastodon (replaces X) ----
// Settings → Development → New Application → scope: write:statuses
#define CONFIG_MASTODON_INSTANCE  "https://mastodon.social"
#define CONFIG_MASTODON_TOKEN     "YOUR_MASTODON_TOKEN"