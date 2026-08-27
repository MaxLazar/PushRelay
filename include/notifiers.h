// PushRelay v0.5 — oleg@abramov.dev
// Notifier abstraction layer: turns a parsed Notification into an outbound push via a provider.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// A single parsed ANCS notification, ready to hand off to a notifier.
struct Notification {
    String appId;   // raw bundle ID, e.g. "com.microsoft.teams"
    String appName; // human-readable, e.g. "Teams"
    String title;
    String body;
    uint32_t uid;   // ANCS notification UID, used for dedup in Phase 2
};

// Common interface implemented by every delivery provider.
class BaseNotifier {
public:
    virtual ~BaseNotifier() {}
    // `priority` is one of "critical" | "high" | "low" | "default" (see
    // priorityToBarkLevel / priorityToPushoverValue) — the per-app priority rule,
    // if any, resolved by the caller. Returns true once the notification was
    // accepted by the provider.
    virtual bool send(const Notification& n, const String& priority = "default") = 0;
};

// Maps a generic priority name to a Bark `level` query parameter.
inline String priorityToBarkLevel(const String& priority) {
    if (priority == "critical") return "critical";
    if (priority == "low") return "passive";
    return "timeSensitive"; // "high" or "default"
}

// Bark's docs specify the literal two-character sequence "\n" for line breaks
// in the body — not an actual newline byte. ANCS notification bodies (e.g.
// multi-line calendar invites) contain real newlines, and Bark's server
// rejects a percent-encoded raw newline (%0A) in a URL path segment with
// HTTP 400. This must run before urlEncode().
inline String barkEscapeNewlines(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            continue; // drop bare CRs; the following \n (if any) already covers the break
        } else {
            out += c;
        }
    }
    return out;
}

// Maps a generic priority name to a Pushover `priority` value.
// https://pushover.net/api#priority
inline int priorityToPushoverValue(const String& priority) {
    if (priority == "critical") return 2; // emergency — requires retry/expire, see PushoverNotifier
    if (priority == "high") return 1;     // high priority, bypasses quiet hours
    if (priority == "low") return -1;     // low priority, no sound/vibration
    return 0;                             // default (normal) priority
}

// Percent-encodes a string for safe use inside a URL path segment.
inline String urlEncode(const String& value) {
    String encoded;
    encoded.reserve(value.length() * 3);
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < value.length(); i++) {
        uint8_t c = (uint8_t)value[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else {
            encoded += '%';
            encoded += hex[(c >> 4) & 0xF];
            encoded += hex[c & 0xF];
        }
    }
    return encoded;
}

// Delivers notifications to a Bark server (https://api.day.app or a self-hosted instance).
class BarkNotifier : public BaseNotifier {
public:
    explicit BarkNotifier(const String& deviceKey, const String& serverHost = "api.day.app")
        : deviceKey(deviceKey), serverHost(serverHost) {}

    bool send(const Notification& n, const String& priority = "default") override {
        if (deviceKey.isEmpty()) {
            Serial.printf("[Bark] Skipped: no device key configured\n");
            return false;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[Bark] Skipped: WiFi not connected\n");
            return false;
        }

        // Bark's URL API is /{key}/{title}/{body} — there's no separate app-name
        // path segment (an earlier version of this used {key}/{appName}/{title}
        // with body as a query param, which Bark's real API rejects with
        // HTTP 400). The app name is preserved via the `group` param instead,
        // which Bark uses to cluster notifications by source.
        String title = barkEscapeNewlines(n.title.isEmpty() ? n.appName : n.title);
        String body = barkEscapeNewlines(n.body);
        String url = "https://" + serverHost + "/" + deviceKey + "/" +
                     urlEncode(title) + "/" + urlEncode(body) + "?level=" +
                     priorityToBarkLevel(priority) + "&group=" + urlEncode(n.appName);

        const int maxAttempts = 3;
        for (int attempt = 1; attempt <= maxAttempts; attempt++) {
            WiFiClientSecure client;
            client.setInsecure(); // Bark's public API cert chain is not pinned here.
            HTTPClient http;
            if (!http.begin(client, url)) {
                Serial.printf("[Bark] begin() failed for request\n");
                return false;
            }
            int code = http.GET();
            http.end();
            if (code > 0 && code < 400) {
                Serial.printf("[Bark] Sent (HTTP %d): %s / %s\n", code, n.appName.c_str(), title.c_str());
                return true;
            }
            Serial.printf("[Bark] Attempt %d/%d failed (HTTP %d)\n", attempt, maxAttempts, code);
            delay(500 * attempt);
        }
        Serial.printf("[Bark] Giving up after %d attempts\n", maxAttempts);
        return false;
    }

private:
    String deviceKey;
    String serverHost;
};

