// PushRelay v0.5 — oleg@abramov.dev
// In-memory notification statistics, reset on reboot.
#pragma once

#include <Arduino.h>
#include <algorithm>
#include <map>
#include <vector>

class Stats {
public:
    void recordReceived(const String& appName) {
        received++;
        appCounts[appName]++;
    }
    void recordForwarded() { forwarded++; }
    void recordFiltered() { filtered++; }

    // Highest-count apps first, capped at `limit`.
    std::vector<std::pair<String, uint32_t>> topApps(size_t limit = 5) const {
        std::vector<std::pair<String, uint32_t>> sorted(appCounts.begin(), appCounts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::pair<String, uint32_t>& a, const std::pair<String, uint32_t>& b) {
                      return a.second > b.second;
                  });
        if (sorted.size() > limit) sorted.resize(limit);
        return sorted;
    }

    uint32_t received = 0;
    uint32_t forwarded = 0;
    uint32_t filtered = 0;

private:
    std::map<String, uint32_t> appCounts;
};
