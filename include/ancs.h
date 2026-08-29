// PushRelay v0.5 — oleg@abramov.dev
//
// ANCS (Apple Notification Center Service) handler, built on NimBLE-Arduino.
//
// The ESP32 plays two BLE roles at once over the same physical link to the phone:
//   - Peripheral: advertises itself so iOS/macOS can discover and connect to it,
//     the same way a smartwatch does.
//   - GATT client: once connected and bonded, it connects back to the same peer
//     address to browse ANCS (a service the *phone* exposes) and subscribe to
//     its characteristics. NimBLE recognizes the existing link to that address
//     and attaches the client role to it rather than opening a second
//     connection — the standard technique for ANCS accessories.
//
// This project originally used the classic "ESP32 BLE Arduino" (Bluedroid)
// library, which turned out to have a serious WiFi+BLE coexistence bug: under
// simultaneous WiFi/HTTP and active BLE traffic, its Bluetooth controller would
// run out of internal HCI packet buffers and crash the whole device
// (assert failures in unrelated subsystems — lwIP, the BT HCI layer, generic
// linked-list code — are the signature of that kind of memory pressure/
// corruption). NimBLE is a much lighter-weight BLE host stack and is the
// standard fix for this class of problem in the ESP32 community.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>
#include <map>
#include <vector>

#include "notifiers.h"

// ANCS UUIDs (Apple Notification Center Service specification).
static const NimBLEUUID ANCS_SERVICE_UUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static const NimBLEUUID ANCS_NOTIFICATION_SOURCE_UUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static const NimBLEUUID ANCS_CONTROL_POINT_UUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static const NimBLEUUID ANCS_DATA_SOURCE_UUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

// ANCS EventID values (Notification Source, byte 0).
enum AncsEventId : uint8_t {
    ANCS_EVENT_NOTIFICATION_ADDED = 0,
    ANCS_EVENT_NOTIFICATION_MODIFIED = 1,
    ANCS_EVENT_NOTIFICATION_REMOVED = 2,
};

// ANCS CommandID values (Control Point / Data Source, byte 0).
enum AncsCommandId : uint8_t {
    ANCS_COMMAND_GET_NOTIFICATION_ATTRIBUTES = 0,
    ANCS_COMMAND_GET_APP_ATTRIBUTES = 1,
};

// ANCS NotificationAttributeID values.
enum AncsNotificationAttributeId : uint8_t {
    ANCS_ATTR_APP_IDENTIFIER = 0,
    ANCS_ATTR_TITLE = 1,
    ANCS_ATTR_SUBTITLE = 2,
    ANCS_ATTR_MESSAGE = 3,
    ANCS_ATTR_MESSAGE_SIZE = 4,
    ANCS_ATTR_DATE = 5,
};

static const uint16_t ANCS_MAX_TITLE_LEN = 32;
// Full message length requested from ANCS — backs Notification::bodyLong and the
// {longBody} template token. Deliberately large; the phone truncates to this and
// sets a "truncated" flag we ignore.
static const uint16_t ANCS_MAX_MESSAGE_LEN = 1024;
// Short preview length. Notification::body (and therefore {body} plus every
// provider's default formatting) is capped here so ordinary pushes and Bark URL
// paths stay small; {longBody} is the opt-in way to get the full text.
static const uint16_t ANCS_SHORT_MESSAGE_LEN = 128;

// Derives a readable app name from a reverse-DNS bundle identifier when it is
// not in the well-known map, e.g. "com.microsoft.Office.Outlook" -> "Outlook",
// "com.apple.MobileSMS" -> "SMS", "ph.telegra.Telegraph" -> "Telegraph".
inline String ancsNormalizeBundleId(const String& bundleId) {
    // Split on '.' and walk backwards for the first meaningful segment,
    // skipping generic tail words that carry no product identity.
    static const char* kGenericSegments[] = {
        "app", "ios", "iphone", "ipad", "mobile", "client", "inc", "llc",
    };

    int end = bundleId.length();
    while (end > 0) {
        int start = bundleId.lastIndexOf('.', end - 1);
        String segment = bundleId.substring(start + 1, end);
        end = start;

        if (segment.length() == 0) {
            continue;
        }

        bool generic = false;
        for (const char* g : kGenericSegments) {
            if (segment.equalsIgnoreCase(g)) {
                generic = true;
                break;
            }
        }
        if (generic && start > 0) {
            continue; // try the next segment up
        }

        // Trim a common "Mobile" prefix ("MobileSMS" -> "SMS").
        if (segment.length() > 6 && segment.startsWith("Mobile")) {
            segment = segment.substring(6);
        }

        // Capitalise the first letter; leave the rest as the vendor wrote it
        // so existing camel case ("FaceTime", "WhatsApp") survives.
        if (segment[0] >= 'a' && segment[0] <= 'z') {
            segment.setCharAt(0, segment[0] - ('a' - 'A'));
        }
        return segment;
    }

    return bundleId;
}

