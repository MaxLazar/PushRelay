// PushRelay v0.5 — oleg@abramov.dev
// Static configuration, committed to git. Runtime-editable values live in NVS (see webadmin.h).
#pragma once

#define APP_NAME      "PushRelay"
#define APP_VERSION   "0.5"
#define APP_AUTHOR    "Oleg Abramov"
#define APP_EMAIL     "oleg@abramov.dev"
#define MDNS_HOSTNAME "pushrelay" // http://pushrelay.local

// BLE identity advertised to iOS/macOS.
#define BLE_DEVICE_NAME "PushRelay"

// WiFiManager captive-portal AP name used on first boot / lost credentials.
#define WIFI_SETUP_AP_NAME "PushRelay-Setup"
#define WIFI_SETUP_TIMEOUT_SEC 180

// Notifier selection.
#define NOTIFIER_BARK 0
#define NOTIFIER_PUSHOVER 1
#define NOTIFIER_MQTT 2

#define ACTIVE_NOTIFIER NOTIFIER_BARK
