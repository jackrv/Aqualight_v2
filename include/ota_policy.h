#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Kept in the firmware image by /status; prevents flashing an unrelated .bin.
constexpr char FIRMWARE_MARKER[] = "AQUALIGHT:ESP8266:D1_MINI:OTA_V1";
constexpr uint32_t OTA_WINDOW_MS = 5 * 60 * 1000;

class OtaWindow {
    bool armed = false;
    uint32_t since = 0;
public:
    void arm(uint32_t now) { since = now; armed = true; }
    void cancel() { armed = false; }
    bool active(uint32_t now) const { return armed && uint32_t(now - since) < OTA_WINDOW_MS; }
};

inline bool parseFirmwareMd5(const char* caption, char output[33]) {
    if (!caption || strlen(caption) != 36 || strncmp(caption, "md5:", 4)) return false;
    for (size_t i = 0; i < 32; ++i) {
        char c = caption[i + 4];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        output[i] = c;
    }
    output[32] = '\0';
    return true;
}

class FirmwareMarkerScanner {
    size_t matched = 0;
    bool found = false;
    uint8_t prefix[sizeof(FIRMWARE_MARKER) - 1] = {};
public:
    FirmwareMarkerScanner() {
        size_t length = 0;
        for (size_t i = 1; i < sizeof(FIRMWARE_MARKER) - 1; ++i) {
            while (length && FIRMWARE_MARKER[i] != FIRMWARE_MARKER[length]) length = prefix[length - 1];
            if (FIRMWARE_MARKER[i] == FIRMWARE_MARKER[length]) ++length;
            prefix[i] = length;
        }
    }
    void add(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size && !found; ++i) {
            while (matched && data[i] != FIRMWARE_MARKER[matched]) matched = prefix[matched - 1];
            if (data[i] == FIRMWARE_MARKER[matched]) ++matched;
            found = matched == sizeof(FIRMWARE_MARKER) - 1;
        }
    }
    bool valid() const { return found; }
};