// Maps well-known bundle identifiers to a human-readable app name, falling back
// to a normalized form derived from the bundle ID itself.
inline String ancsFriendlyAppName(const String& bundleId) {
    static const std::map<String, String> kKnownApps = {
        // Apple stock apps
        {"com.apple.MobileSMS", "Messages"},
        {"com.apple.mobilephone", "Phone"},
        {"com.apple.mobilemail", "Mail"},
        {"com.apple.facetime", "FaceTime"},
        {"com.apple.calendar", "Calendar"},
        {"com.apple.mobilecal", "Calendar"},
        {"com.apple.reminders", "Reminders"},
        // Team chat / collaboration
        {"com.microsoft.teams", "Teams"},
        {"com.microsoft.skype.teams", "Teams"},
        {"com.tinyspeck.chatlyio", "Slack"},
        {"com.hammerandchisel.discord", "Discord"},
        {"com.google.dynamite", "Google Chat"},
        {"com.microsoft.skype.teams.disp", "Teams"},
        // Messengers
        {"net.whatsapp.WhatsApp", "WhatsApp"},
        {"ph.telegra.Telegraph", "Telegram"},
        {"org.whispersystems.signal", "Signal"},
        {"com.facebook.Messenger", "Messenger"},
        {"com.toyopagroup.picaboo", "Snapchat"},
        {"jp.naver.line", "LINE"},
        {"com.viber", "Viber"},
        {"com.skype.skype", "Skype"},
        {"com.tencent.xin", "WeChat"},
        {"com.google.GVDialer", "Google Voice"},
        // Mail
        {"com.microsoft.Office.Outlook", "Outlook"},
        {"com.google.Gmail", "Gmail"},
        {"com.readdle.smartemail", "Spark"},
        // Video calls
        {"us.zoom.videomeetings", "Zoom"},
        {"com.google.Meet", "Google Meet"},
        {"com.webex.meeting", "Webex"},
        // Social
        {"com.atebits.Tweetie2", "X"},
        {"com.burbn.instagram", "Instagram"},
        {"com.facebook.Facebook", "Facebook"},
        {"com.reddit.Reddit", "Reddit"},
    };
    auto it = kKnownApps.find(bundleId);
    if (it != kKnownApps.end()) {
        return it->second;
    }
    return ancsNormalizeBundleId(bundleId);
}

class AncsManager {
public:
    using NotificationCallback = std::function<void(const Notification&)>;

    // Starts BLE, begins advertising as a peripheral, and installs the callback
    // invoked for every fully-parsed incoming notification.
    void begin(const String& deviceName, NotificationCallback cb) {
        onNotification = cb;
        NimBLEDevice::init(deviceName.c_str());

        // Apple's ANCS spec says all three characteristics "require authorization
        // for access" — in practice this means an *authenticated* (MITM-
        // protected) bond, not just an encrypted one. "Just Works" pairing
        // (IO_CAP_NO_IO) is encrypted but never authenticated, and the phone
        // will happily let a Just-Works-bonded accessory subscribe to
        // Notification Source without ever actually sending it any data — which
        // is indistinguishable from a working subscription until you notice
        // nothing arrives. NimBLE's own bundled ANCS.ino example uses exactly
        // this DISPLAY_YESNO + MITM configuration for that reason.
        //
        // Our hardware has no display or buttons, so we can't do genuine
        // numeric-comparison verification — we advertise the capability and
        // auto-confirm on our side (see onConfirmPassKey below) purely to
        // satisfy the protocol's requirement for an authenticated bond.
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
        NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM | BLE_SM_PAIR_AUTHREQ_SC);

