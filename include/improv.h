// PushRelay v0.5 — oleg@abramov.dev
// Minimal Improv-serial implementation (https://www.improv-wifi.com/serial/).
//
// This lets the browser-based flasher (ESP Web Tools) talk to the device over
// the same USB cable right after flashing: it can push WiFi credentials, and
// once the device is on the network it reports back its URL so the flasher
// shows a clickable "Visit device" link (http://192.168.x.x). When nothing is
// speaking Improv, none of this fires and the normal WiFiManager captive
// portal still handles setup.
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <initializer_list>

class ImprovSerial {
public:
    void begin(const char* fwName, const char* fwVersion, const char* deviceName) {
        _fwName = fwName;
        _fwVersion = fwVersion;
        _deviceName = deviceName;
    }

    // Pump the serial parser. Returns true once any Improv frame has been seen,
    // so the caller can widen its provisioning window while a flasher is
    // actively talking to us.
    bool loop() {
        while (Serial.available()) {
            parseByte((uint8_t)Serial.read());
        }
        return _sawTraffic;
    }

    bool sawTraffic() const { return _sawTraffic; }
    bool isConnected() const { return _connected; }

    // Call once WiFi is up (by any path) so later state queries return the URL.
    void setConnected(const IPAddress& ip) {
        _connected = true;
        _url = String("http://") + ip.toString();
    }

private:
    // --- protocol constants -------------------------------------------------
    static const uint8_t TYPE_CURRENT_STATE = 0x01;
    static const uint8_t TYPE_ERROR_STATE   = 0x02;
    static const uint8_t TYPE_RPC           = 0x03;
    static const uint8_t TYPE_RPC_RESULT    = 0x04;

    static const uint8_t STATE_AUTHORIZED   = 0x02;
    static const uint8_t STATE_PROVISIONING = 0x03;
    static const uint8_t STATE_PROVISIONED  = 0x04;

    static const uint8_t ERROR_INVALID_RPC       = 0x01;
    static const uint8_t ERROR_UNKNOWN_RPC       = 0x02;
    static const uint8_t ERROR_UNABLE_TO_CONNECT = 0x03;

    static const uint8_t CMD_WIFI_SETTINGS = 0x01;
    static const uint8_t CMD_REQUEST_STATE = 0x02;
    static const uint8_t CMD_REQUEST_INFO  = 0x03;
    static const uint8_t CMD_REQUEST_SCAN  = 0x04;

    // Header "IMPROV" + version + type + length = 9 bytes, then <=255 data, then checksum.
    uint8_t  _buf[266];
    uint16_t _pos = 0;
    uint8_t  _type = 0;
    uint8_t  _len = 0;
    bool     _sawTraffic = false;
    bool     _connected = false;
    String   _url;
    const char* _fwName = "firmware";
    const char* _fwVersion = "0";
    const char* _deviceName = "device";

