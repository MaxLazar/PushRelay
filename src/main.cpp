// PushRelay v0.5 — oleg@abramov.dev
// Entry point: brings up WiFi, mDNS, the ANCS BLE handler, the notification
// pipeline (filter/DND/priority/multi-recipient), the web admin UI, and the
// Phase 2 reliability features (watchdog, LED, OTA).
#include <Arduino.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <esp_coexist.h>
#include <time.h>

#include "config.h"
#include "secrets.h"
#include "version.h"
#include "notifiers.h"
#include "ancs.h"
#include "webadmin.h"
#include "led.h"
#include "stats.h"
#include "notiflog.h"
#include "improv.h"

// Reboot if the BLE link to the phone stays down this long — recovers from a
// stuck BLE/ANCS stack without needing a manual power cycle.
static const uint32_t BLE_RECOVERY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
static const uint32_t WATCHDOG_TIMEOUT_SEC = 300;

static AncsManager ancs;
static WebAdmin webAdmin;
static LedIndicator led;
static Stats stats;
static NotificationLog notifLog;
static ImprovSerial improv;
static uint32_t lastBleOkMillis = 0;

// Returns true if the STA interface already has WiFi credentials stored in NVS
// (i.e. this isn't a fresh device) — used to let stored credentials win the
// boot race before we offer the Improv-serial provisioning form.
static bool hasStoredWifiCredentials() {
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
    return conf.sta.ssid[0] != '\0';
}

// Brings WiFi up. Tries stored credentials first; while that races, services
// Improv-serial so the browser flasher can push credentials over USB and get a
// clickable device URL back. Falls back to the WiFiManager captive portal only
// if neither path connects.
static void connectWifi() {
    improv.begin(APP_NAME, FIRMWARE_VERSION, BLE_DEVICE_NAME);

    WiFi.mode(WIFI_STA);
    const bool haveCreds = hasStoredWifiCredentials();
    if (haveCreds) WiFi.begin();   // reconnect with NVS-stored credentials

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        const uint32_t waited = millis() - start;
        // Give stored credentials a ~5s head start before answering Improv
        // state queries (which would otherwise pop the "enter WiFi" form in
        // the flasher even though we're seconds from connecting).
        if (!haveCreds || waited > 5000) improv.loop();

        if (improv.isConnected()) return;                    // provisioned over serial
        if (!improv.sawTraffic() && waited > (haveCreds ? 12000 : 6000)) break;
        if (waited > 90000) break;                           // hard cap
        delay(10);
    }

    if (WiFi.status() != WL_CONNECTED) {
        WiFiManager wm;
        wm.setConfigPortalTimeout(WIFI_SETUP_TIMEOUT_SEC);
        if (!wm.autoConnect(WIFI_SETUP_AP_NAME)) {
            Serial.printf("[Main] WiFi setup timed out, rebooting\n");
            ESP.restart();
        }
    }
    improv.setConnected(WiFi.localIP());
}

// Sends one notification to a single recipient.
static bool sendToRecipient(const Recipient& r, const Notification& n, const String& priority) {
    if (r.type == "bark") {
        return BarkNotifier(r.key).send(n, priority);
    }
    if (r.type == "pushover") {
        return PushoverNotifier(r.token, r.user).send(n, priority);
    }
    Serial.printf("[Main] Unknown recipient type '%s'\n", r.type.c_str());
    return false;
}

// Sends via the single "Default provider" selection — used when no
// per-recipient list is configured. MQTT is only available this way (a
// per-recipient broker/topic doesn't fit the Recipient schema, and one global
// broker covers the realistic use case).
static bool sendViaDefaultProvider(const Notification& n, const String& priority) {
    switch (webAdmin.config.activeNotifier) {
        case NOTIFIER_PUSHOVER:
            return PushoverNotifier(webAdmin.config.pushoverToken, webAdmin.config.pushoverUser).send(n, priority);
        case NOTIFIER_MQTT:
            return MqttNotifier(webAdmin.config.mqttBroker, webAdmin.config.mqttPort,
                                 webAdmin.config.mqttUser, webAdmin.config.mqttPassword,
                                 webAdmin.config.mqttTopic, webAdmin.config.mqttUseTls)
                .send(n, priority);
        case NOTIFIER_BARK:
        default: {
            String key = webAdmin.config.barkKey.length() ? webAdmin.config.barkKey : BARK_KEY;
            return BarkNotifier(key).send(n, priority);
        }
    }
}