        server = NimBLEDevice::createServer();
        server->setCallbacks(new ServerCallbacks(this));

        NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();

        // Advertise the ANCS UUID as a *Service Solicitation* (AD type 0x15),
        // not as a normal advertised/provided service (AD type 0x07). ANCS is a
        // service the *phone* hosts — solicitation is how an accessory signals
        // "I'm looking for a peer offering this service", which is what iOS
        // checks before it treats this peripheral as an ANCS accessory and
        // offers the pairing/notification-sharing prompt.
        NimBLEAdvertisementData advData;
        advData.setFlags(0x06); // General Discoverable, BR/EDR not supported
        const uint8_t* uuidBytes = ANCS_SERVICE_UUID.getValue(); // 16 bytes, native order
        uint8_t solicitation[18];
        solicitation[0] = 17;   // length: 1 (AD type) + 16 (UUID)
        solicitation[1] = 0x15; // AD type: List of 128-bit Service Solicitation UUIDs
        memcpy(&solicitation[2], uuidBytes, 16);
        advData.addData(solicitation, sizeof(solicitation));
        advertising->setAdvertisementData(advData);

        NimBLEAdvertisementData scanResponse;
        scanResponse.setName(deviceName.c_str());
        advertising->setScanResponseData(scanResponse);

        NimBLEDevice::startAdvertising();

        Serial.printf("[ANCS] Advertising as \"%s\"\n", deviceName.c_str());
    }

    // Call from the main loop. Reacts to connect/disconnect flags set by BLE
    // callbacks (kept minimal — no heap allocation, no Serial I/O, no further
    // BLE calls — since doing real work directly in a BLE stack callback can
    // destabilize the stack's internal event queue) and retries the ANCS
    // discovery handshake until it succeeds or the peer disconnects. A single
    // attempt isn't enough: pairing requires the user to respond to a system
    // dialog, and ANCS may not be exposed until that completes.
    void loop() {
        if (hasPendingConnectEvent) {
            hasPendingConnectEvent = false;
            peerAddress = pendingPeerAddress;
            peerConnHandle = pendingConnHandle;
            pendingClientConnect = true;
            clientConnectAttempts = 0;
            nextClientAttemptMillis = millis() + 1500; // let pairing kick off first
            Serial.printf("[ANCS] Peer connected: %s\n", peerAddress.toString().c_str());

            // Nothing on our GATT server actually requires encryption, so
            // nothing forces the phone to initiate pairing on its own. Request
            // it ourselves: this is what makes iOS/macOS show the pairing
            // dialog (and, since we asked for a bonded+encrypted link, the
            // "share your notifications with this accessory" ANCS consent).
            NimBLEDevice::startSecurity(peerConnHandle);
        }

        if (hasPendingDisconnectEvent) {
            hasPendingDisconnectEvent = false;
            Serial.printf("[ANCS] Peer disconnected (reason: %d)\n", pendingDisconnectReason);
            ancsClient = nullptr; // NimBLE owns/frees client objects on disconnect
            NimBLEDevice::startAdvertising();
        }

        // Send the GetNotificationAttributes write here, not from the
        // Notification Source subscribe callback — see the comment on
        // handleNotificationSource() for why that deadlocks the host task.
        if (hasPendingAttributeRequest && controlPointChar) {
            hasPendingAttributeRequest = false;
            requestNotificationAttributes(pendingAttributeRequestUid);
        }

        // Hand off completed notifications here, not from the Data Source
        // subscribe callback — see the comment on handleDataSource() for why.
        while (!pendingNotifications.empty()) {
            Notification n = pendingNotifications.front();
            pendingNotifications.erase(pendingNotifications.begin());
            if (onNotification) onNotification(n);
        }

        if (!pendingClientConnect || ancsReady) return;
        if (millis() < nextClientAttemptMillis) return;

        clientConnectAttempts++;
        connectAncsClient();

        if (ancsReady) {
            pendingClientConnect = false;
        } else if (clientConnectAttempts >= kMaxClientConnectAttempts) {
            Serial.printf("[ANCS] Giving up on ANCS client connect after %u attempts\n",
                          clientConnectAttempts);
            pendingClientConnect = false;
        } else {
            nextClientAttemptMillis = millis() + 2500;
        }
    }

    bool isPeerConnected() const { return peerConnected; }
    bool isAncsReady() const { return ancsReady; }

    // Lets the caller supply a user-configured phone UTC offset (minutes) for
    // the backlog-staleness filter below. `fn` should return true and set
    // `minutes` if the admin UI has one configured, false to fall back to
    // auto-calibration. See isStaleNotification().
    void setManualUtcOffsetProvider(std::function<bool(int16_t&)> fn) { manualUtcOffsetFn = fn; }