    // --- frame parser -----------------------------------------------------
    void parseByte(uint8_t b) {
        static const char HEADER[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

        if (_pos < 6) {
            if (b == (uint8_t)HEADER[_pos]) {
                _buf[_pos++] = b;
            } else {
                // Not Improv (or noise) — resync only on a fresh 'I'.
                _pos = (b == 'I') ? 1 : 0;
                if (_pos == 1) _buf[0] = 'I';
            }
            return;
        }

        _buf[_pos] = b;

        if (_pos == 6) { _pos++; return; }              // version byte
        if (_pos == 7) { _type = b; _pos++; return; }   // packet type
        if (_pos == 8) { _len = b; _pos++; return; }    // data length

        if (_pos < (uint16_t)(9 + _len)) { _pos++; return; }  // data bytes

        // Final byte is the checksum.
        uint8_t sum = 0;
        for (uint16_t i = 0; i < (uint16_t)(9 + _len); i++) sum += _buf[i];
        _pos = 0;
        _sawTraffic = true;

        if (sum != b) { sendError(ERROR_INVALID_RPC); return; }
        if (_type == TYPE_RPC) handleRpc(&_buf[9], _len);
    }

    // --- RPC dispatch ---------------------------------------------------
    void handleRpc(const uint8_t* data, uint8_t len) {
        if (len < 2) { sendError(ERROR_INVALID_RPC); return; }
        uint8_t cmd = data[0];
        const uint8_t* d = &data[2];
        uint8_t dlen = data[1];

        switch (cmd) {
            case CMD_REQUEST_STATE:
                if (_connected) {
                    sendState(STATE_PROVISIONED);
                    sendRpcResult(CMD_REQUEST_STATE, {_url});
                } else {
                    sendState(STATE_AUTHORIZED);
                }
                break;

            case CMD_REQUEST_INFO:
                sendRpcResult(CMD_REQUEST_INFO,
                              {String(_fwName), String(_fwVersion), String("ESP32"), String(_deviceName)});
                break;

            case CMD_REQUEST_SCAN:
                doScan();
                break;

            case CMD_WIFI_SETTINGS: {
                if (dlen < 2) { sendError(ERROR_INVALID_RPC); return; }
                uint8_t ssidLen = d[0];
                String ssid = bytesToString(d + 1, ssidLen);
                uint8_t passLen = d[1 + ssidLen];
                String pass = bytesToString(d + 2 + ssidLen, passLen);
                connectWifi(ssid, pass);
                break;
            }

            default:
                sendError(ERROR_UNKNOWN_RPC);
        }
    }

    void connectWifi(const String& ssid, const String& pass) {
        sendState(STATE_PROVISIONING);
        WiFi.persistent(true);          // save creds so normal boots reconnect
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            setConnected(WiFi.localIP());
            sendState(STATE_PROVISIONED);
            sendRpcResult(CMD_WIFI_SETTINGS, {_url});
        } else {
            sendError(ERROR_UNABLE_TO_CONNECT);
        }
    }

    void doScan() {
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n && i < 30; i++) {
            bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
            sendRpcResult(CMD_REQUEST_SCAN,
                          {WiFi.SSID(i), String(WiFi.RSSI(i)), String(open ? "NO" : "YES")});
            delay(1);
        }
        sendRpcResult(CMD_REQUEST_SCAN, {});   // empty result terminates the list
        WiFi.scanDelete();
    }

    // --- outbound framing ---------------------------------------------
    void sendPacket(uint8_t type, const uint8_t* data, uint8_t len) {
        uint8_t hdr[9] = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, len};
        uint8_t sum = 0;
        for (int i = 0; i < 9; i++) sum += hdr[i];
        for (int i = 0; i < len; i++) sum += data[i];
        Serial.write(hdr, 9);
        if (len) Serial.write(data, len);
        Serial.write(sum);
        Serial.write('\n');
        Serial.flush();
    }

    void sendState(uint8_t state) { sendPacket(TYPE_CURRENT_STATE, &state, 1); }
    void sendError(uint8_t err)   { sendPacket(TYPE_ERROR_STATE, &err, 1); }

    void sendRpcResult(uint8_t cmd, std::initializer_list<String> strings) {
        uint8_t data[256];
        uint16_t p = 0;
        data[p++] = cmd;
        uint16_t lenPos = p++;              // filled in after we know the blob size
        for (const String& s : strings) {
            uint8_t sl = (uint8_t)s.length();
            if (p + 1 + sl > sizeof(data)) break;
            data[p++] = sl;
            memcpy(&data[p], s.c_str(), sl);
            p += sl;
        }
        data[lenPos] = (uint8_t)(p - lenPos - 1);
        sendPacket(TYPE_RPC_RESULT, data, (uint8_t)p);
    }

    static String bytesToString(const uint8_t* p, uint8_t len) {
        String s;
        s.reserve(len);
        for (uint8_t i = 0; i < len; i++) s += (char)p[i];
        return s;
    }
};
