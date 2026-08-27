// PushRelay v0.5 — oleg@abramov.dev
// Web admin UI: serves the LittleFS-hosted page and the runtime config/status/stats API.
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "version.h"
#include "stats.h"
#include "notiflog.h"

// One delivery target for a forwarded notification. `type` is "bark" |
// "pushover" | "mqtt" (only "bark" actually delivers before Phase 4).
struct Recipient {
    String type;
    String key;   // bark
    String token; // pushover
    String user;  // pushover
};

// Runtime settings persisted in NVS, editable from the admin UI.
struct RuntimeConfig {
    uint8_t activeNotifier = ACTIVE_NOTIFIER;
    String barkKey;
    String pushoverToken;
    String pushoverUser;
    String mqttBroker;
    uint16_t mqttPort = 1883;
    String mqttUser;
    String mqttPassword;
    String mqttTopic = "pushrelay/notifications";
    bool mqttUseTls = false;

    bool filterEnabled = false;
    String allowedApps; // comma-separated app names

    bool dndEnabled = false;
    uint8_t dndStartHour = 23; // UTC — device time is NTP-synced without a TZ offset
    uint8_t dndEndHour = 8;

    String prioritiesJson = "{}"; // { "AppName": "critical"|"high"|"low" }

    std::vector<Recipient> recipients;

    // True if `appName` should be forwarded given the current filter settings.
    bool isAppAllowed(const String& appName) const {
        if (!filterEnabled || allowedApps.isEmpty()) return true;
        String list = "," + allowedApps + ",";
        list.toLowerCase();
        String needle = "," + appName + ",";
        needle.toLowerCase();
        return list.indexOf(needle) >= 0;
    }

    // True if `hour` (0-23) falls inside the configured DND window.
    bool isWithinDnd(int hour) const {
        if (!dndEnabled || dndStartHour == dndEndHour) return false;
        if (dndStartHour < dndEndHour) {
            return hour >= dndStartHour && hour < dndEndHour;
        }
        return hour >= dndStartHour || hour < dndEndHour; // wraps past midnight
    }

    // Per-app priority rule ("critical" | "high" | "low"), or "default" if unset.
    String priorityForApp(const String& appName) const {
        JsonDocument doc;
        if (deserializeJson(doc, prioritiesJson) != DeserializationError::Ok) return "default";
        if (doc[appName].is<const char*>()) return doc[appName].as<String>();
        return "default";
    }

    String serializeRecipients() const {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (const Recipient& r : recipients) {
            JsonObject o = arr.add<JsonObject>();
            o["type"] = r.type;
            o["key"] = r.key;
            o["token"] = r.token;
            o["user"] = r.user;
        }
        String out;
        serializeJson(doc, out);
        return out;
    }

    void deserializeRecipients(const String& json) {
        recipients.clear();
        JsonDocument doc;
        if (deserializeJson(doc, json) != DeserializationError::Ok) return;
        if (!doc.is<JsonArray>()) return;
        for (JsonObject o : doc.as<JsonArray>()) {
            Recipient r;
            r.type = o["type"] | "";
            r.key = o["key"] | "";
            r.token = o["token"] | "";
            r.user = o["user"] | "";
            if (r.type.length()) recipients.push_back(r);
        }
    }
};

// Sends a JSON body and forces the connection closed instead of attempting
// HTTP keep-alive. Without this, ESPAsyncWebServer/lwIP on this platform
// leaks TCP PCBs under repeated polling (the admin page refreshes /api/status
// and /api/stats every few seconds) until the pool is exhausted and lwIP hits
// a fatal assert ("tcp_receive: wrong state"), crashing the whole device.
static void sendJson(AsyncWebServerRequest* request, int code, const String& body) {
    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", body);
    response->addHeader("Connection", "close");
    request->send(response);
}

class WebAdmin {
public:
    // Loads persisted settings from NVS into `config`.
    void loadConfig() {
        prefs.begin("pushrelay", true);
        config.activeNotifier = prefs.getUChar("notifier", ACTIVE_NOTIFIER);
        config.barkKey = prefs.getString("barkKey", "");
        config.pushoverToken = prefs.getString("poToken", "");
        config.pushoverUser = prefs.getString("poUser", "");
        config.mqttBroker = prefs.getString("mqttBroker", "");
        config.mqttPort = prefs.getUShort("mqttPort", 1883);
        config.mqttUser = prefs.getString("mqttUser", "");
        config.mqttPassword = prefs.getString("mqttPass", "");
        config.mqttTopic = prefs.getString("mqttTopic", "pushrelay/notifications");
        config.mqttUseTls = prefs.getBool("mqttTls", false);
        config.filterEnabled = prefs.getBool("filterOn", false);
        config.allowedApps = prefs.getString("allowedApps", "");
        config.dndEnabled = prefs.getBool("dndOn", false);
        config.dndStartHour = prefs.getUChar("dndStart", 23);
        config.dndEndHour = prefs.getUChar("dndEnd", 8);
        config.prioritiesJson = prefs.getString("priorities", "{}");
        config.deserializeRecipients(prefs.getString("recipients", "[]"));
        prefs.end();
    }

