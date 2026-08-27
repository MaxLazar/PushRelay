// PushRelay — admin-UI "pull" OTA self-update.
//
// The device fetches a small JSON manifest from GitHub Pages, compares its
// version with the running firmware, and (on request) streams the new
// firmware/filesystem images straight into the inactive OTA slot, verifying
// each against the SHA-256 in the manifest before committing it.
//
// All network work happens from loop() (see OtaUpdater::loop), never inside an
// async web handler — the web handlers only set a pending command and return.
// A full download blocks the main loop for a few seconds to a minute; that is
// well within the 5-minute task watchdog / BLE-recovery windows, and the
// watchdog is fed explicitly while streaming.
#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>

#include "config.h"
#include "version.h"

class OtaUpdater {
public:
    enum class Phase {
        Idle,            // nothing has been checked yet this boot
        Checking,        // fetching the manifest
        UpToDate,        // checked, running the latest version
        UpdateAvailable, // checked, a newer version is published
        Downloading,     // streaming an image into flash
        Applying,        // images written, about to reboot
        Error,           // last check or apply failed (see error())
    };

    // Called from loop(). Runs at most one pending command per iteration.
    void loop() {
        const Cmd cmd = pending_;
        if (cmd == Cmd::None) return;
        pending_ = Cmd::None;
        if (cmd == Cmd::Check) doCheck();
        else if (cmd == Cmd::Apply) doApply();
    }

    // Queued by the web handlers; picked up on the next loop().
    void requestCheck() { pending_ = Cmd::Check; }
    void requestApply() { pending_ = Cmd::Apply; }

    // --- state, serialized into /api/ota-status ---
    Phase phase() const { return phase_; }
    const String& latestVersion() const { return manifestVersion_; }
    const String& releaseNotes() const { return notes_; }
    const String& error() const { return error_; }
    int progress() const { return progress_; }        // 0..100 for the current image, -1 if unknown
    const String& stage() const { return stage_; }    // human label for the current image

    bool updateAvailable() const {
        return manifestVersion_.length() && cmpSemver(manifestVersion_, FIRMWARE_VERSION) > 0;
    }

    static const char* phaseName(Phase p) {
        switch (p) {
            case Phase::Checking:        return "checking";
            case Phase::UpToDate:        return "up_to_date";
            case Phase::UpdateAvailable: return "update_available";
            case Phase::Downloading:     return "downloading";
            case Phase::Applying:        return "applying";
            case Phase::Error:           return "error";
            case Phase::Idle:
            default:                     return "idle";
        }
    }

private:
    enum class Cmd { None, Check, Apply };

    volatile Cmd pending_ = Cmd::None;
    Phase phase_ = Phase::Idle;
    int progress_ = -1;
    String stage_;
    String error_;

    // Last manifest seen, reused by an Apply that follows a Check.
    String manifestVersion_;
    String notes_;
    String fwUrl_, fwSha_;
    String fsUrl_, fsSha_;
    size_t fwSize_ = 0, fsSize_ = 0;

    // Resolves a manifest "path" (relative or absolute) against OTA_BASE_URL.
    static String resolveUrl(const String& path) {
        if (path.startsWith("http://") || path.startsWith("https://")) return path;
        String p = path;
        while (p.startsWith("/")) p.remove(0, 1);
        return String(OTA_BASE_URL) + p;
    }

    // Numeric compare of "x.y.z" strings; ignores a leading 'v' and any
    // "-suffix" (e.g. "-dev"). Returns <0, 0, >0.
    static int cmpSemver(const String& a, const String& b) {
        int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
        parseSemver(a, pa);
        parseSemver(b, pb);
        for (int i = 0; i < 3; i++) {
            if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
        }
        return 0;
    }

    static void parseSemver(const String& s, int out[3]) {
        int start = 0;
        if (start < (int)s.length() && (s[start] == 'v' || s[start] == 'V')) start++;
        for (int part = 0; part < 3; part++) {
            long val = 0;
            bool any = false;
            while (start < (int)s.length() && isDigit(s[start])) {
                val = val * 10 + (s[start] - '0');
                start++;
                any = true;
            }
            out[part] = any ? (int)val : 0;
            if (start < (int)s.length() && s[start] == '.') start++;
            else break;
        }
    }

    void fail(const String& msg) {
        phase_ = Phase::Error;
        error_ = msg;
        progress_ = -1;
        Serial.printf("[OTA] %s\n", msg.c_str());
    }

    void doCheck() {
        phase_ = Phase::Checking;
        error_ = "";
        Serial.printf("[OTA] Checking %s\n", OTA_MANIFEST_URL);

        WiFiClientSecure client;
        client.setInsecure();               // integrity comes from the SHA-256 check
        client.setHandshakeTimeout(15);

        HTTPClient http;
        http.setTimeout(15000);
        http.setReuse(false);
        if (!http.begin(client, OTA_MANIFEST_URL)) {
            fail("manifest: connection failed");
            return;
        }
        http.addHeader("Cache-Control", "no-cache");
        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            http.end();
            fail(String("manifest: HTTP ") + code);
            return;
        }
        const String payload = http.getString();
        http.end();

        JsonDocument doc;
        if (deserializeJson(doc, payload) != DeserializationError::Ok) {
            fail("manifest: invalid JSON");
            return;
        }