// Handles one fully-parsed ANCS notification: applies the app filter and DND
// schedule, then forwards it (with its per-app priority) to every configured
// recipient, falling back to the legacy single-provider config if none are set.
static void onAncsNotification(const Notification& n) {
    stats.recordReceived(n.appName);
    Serial.printf("[Main] Notification: %s / %s: %s (uid=%u)\n",
                   n.appName.c_str(), n.title.c_str(), n.body.c_str(), n.uid);

    String priority = webAdmin.config.priorityForApp(n.appName);

    // Apply the user's custom message-format template, if any, before the
    // notification is filtered/logged/sent — everything downstream sees the
    // rendered body so the log reflects exactly what was (or would have been)
    // forwarded.
    Notification outbound = n;
    if (webAdmin.config.messageTemplate.length()) {
        outbound.body = renderMessageTemplate(webAdmin.config.messageTemplate, n, priority);
    }

    if (!webAdmin.config.isAppAllowed(n.appName)) {
        Serial.printf("[Main] Dropped (not on allowlist): %s\n", n.appName.c_str());
        stats.recordFiltered();
        notifLog.add(outbound, priority, "filtered_app");
        return;
    }

    struct tm timeinfo;
    if (webAdmin.config.dndEnabled && getLocalTime(&timeinfo, 100) &&
        webAdmin.config.isWithinDnd(timeinfo.tm_hour)) {
        Serial.printf("[Main] Dropped (DND window): %s\n", n.appName.c_str());
        stats.recordFiltered();
        notifLog.add(outbound, priority, "filtered_dnd");
        return;
    }

    bool anySent = false;

    if (webAdmin.config.recipients.empty()) {
        anySent = sendViaDefaultProvider(outbound, priority);
    } else {
        for (const Recipient& r : webAdmin.config.recipients) {
            if (sendToRecipient(r, outbound, priority)) anySent = true;
        }
    }

    notifLog.add(outbound, priority, anySent ? "forwarded" : "failed");

    if (anySent) {
        stats.recordForwarded();
        led.pulseSuccess();
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("[Main] %s v%s starting (%s)\n", APP_NAME, FIRMWARE_VERSION, FIRMWARE_BUILD_DATE);

    led.begin();
    led.setState(LedState::Connecting);

    // 1. WiFi: stored credentials, then Improv-serial (browser flasher over
    //    USB), then the WiFiManager captive portal as a last resort.
    connectWifi();
    Serial.printf("[Main] WiFi connected: %s\n", WiFi.localIP().toString().c_str());

    // ESP32 has a single 2.4GHz radio shared by WiFi and Bluetooth via
    // time-division coexistence. Under simultaneous WiFi (web admin traffic)
    // and active BLE (ANCS) load, the default balanced scheduling has been
    // seen to corrupt TCP/HCI packet timing badly enough to crash the lwIP
    // and Bluetooth stacks. ANCS is this device's actual purpose — prioritize
    // Bluetooth's radio time over WiFi's.
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    // 2. mDNS so the admin UI is reachable at http://pushrelay.local
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[Main] mDNS started: http://%s.local\n", MDNS_HOSTNAME);
    } else {
        Serial.printf("[Main] mDNS setup failed\n");
    }

    // 3. NTP sync (UTC, no offset) — needed for the Do Not Disturb schedule.
    configTime(0, 0, "pool.ntp.org");

    // 4. OTA updates — no USB cable needed after this first flash.
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.printf("[OTA] Update starting\n"); });
    ArduinoOTA.onEnd([]() { Serial.printf("[OTA] Update complete, rebooting\n"); });
    ArduinoOTA.onError([](ota_error_t error) { Serial.printf("[OTA] Error %u\n", error); });
    ArduinoOTA.begin();

    // 5. Load persisted config, start the web admin UI.
    webAdmin.loadConfig();
    webAdmin.begin(
        []() { return ancs.isPeerConnected(); },
        []() { return ancs.isAncsReady(); },
        &stats,
        &notifLog);

    // 6. Start advertising for ANCS and wire up the notification callback.
    ancs.begin(BLE_DEVICE_NAME, onAncsNotification);
    ancs.setManualUtcOffsetProvider([](int16_t& outMinutes) -> bool {
        if (!webAdmin.config.phoneUtcOffsetSet) return false;
        outMinutes = webAdmin.config.phoneUtcOffsetMinutes;
        return true;
    });
    lastBleOkMillis = millis();

    // 7. Task watchdog — reboots if the main loop ever stops running.
    esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
    esp_task_wdt_add(NULL);
}

void loop() {
    esp_task_wdt_reset();

    ancs.loop();
    ArduinoOTA.handle();
    led.update();
    improv.loop();   // keep answering Improv state queries (flasher reopened, etc.)

    // BLE-specific recovery: reboot if disconnected too long, independent of the
    // generic task watchdog (which only catches a fully hung loop).
    if (ancs.isPeerConnected()) {
        lastBleOkMillis = millis();
    } else if (millis() - lastBleOkMillis > BLE_RECOVERY_TIMEOUT_MS) {
        Serial.printf("[Main] BLE disconnected for 5+ minutes, rebooting\n");
        ESP.restart();
    }

    if (WiFi.status() != WL_CONNECTED) {
        led.setState(LedState::Error);
    } else if (ancs.isAncsReady()) {
        led.setState(LedState::Connected);
    } else {
        led.setState(LedState::Connecting);
    }
}