    // Persists `config` to NVS.
    void saveConfig() {
        prefs.begin("pushrelay", false);
        prefs.putUChar("notifier", config.activeNotifier);
        prefs.putString("barkKey", config.barkKey);
        prefs.putString("poToken", config.pushoverToken);
        prefs.putString("poUser", config.pushoverUser);
        prefs.putString("mqttBroker", config.mqttBroker);
        prefs.putUShort("mqttPort", config.mqttPort);
        prefs.putString("mqttUser", config.mqttUser);
        prefs.putString("mqttPass", config.mqttPassword);
        prefs.putString("mqttTopic", config.mqttTopic);
        prefs.putBool("mqttTls", config.mqttUseTls);
        prefs.putBool("filterOn", config.filterEnabled);
        prefs.putString("allowedApps", config.allowedApps);
        prefs.putBool("dndOn", config.dndEnabled);
        prefs.putUChar("dndStart", config.dndStartHour);
        prefs.putUChar("dndEnd", config.dndEndHour);
        prefs.putString("priorities", config.prioritiesJson);
        prefs.putString("recipients", config.serializeRecipients());
        prefs.end();
    }

    // Registers routes and starts the async HTTP server. `peerConnectedFn` and
    // `ancsReadyFn` let the status endpoint report live BLE state; `stats` backs
    // the /api/stats endpoint; `notifLog` backs /api/log.
    void begin(std::function<bool()> peerConnected, std::function<bool()> ancsReady, Stats* statsPtr,
               NotificationLog* notifLogPtr) {
        peerConnectedFn = peerConnected;
        ancsReadyFn = ancsReady;
        stats = statsPtr;
        notifLog = notifLogPtr;

        if (!LittleFS.begin(true)) {
            Serial.printf("[WebAdmin] LittleFS mount failed\n");
        }

        server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            doc["activeNotifier"] = config.activeNotifier;
            doc["barkKey"] = config.barkKey;
            doc["pushoverToken"] = config.pushoverToken;
            doc["pushoverUser"] = config.pushoverUser;
            doc["mqttBroker"] = config.mqttBroker;
            doc["mqttPort"] = config.mqttPort;
            doc["mqttUser"] = config.mqttUser;
            doc["mqttPassword"] = config.mqttPassword;
            doc["mqttTopic"] = config.mqttTopic;
            doc["mqttUseTls"] = config.mqttUseTls;
            doc["filterEnabled"] = config.filterEnabled;
            doc["allowedApps"] = config.allowedApps;
            doc["dndEnabled"] = config.dndEnabled;
            doc["dndStartHour"] = config.dndStartHour;
            doc["dndEndHour"] = config.dndEndHour;
            JsonDocument prioritiesDoc;
            deserializeJson(prioritiesDoc, config.prioritiesJson);
            doc["priorities"] = prioritiesDoc;
            JsonDocument recipientsDoc;
            deserializeJson(recipientsDoc, config.serializeRecipients());
            doc["recipients"] = recipientsDoc;
            String out;
            serializeJson(doc, out);
            sendJson(request, 200, out);
        });

        server.on("/api/config", HTTP_POST,
            [](AsyncWebServerRequest* request) {},
            nullptr,
            [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
                JsonDocument doc;
                if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                    sendJson(request, 400, "{\"error\":\"invalid JSON\"}");
                    return;
                }
                if (doc["activeNotifier"].is<uint8_t>()) config.activeNotifier = doc["activeNotifier"];
                if (doc["barkKey"].is<const char*>()) config.barkKey = doc["barkKey"].as<String>();
                if (doc["pushoverToken"].is<const char*>()) config.pushoverToken = doc["pushoverToken"].as<String>();
                if (doc["pushoverUser"].is<const char*>()) config.pushoverUser = doc["pushoverUser"].as<String>();
                if (doc["mqttBroker"].is<const char*>()) config.mqttBroker = doc["mqttBroker"].as<String>();
                if (doc["mqttPort"].is<uint16_t>()) config.mqttPort = doc["mqttPort"];
                if (doc["mqttUser"].is<const char*>()) config.mqttUser = doc["mqttUser"].as<String>();
                if (doc["mqttPassword"].is<const char*>()) config.mqttPassword = doc["mqttPassword"].as<String>();
                if (doc["mqttTopic"].is<const char*>()) config.mqttTopic = doc["mqttTopic"].as<String>();
                if (doc["mqttUseTls"].is<bool>()) config.mqttUseTls = doc["mqttUseTls"];
                if (doc["filterEnabled"].is<bool>()) config.filterEnabled = doc["filterEnabled"];
                if (doc["allowedApps"].is<const char*>()) config.allowedApps = doc["allowedApps"].as<String>();
                if (doc["dndEnabled"].is<bool>()) config.dndEnabled = doc["dndEnabled"];
                if (doc["dndStartHour"].is<uint8_t>()) config.dndStartHour = doc["dndStartHour"];
                if (doc["dndEndHour"].is<uint8_t>()) config.dndEndHour = doc["dndEndHour"];
                if (doc["priorities"].is<JsonObject>()) {
                    String out;
                    serializeJson(doc["priorities"], out);
                    config.prioritiesJson = out;
                }
                if (doc["recipients"].is<JsonArray>()) {
                    String out;
                    serializeJson(doc["recipients"], out);
                    config.deserializeRecipients(out);
                }
                saveConfig();
                sendJson(request, 200, "{\"status\":\"ok\"}");
            });

        server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            doc["bleConnected"] = peerConnectedFn ? peerConnectedFn() : false;
            doc["ancsReady"] = ancsReadyFn ? ancsReadyFn() : false;
            doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
            doc["wifiRssi"] = WiFi.RSSI();
            doc["ipAddress"] = WiFi.localIP().toString();
            doc["uptimeSeconds"] = millis() / 1000;
            doc["provider"] = config.activeNotifier;
            doc["firmwareVersion"] = FIRMWARE_VERSION;
            String out;
            serializeJson(doc, out);
            sendJson(request, 200, out);
        });

        server.on("/api/stats", HTTP_GET, [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            doc["uptime_seconds"] = millis() / 1000;
            doc["ble_connected"] = peerConnectedFn ? peerConnectedFn() : false;
            doc["wifi_rssi"] = WiFi.RSSI();
            if (stats) {
                doc["notifications_received"] = stats->received;
                doc["notifications_forwarded"] = stats->forwarded;
                doc["notifications_filtered"] = stats->filtered;
                JsonArray topApps = doc["top_apps"].to<JsonArray>();
                for (auto& entry : stats->topApps()) {
                    JsonObject o = topApps.add<JsonObject>();
                    o["app"] = entry.first;
                    o["count"] = entry.second;
                }
            }
            String out;
            serializeJson(doc, out);
            sendJson(request, 200, out);
        });

        server.on("/api/log", HTTP_GET, [this](AsyncWebServerRequest* request) {
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            if (notifLog) {
                uint32_t now = millis();
                for (const LogEntry& e : notifLog->recent()) {
                    JsonObject o = arr.add<JsonObject>();
                    o["app"] = e.appName;
                    o["title"] = e.title;
                    o["body"] = e.body;
                    o["uid"] = e.uid;
                    o["priority"] = e.priority;
                    o["status"] = e.status;
                    o["ageSeconds"] = (now - e.timestampMs) / 1000;
                }
            }
            String out;
            serializeJson(doc, out);
            sendJson(request, 200, out);
        });

        server.on("/api/ota-status", HTTP_GET, [](AsyncWebServerRequest* request) {
            JsonDocument doc;
            doc["firmwareVersion"] = FIRMWARE_VERSION;
            doc["buildDate"] = FIRMWARE_BUILD_DATE;
            doc["hostname"] = MDNS_HOSTNAME;
            doc["freeHeap"] = ESP.getFreeHeap();
            doc["sketchSize"] = ESP.getSketchSize();
            doc["sketchSpace"] = ESP.getSketchSize() + ESP.getFreeSketchSpace();
            String out;
            serializeJson(doc, out);
            sendJson(request, 200, out);
        });

        // Registered last so it never shadows the /api/* routes above — ESPAsyncWebServer
        // matches handlers in registration order, and a static handler at "/" matches
        // every path.
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

        server.begin();
        Serial.printf("[WebAdmin] Listening on http://%s.local/\n", MDNS_HOSTNAME);
    }

    RuntimeConfig config;

private:
    AsyncWebServer server{80};
    Preferences prefs;
    std::function<bool()> peerConnectedFn;
    std::function<bool()> ancsReadyFn;
    Stats* stats = nullptr;
    NotificationLog* notifLog = nullptr;
};
