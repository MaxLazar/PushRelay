// PushRelay v0.5 — oleg@abramov.dev
// Non-blocking onboard LED status indicator (GPIO2 on ESP32-DevKitC-VE).
#pragma once

#include <Arduino.h>

enum class LedState {
    Connecting, // slow blink (1s) — waiting for BLE pair
    Connected,  // solid on — BLE connected, all OK
    Error,      // fast blink (200ms) — WiFi lost / provider failed
};

class LedIndicator {
public:
    void begin(uint8_t ledPin = 2) {
        pin = ledPin;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }

    void setState(LedState s) { state = s; }

    // Triggers a brief double-blink overlay (notification sent), then resumes
    // whatever base pattern `state` describes.
    void pulseSuccess() {
        pulsing = true;
        pulseStep = 0;
        pulseStepStart = millis();
    }

    // Call every loop() iteration; never blocks.
    void update() {
        uint32_t now = millis();

        if (pulsing) {
            renderPulse(now);
            return;
        }

        switch (state) {
            case LedState::Connecting: blink(now, 1000); break;
            case LedState::Connected: digitalWrite(pin, HIGH); break;
            case LedState::Error: blink(now, 200); break;
        }
    }

private:
    void blink(uint32_t now, uint32_t periodMs) {
        digitalWrite(pin, (now / periodMs) % 2 == 0 ? HIGH : LOW);
    }

    // Double blink: on/off/on/off, 100ms per step, then falls back to the base pattern.
    void renderPulse(uint32_t now) {
        static const uint32_t stepDurations[] = {100, 100, 100, 100};
        static const uint8_t stepLevels[] = {HIGH, LOW, HIGH, LOW};
        const uint8_t stepCount = 4;

        if (now - pulseStepStart >= stepDurations[pulseStep]) {
            pulseStep++;
            pulseStepStart = now;
            if (pulseStep >= stepCount) {
                pulsing = false;
                return;
            }
        }
        digitalWrite(pin, stepLevels[pulseStep]);
    }

    uint8_t pin = 2;
    LedState state = LedState::Connecting;

    bool pulsing = false;
    uint8_t pulseStep = 0;
    uint32_t pulseStepStart = 0;
};
