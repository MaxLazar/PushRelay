// PushRelay v0.5 — oleg@abramov.dev
// In-memory ring buffer of the last N notifications, with full detail, for the
// admin UI's Log tab. Reset on reboot, like Stats.
#pragma once

#include <Arduino.h>
#include <vector>

#include "notifiers.h"

struct LogEntry {
    String appName;
    String title;
    String body;
    uint32_t uid;
    String priority;
    String status; // "forwarded" | "filtered_app" | "filtered_dnd" | "failed"
    uint32_t timestampMs; // millis() at the time this was logged
};

class NotificationLog {
public:
    static const size_t kCapacity = 20;

    void add(const Notification& n, const String& priority, const String& status) {
        entries[nextIndex] = LogEntry{n.appName, n.title, n.body, n.uid, priority, status, millis()};
        nextIndex = (nextIndex + 1) % kCapacity;
        if (count < kCapacity) count++;
    }

    // Most recent first.
    std::vector<LogEntry> recent() const {
        std::vector<LogEntry> out;
        out.reserve(count);
        for (size_t i = 0; i < count; i++) {
            size_t idx = (nextIndex + kCapacity - 1 - i) % kCapacity;
            out.push_back(entries[idx]);
        }
        return out;
    }

private:
    LogEntry entries[kCapacity];
    size_t nextIndex = 0;
    size_t count = 0;
};