private:
    class ServerCallbacks : public NimBLEServerCallbacks {
    public:
        explicit ServerCallbacks(AncsManager* owner) : owner(owner) {}

        // Deliberately minimal: copy connection info and set flags only — see
        // the comment at the top of loop() for why.
        void onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) override {
            owner->pendingPeerAddress = connInfo.getAddress();
            owner->pendingConnHandle = connInfo.getConnHandle();
            owner->hasPendingConnectEvent = true;
            owner->peerConnected = true;
        }
        void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) override {
            owner->pendingDisconnectReason = reason;
            owner->hasPendingDisconnectEvent = true;
            owner->peerConnected = false;
            owner->ancsReady = false;
            owner->pendingClientConnect = false;
        }

        // We have no display or buttons to do genuine numeric-comparison
        // verification, so we auto-confirm — this is only to satisfy the
        // protocol's MITM/authenticated-bond requirement (see the comment on
        // setSecurityIOCap in begin()), not real out-of-band verification.
        void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
            NimBLEDevice::injectConfirmPasskey(connInfo, true);
        }
        AncsManager* owner;
    };

    // Discovers ANCS on the existing connection and subscribes to it. May be
    // called more than once per physical connection — see loop() — since ANCS
    // may not be exposed yet on the first attempt if pairing/bonding is still
    // completing in the background.
    //
    // NimBLEClient::connect() cannot be used here: NimBLE's host stack tracks
    // connections centrally and refuses to "connect" to a peer address that's
    // already connected (which it is — via our own peripheral role). The
    // documented way to get GATT-client access to a peer that connected to us
    // is NimBLEServer::getClient(), which returns a client object already
    // attached to that same connection (see NimBLE-Arduino's bundled ANCS.ino
    // example, which uses exactly this pattern).
    void connectAncsClient() {
        ancsClient = server->getClient(peerConnHandle);
        if (!ancsClient) {
            Serial.printf("[ANCS] getClient() returned null (attempt %u, will retry)\n", clientConnectAttempts);
            return;
        }

        NimBLERemoteService* svc = ancsClient->getService(ANCS_SERVICE_UUID);
        if (!svc) {
            Serial.printf("[ANCS] ANCS service not found yet (attempt %u, will retry)\n", clientConnectAttempts);
            return;
        }

        notificationSourceChar = svc->getCharacteristic(ANCS_NOTIFICATION_SOURCE_UUID);
        controlPointChar = svc->getCharacteristic(ANCS_CONTROL_POINT_UUID);
        dataSourceChar = svc->getCharacteristic(ANCS_DATA_SOURCE_UUID);

        if (!notificationSourceChar || !controlPointChar || !dataSourceChar) {
            Serial.printf("[ANCS] Missing one or more ANCS characteristics\n");
            return;
        }

        bool subscribedToNotificationSource = notificationSourceChar->subscribe(
            true,
            [this](NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
                handleNotificationSource(data, len);
            });
        bool subscribedToDataSource = dataSourceChar->subscribe(
            true,
            [this](NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
                handleDataSource(data, len);
            });

        if (!subscribedToNotificationSource || !subscribedToDataSource) {
            Serial.printf("[ANCS] Subscribe failed (notifSrc=%d, dataSrc=%d, attempt %u, will retry)\n",
                          subscribedToNotificationSource, subscribedToDataSource, clientConnectAttempts);
            return;
        }

        ancsReady = true;
        ancsReadyMillis = millis();
        Serial.printf("[ANCS] Ready — subscribed to Notification Source and Data Source\n");
    }

    // Parses an 8-byte Notification Source event and, for new/modified
    // notifications, queues the attribute request for loop() to send.
    //
    // This callback runs directly on NimBLE's host task (it's invoked straight
    // from the GAP event dispatcher — see NimBLEClient::handleGapEvent's
    // BLE_GAP_EVENT_NOTIFY_RX case). requestNotificationAttributes() sends a
    // write-with-response, which blocks waiting for that task to process the
    // write-completion event — calling it directly from here deadlocks the
    // host task against itself. Every real attempt at this hung the whole
    // device with zero further BLE activity until confirmed via a live debug
    // capture, so this deferral is load-bearing, not just tidiness.
    void handleNotificationSource(uint8_t* data, size_t len) {
        if (len < 8) return;

        uint8_t eventId = data[0];
        uint32_t uid;
        memcpy(&uid, &data[4], 4); // already little-endian on ESP32

        if (eventId == ANCS_EVENT_NOTIFICATION_ADDED || eventId == ANCS_EVENT_NOTIFICATION_MODIFIED) {
            if (isDuplicate(uid)) {
                Serial.printf("[ANCS] Duplicate uid=%u, dropped\n", uid);
                return;
            }
            rememberUid(uid);

            // ANCS resends "Added" events for every notification still sitting
            // in Notification Center as soon as we (re)subscribe — not just
            // genuinely new ones. Without this, every reconnect re-forwards the
            // phone's entire notification backlog (observed: a two-week-old
            // calendar reminder got pushed to Bark on reconnect, despite never
            // appearing on the phone that day). Real backlog arrives in a burst
            // right at connect; treat anything in that initial window as
            // backlog and drop it, while still remembering the UID above so a
            // near-simultaneous "Modified" for the same notification doesn't
            // double up.
            if (ancsReadyMillis != 0 && millis() - ancsReadyMillis < kBacklogSuppressWindowMs) {
                Serial.printf("[ANCS] Skipping backlog notification uid=%u (arrived %u ms after ready)\n",
                              uid, (unsigned)(millis() - ancsReadyMillis));
                return;
            }

            pendingAttributeRequestUid = uid;
            hasPendingAttributeRequest = true;
        }
    }

    // Sends a GetNotificationAttributes command via the Control Point for the
    // AppIdentifier, Title, Message and Date attributes.
    void requestNotificationAttributes(uint32_t uid) {
        if (!controlPointChar) return;

        pendingUid = uid;
        dataSourceBuffer.clear();

        uint8_t cmd[16];
        size_t i = 0;
        cmd[i++] = ANCS_COMMAND_GET_NOTIFICATION_ATTRIBUTES;
        memcpy(&cmd[i], &uid, 4);
        i += 4;

        cmd[i++] = ANCS_ATTR_APP_IDENTIFIER;

        cmd[i++] = ANCS_ATTR_TITLE;
        cmd[i++] = (uint8_t)(ANCS_MAX_TITLE_LEN & 0xFF);
        cmd[i++] = (uint8_t)((ANCS_MAX_TITLE_LEN >> 8) & 0xFF);

        cmd[i++] = ANCS_ATTR_MESSAGE;
        cmd[i++] = (uint8_t)(ANCS_MAX_MESSAGE_LEN & 0xFF);
        cmd[i++] = (uint8_t)((ANCS_MAX_MESSAGE_LEN >> 8) & 0xFF);

        // Date has no length field — unlike Title/Subtitle/Message, it isn't
        // truncatable, per the ANCS spec. It comes back as a fixed 15-char
        // ISO 8601 basic string: "YYYYMMDD'T'HHMMSS" (the phone's local time
        // when the notification was originally created, not when we asked).
        cmd[i++] = ANCS_ATTR_DATE;

        controlPointChar->writeValue(cmd, i, true);
    }

    // Accumulates Data Source fragments and, once a full GetNotificationAttributes
    // response has arrived, parses it into a Notification and fires the callback.
    void handleDataSource(uint8_t* data, size_t len) {
        dataSourceBuffer.insert(dataSourceBuffer.end(), data, data + len);

        // Minimum viable response: CommandID(1) + NotificationUID(4).
        if (dataSourceBuffer.size() < 5) return;
        if (dataSourceBuffer[0] != ANCS_COMMAND_GET_NOTIFICATION_ATTRIBUTES) {
            dataSourceBuffer.clear();
            return;
        }

        size_t pos = 5;
        String appId, title, message, date;
        bool complete = true;

        while (pos < dataSourceBuffer.size()) {
            if (pos + 3 > dataSourceBuffer.size()) { complete = false; break; }
            uint8_t attrId = dataSourceBuffer[pos];
            uint16_t attrLen = dataSourceBuffer[pos + 1] | (dataSourceBuffer[pos + 2] << 8);
            pos += 3;
            if (pos + attrLen > dataSourceBuffer.size()) { complete = false; break; }

            String value;
            value.reserve(attrLen);
            for (uint16_t j = 0; j < attrLen; j++) value += (char)dataSourceBuffer[pos + j];
            pos += attrLen;

            switch (attrId) {
                case ANCS_ATTR_APP_IDENTIFIER: appId = value; break;
                case ANCS_ATTR_TITLE: title = value; break;
                case ANCS_ATTR_MESSAGE: message = value; break;
                case ANCS_ATTR_DATE: date = value; break;
                default: break;
            }
        }

        if (!complete) return; // wait for the next fragment

        if (isStaleNotification(date)) {
            Serial.printf("[ANCS] Skipping stale notification uid=%u\n", pendingUid);
            dataSourceBuffer.clear();
            return;
        }

        Notification n;
        n.uid = pendingUid;
        n.appId = appId;
        n.appName = ancsFriendlyAppName(appId);
        n.title = title;
        n.body = message.length() > ANCS_SHORT_MESSAGE_LEN
                     ? message.substring(0, ANCS_SHORT_MESSAGE_LEN)
                     : message;
        n.bodyLong = message;
        n.sourceDate = date;

        dataSourceBuffer.clear();

        // Queue for loop() rather than calling onNotification() directly: this
        // callback runs on NimBLE's host task, and the notifier chain ends in a
        // synchronous HTTP request (BarkNotifier). Blocking that task for the
        // duration of a network round-trip would stall all BLE processing —
        // same class of problem as the write-deadlock described above, just
        // less immediately fatal.
        pendingNotifications.push_back(n);
    }

    // ANCS can redeliver the same notification more than once (e.g. on reconnect).
    // Keep a small circular buffer of recently-seen UIDs to drop repeats silently.
    static const size_t kDedupeBufferSize = 10;

    bool isDuplicate(uint32_t uid) const {
        for (size_t i = 0; i < kDedupeBufferSize; i++) {
            if (recentUids[i] == uid) return true;
        }
        return false;
    }

    void rememberUid(uint32_t uid) {
        recentUids[uidIndex] = uid;
        uidIndex = (uidIndex + 1) % kDedupeBufferSize;
    }

    // Retry schedule for connectAncsClient() — see loop().
    static const uint8_t kMaxClientConnectAttempts = 8; // ~20s of retries total
    uint32_t nextClientAttemptMillis = 0;
    uint8_t clientConnectAttempts = 0;

    // Backlog-suppression window — see handleNotificationSource(). Catches
    // the immediate burst of "Added" events ANCS resends right at (re)connect.
    // Slower-trickling backlog (arriving after this window, e.g. an
    // attribute response that took a while) is caught by isStaleNotification()
    // below instead.
    static const uint32_t kBacklogSuppressWindowMs = 15000;
    uint32_t ancsReadyMillis = 0;

    // Content-based backlog filter, layered on top of the window above.
    // ANCS's Date attribute is the phone's LOCAL time, not UTC — confirmed via
    // live testing: an earlier version of this compared it to UTC directly,
    // which made every real notification look hours old and silently dropped
    // everything. This corrects for that with a UTC offset that's either
    // user-configured (manualUtcOffsetFn) or auto-calibrated: two post-window
    // notifications agreeing (once quantized to the nearest 15 minutes — real
    // timezones sit on 15/30/45/60-minute boundaries) are required before the
    // offset is trusted, since a single sample could itself be a
    // slow-trickling backlog item and calibrate against a wrong value.
    static const uint32_t kStaleNotificationThresholdSec = 120;
    std::function<bool(int16_t&)> manualUtcOffsetFn;
    bool utcOffsetKnown = false;
    int32_t utcOffsetSeconds = 0;
    int32_t pendingOffsetCandidate = 0;
    uint8_t pendingOffsetStreak = 0;

    // True if `rawDate` (an ANCS Date attribute) indicates the notification is
    // older than kStaleNotificationThresholdSec. Returns false (i.e. "not
    // stale, let it through") whenever the age can't be determined yet — no
    // Date attribute, clock not NTP-synced, or no trusted UTC offset yet —
    // so this only ever adds filtering on top of the connect-timing window,
    // never removes the safety net of forwarding when unsure.
    bool isStaleNotification(const String& rawDate) {
        time_t naiveEpoch = parseAncsDateNaive(rawDate);
        if (naiveEpoch == 0) return false;

        time_t now = time(nullptr);
        static const time_t kMinPlausibleEpoch = 1700000000; // 2023-11-14, guards an unsynced clock
        if (now <= kMinPlausibleEpoch) return false;

        int16_t manualMinutes;
        if (manualUtcOffsetFn && manualUtcOffsetFn(manualMinutes)) {
            utcOffsetSeconds = (int32_t)manualMinutes * 60;
            utcOffsetKnown = true;
        } else if (!utcOffsetKnown) {
            int32_t candidate = (int32_t)(naiveEpoch - now);
            static const int32_t kMinOffsetSec = -12 * 3600;
            static const int32_t kMaxOffsetSec = 14 * 3600;
            if (candidate >= kMinOffsetSec && candidate <= kMaxOffsetSec) {
                int32_t quantized = ((candidate + (candidate >= 0 ? 450 : -450)) / 900) * 900;
                if (pendingOffsetStreak > 0 && quantized == pendingOffsetCandidate) {
                    utcOffsetSeconds = quantized;
                    utcOffsetKnown = true;
                    Serial.printf("[ANCS] Auto-calibrated phone UTC offset: %ld min\n",
                                  (long)(utcOffsetSeconds / 60));
                } else {
                    pendingOffsetCandidate = quantized;
                    pendingOffsetStreak = 1;
                }
            }
        }

        if (!utcOffsetKnown) return false;

        time_t realCreatedUtc = naiveEpoch - utcOffsetSeconds;
        time_t ageSec = now - realCreatedUtc;
        return ageSec > (time_t)kStaleNotificationThresholdSec;
    }

    NotificationCallback onNotification;
    NimBLEServer* server = nullptr;
    NimBLEClient* ancsClient = nullptr;
    NimBLEAddress peerAddress;
    uint16_t peerConnHandle = 0;

    NimBLERemoteCharacteristic* notificationSourceChar = nullptr;
    NimBLERemoteCharacteristic* controlPointChar = nullptr;
    NimBLERemoteCharacteristic* dataSourceChar = nullptr;

    volatile bool peerConnected = false;
    volatile bool pendingClientConnect = false;
    volatile bool ancsReady = false;

    // Set by BLE callbacks, consumed by loop() — see the comment there.
    volatile bool hasPendingConnectEvent = false;
    NimBLEAddress pendingPeerAddress;
    uint16_t pendingConnHandle = 0;
    volatile bool hasPendingDisconnectEvent = false;
    int pendingDisconnectReason = 0;

    uint32_t pendingUid = 0;
    std::vector<uint8_t> dataSourceBuffer;

    // Deferred from BLE notify callbacks to loop() — see the comments on
    // handleNotificationSource() and handleDataSource().
    volatile bool hasPendingAttributeRequest = false;
    uint32_t pendingAttributeRequestUid = 0;
    std::vector<Notification> pendingNotifications;

    // Initialized to a value no real ANCS UID collides with at boot (uid 0 is valid).
    uint32_t recentUids[kDedupeBufferSize] = {
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint8_t uidIndex = 0;
};