        manifestVersion_ = doc["version"] | "";
        notes_ = doc["notes"] | "";
        fwUrl_ = resolveUrl(String(doc["firmware"]["path"] | ""));
        fwSha_ = String(doc["firmware"]["sha256"] | "");
        fwSize_ = doc["firmware"]["size"] | 0;
        fsUrl_ = resolveUrl(String(doc["filesystem"]["path"] | ""));
        fsSha_ = String(doc["filesystem"]["sha256"] | "");
        fsSize_ = doc["filesystem"]["size"] | 0;

        if (!manifestVersion_.length() || !fwSha_.length() || fwUrl_.endsWith("/")) {
            fail("manifest: missing fields");
            return;
        }

        if (updateAvailable()) {
            phase_ = Phase::UpdateAvailable;
            Serial.printf("[OTA] Update available: %s -> %s\n", FIRMWARE_VERSION, manifestVersion_.c_str());
        } else {
            phase_ = Phase::UpToDate;
            Serial.printf("[OTA] Up to date (running %s)\n", FIRMWARE_VERSION);
        }
    }

    void doApply() {
        if (!manifestVersion_.length()) {
            doCheck();
            if (phase_ == Phase::Error) return;
        }
        if (!updateAvailable()) {
            fail("apply: no newer version to install");
            return;
        }

        error_ = "";

        // Filesystem image first (holds the admin page), then firmware. A
        // failure at any point aborts without rebooting, leaving the running
        // slots untouched. The filesystem image is the full 1.5 MB partition,
        // so it's only re-flashed when its hash actually changed since the last
        // update this device applied (tracked in NVS).
        if (fsUrl_.length() && fsSha_.length() && !fsUrl_.endsWith("/")) {
            Preferences p;
            p.begin("ota", true);
            const String appliedFsSha = p.getString("fsSha", "");
            p.end();
            if (appliedFsSha.equalsIgnoreCase(fsSha_)) {
                Serial.println("[OTA] Filesystem image unchanged, skipping");
            } else if (streamToFlash(fsUrl_, fsSha_, fsSize_, U_SPIFFS, "filesystem")) {
                p.begin("ota", false);
                p.putString("fsSha", fsSha_);
                p.end();
            } else {
                return;
            }
        }
        if (!streamToFlash(fwUrl_, fwSha_, fwSize_, U_FLASH, "firmware")) return;

        phase_ = Phase::Applying;
        progress_ = 100;
        Serial.printf("[OTA] Update to %s written, rebooting\n", manifestVersion_.c_str());
        delay(1200);
        ESP.restart();
    }

    // Downloads `url` into the OTA partition selected by `command`, hashing the
    // stream and refusing to commit unless it matches `expectedShaHex`.
    bool streamToFlash(const String& url, const String& expectedShaHex, size_t expectedSize,
                       int command, const char* label) {
        phase_ = Phase::Downloading;
        stage_ = label;
        progress_ = expectedSize ? 0 : -1;
        Serial.printf("[OTA] Downloading %s image: %s\n", label, url.c_str());

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(15);

        HTTPClient http;
        http.setTimeout(20000);
        http.setReuse(false);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        if (!http.begin(client, url)) {
            fail(String(label) + ": connection failed");
            return false;
        }
        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            http.end();
            fail(String(label) + ": HTTP " + code);
            return false;
        }

        int len = http.getSize();               // -1 if the server omits Content-Length
        if (expectedSize && len > 0 && (size_t)len != expectedSize) {
            http.end();
            fail(String(label) + ": size mismatch");
            return false;
        }
        const size_t total = expectedSize ? expectedSize : (len > 0 ? (size_t)len : 0);

        if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, command)) {
            http.end();
            fail(String(label) + ": no space (" + Update.errorString() + ")");
            return false;
        }

        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
        mbedtls_md_starts(&md);

        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[1024];
        size_t written = 0;
        uint32_t lastData = millis();

        while (http.connected() && (len > 0 || len == -1)) {
            const size_t avail = stream->available();
            if (avail) {
                const int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
                if (n <= 0) break;
                mbedtls_md_update(&md, buf, n);
                if (Update.write(buf, n) != (size_t)n) {
                    mbedtls_md_free(&md);
                    Update.abort();
                    http.end();
                    fail(String(label) + ": flash write failed (" + Update.errorString() + ")");
                    return false;
                }
                written += n;
                if (len > 0) len -= n;
                if (total) progress_ = (int)((uint64_t)written * 100 / total);
                lastData = millis();
            } else {
                if (millis() - lastData > 20000) {
                    mbedtls_md_free(&md);
                    Update.abort();
                    http.end();
                    fail(String(label) + ": stalled");
                    return false;
                }
                delay(5);
            }
            esp_task_wdt_reset();
        }
        http.end();

        uint8_t digest[32];
        mbedtls_md_finish(&md, digest);
        mbedtls_md_free(&md);

        if (total && written != total) {
            Update.abort();
            fail(String(label) + ": truncated download");
            return false;
        }

        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
        if (!expectedShaHex.equalsIgnoreCase(hex)) {
            Update.abort();
            fail(String(label) + ": SHA-256 mismatch");
            return false;
        }

        if (!Update.end(true)) {
            fail(String(label) + ": finalize failed (" + Update.errorString() + ")");
            return false;
        }
        Serial.printf("[OTA] %s image OK (%u bytes)\n", label, (unsigned)written);
        progress_ = 100;
        return true;
    }
};
