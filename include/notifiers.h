// PushRelay v0.5 — oleg@abramov.dev
// Notifier abstraction layer: turns a parsed Notification into an outbound push via a provider.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// A single parsed ANCS notification, ready to hand off to a notifier.
struct Notification {
    String appId;   // raw bundle ID, e.g. "com.microsoft.teams"
    String appName; // human-readable, e.g. "Teams"
    String title;
    String body;     // short preview, capped at ANCS_SHORT_MESSAGE_LEN (see ancs.h)
    String bodyLong; // full message, up to ANCS_MAX_MESSAGE_LEN; backs {longBody}
    uint32_t uid;      // ANCS notification UID, used for dedup in Phase 2
    // Raw ANCS Date attribute: "YYYYMMDD'T'HHMMSS" (ISO 8601 basic format),
    // the phone's local time when the notification was originally created —
    // not when PushRelay received or forwarded it. Empty if ANCS didn't
    // return one (older iOS versions, or some app-generated notifications).
    String sourceDate;
};

// Splits an ANCS "YYYYMMDD'T'HHMMSS" Date attribute into separate date/time
// strings. Leaves both empty if `raw` doesn't match the expected format.
inline void parseAncsDate(const String& raw, String& dateOut, String& timeOut) {
    if (raw.length() < 15 || raw[8] != 'T') return;
    dateOut = raw.substring(0, 4) + "-" + raw.substring(4, 6) + "-" + raw.substring(6, 8);
    timeOut = raw.substring(9, 11) + ":" + raw.substring(11, 13) + ":" + raw.substring(13, 15);
}

// Parses an ANCS "YYYYMMDD'T'HHMMSS" Date attribute into a Unix timestamp,
// treating the fields as if they were UTC. This is NOT the real creation
// instant — ANCS reports the phone's LOCAL time, so the result needs a
// UTC-offset correction (see AncsManager's backlog-staleness filter in
// ancs.h) before it means anything. Returns 0 if `raw` doesn't match the
// expected format.
inline time_t parseAncsDateNaive(const String& raw) {
    if (raw.length() < 15 || raw[8] != 'T') return 0;
    struct tm t = {};
    t.tm_year = raw.substring(0, 4).toInt() - 1900;
    t.tm_mon = raw.substring(4, 6).toInt() - 1;
    t.tm_mday = raw.substring(6, 8).toInt();
    t.tm_hour = raw.substring(9, 11).toInt();
    t.tm_min = raw.substring(11, 13).toInt();
    t.tm_sec = raw.substring(13, 15).toInt();
    t.tm_isdst = 0;
    return mktime(&t);
}

// Renders a user-defined message template by substituting `{token}`
// placeholders with fields of the notification being forwarded. Unknown
// tokens are left as-is. Used to let the admin UI customize what actually
// gets sent to a provider instead of the hardcoded title/body split.
inline String renderMessageTemplate(const String& tmpl, const Notification& n, const String& priority) {
    String out = tmpl;

    char dateBuf[16] = "";
    char timeBuf[16] = "";
    char datetimeBuf[32] = "";
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 50)) {
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &timeinfo);
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
        strftime(datetimeBuf, sizeof(datetimeBuf), "%Y-%m-%d %H:%M", &timeinfo);
    }

    String sourceDate, sourceTime;
    parseAncsDate(n.sourceDate, sourceDate, sourceTime);
    String sourceDatetime = sourceDate.length() ? sourceDate + " " + sourceTime : "";

    out.replace("{app}", n.appName);
    out.replace("{title}", n.title.isEmpty() ? n.appName : n.title);
    out.replace("{body}", n.body);
    out.replace("{longBody}", n.bodyLong.isEmpty() ? n.body : n.bodyLong);
    out.replace("{priority}", priority);
    out.replace("{uid}", String(n.uid));
    out.replace("{date}", dateBuf);
    out.replace("{time}", timeBuf);
    out.replace("{datetime}", datetimeBuf);
    out.replace("{sourceDate}", sourceDate);
    out.replace("{sourceTime}", sourceTime);
    out.replace("{sourceDatetime}", sourceDatetime);
    return out;
}

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