// Delivers notifications via Pushover (https://pushover.net).
class PushoverNotifier : public BaseNotifier {
public:
    PushoverNotifier(const String& appToken, const String& userKey)
        : appToken(appToken), userKey(userKey) {}

    bool send(const Notification& n, const String& priority = "default") override {
        if (appToken.isEmpty() || userKey.isEmpty()) {
            Serial.printf("[Pushover] Skipped: token/user not configured\n");
            return false;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[Pushover] Skipped: WiFi not connected\n");
            return false;
        }

        String title = n.title.isEmpty() ? n.appName : n.title;
        int pushoverPriority = priorityToPushoverValue(priority);

        String form = "token=" + urlEncode(appToken) + "&user=" + urlEncode(userKey) +
                      "&title=" + urlEncode(n.appName) + "&message=" + urlEncode(title + ": " + n.body) +
                      "&priority=" + String(pushoverPriority);
        // Emergency priority (2) requires retry/expire: how often (seconds) to
        // resend until acknowledged, and when to give up.
        if (pushoverPriority == 2) {
            form += "&retry=60&expire=3600";
        }

        const int maxAttempts = 3;
        for (int attempt = 1; attempt <= maxAttempts; attempt++) {
            WiFiClientSecure client;
            client.setInsecure(); // Pushover's cert chain is not pinned here.
            HTTPClient http;
            if (!http.begin(client, "https://api.pushover.net/1/messages.json")) {
                Serial.printf("[Pushover] begin() failed for request\n");
                return false;
            }
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            int code = http.POST(form);
            http.end();
            if (code > 0 && code < 400) {
                Serial.printf("[Pushover] Sent (HTTP %d): %s / %s\n", code, n.appName.c_str(), title.c_str());
                return true;
            }
            Serial.printf("[Pushover] Attempt %d/%d failed (HTTP %d)\n", attempt, maxAttempts, code);
            delay(500 * attempt);
        }
        Serial.printf("[Pushover] Giving up after %d attempts\n", maxAttempts);
        return false;
    }

private:
    String appToken;
    String userKey;
};

// Delivers notifications as a JSON message to an MQTT broker.
class MqttNotifier : public BaseNotifier {
public:
    MqttNotifier(const String& broker, uint16_t port, const String& user, const String& password,
                 const String& topic, bool useTls)
        : broker(broker), port(port), user(user), password(password), topic(topic), useTls(useTls) {}

    bool send(const Notification& n, const String& priority = "default") override {
        if (broker.isEmpty() || topic.isEmpty()) {
            Serial.printf("[MQTT] Skipped: broker/topic not configured\n");
            return false;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[MQTT] Skipped: WiFi not connected\n");
            return false;
        }

        JsonDocument doc;
        doc["app"] = n.appName;
        doc["title"] = n.title.isEmpty() ? n.appName : n.title;
        doc["body"] = n.body;
        doc["priority"] = priority;
        String payload;
        serializeJson(doc, payload);

        WiFiClientSecure secureClient;
        WiFiClient plainClient;
        Client* netClient = &plainClient;
        if (useTls) {
            secureClient.setInsecure(); // broker's cert chain is not pinned here.
            netClient = &secureClient;
        }

        PubSubClient mqtt(*netClient);
        mqtt.setServer(broker.c_str(), port);

        // Each connection needs a distinct client ID so a lingering session on
        // the broker from a previous send doesn't refuse this one.
        String clientId = "PushRelay-" + String((uint32_t)esp_random(), HEX);
        bool connected = user.length()
            ? mqtt.connect(clientId.c_str(), user.c_str(), password.c_str())
            : mqtt.connect(clientId.c_str());

        if (!connected) {
            Serial.printf("[MQTT] Connect failed (rc=%d)\n", mqtt.state());
            return false;
        }

        bool published = mqtt.publish(topic.c_str(), payload.c_str());
        mqtt.disconnect();

        if (published) {
            Serial.printf("[MQTT] Published to %s: %s\n", topic.c_str(), n.appName.c_str());
        } else {
            Serial.printf("[MQTT] Publish failed\n");
        }
        return published;
    }

private:
    String broker;
    uint16_t port;
    String user;
    String password;
    String topic;
    bool useTls;
};
