// PushRelay v0.5 — oleg@abramov.dev
// Static configuration, committed to git. Runtime-editable values live in NVS (see webadmin.h).
#pragma once

#define APP_NAME      "PushRelay"
#define APP_VERSION   "0.6"
#define APP_AUTHOR    "Oleg Abramov"
#define APP_EMAIL     "oleg@abramov.dev"
#define MDNS_HOSTNAME "pushrelay" // http://pushrelay.local

// BLE identity advertised to iOS/macOS.
#define BLE_DEVICE_NAME "PushRelay"

// Over-the-air self-update (admin UI "Firmware update" card). The device pulls
// a small JSON manifest plus the firmware/filesystem images from GitHub Pages
// (published by .github/workflows/flasher.yml on every firmware-affecting push).
// Pages is used instead of the GitHub API so there is no rate limit and no
// cross-host redirect; image integrity is checked with the SHA-256 in the
// manifest, so a plain TLS connection (no pinned CA) is acceptable here.
// NB: this is the OTA manifest (update.json). The esp-web-tools web flasher
// uses a separate install.json with a different schema.
#define OTA_BASE_URL     "https://maxlazar.github.io/PushRelay/"
#define OTA_MANIFEST_URL OTA_BASE_URL "update.json"

// WiFiManager captive-portal AP name used on first boot / lost credentials.
#define WIFI_SETUP_AP_NAME "PushRelay-Setup"
#define WIFI_SETUP_TIMEOUT_SEC 180

// Notifier selection.
#define NOTIFIER_BARK 0
#define NOTIFIER_PUSHOVER 1
#define NOTIFIER_MQTT 2

#define ACTIVE_NOTIFIER NOTIFIER_BARK
