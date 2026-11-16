#pragma once
#include <stdint.h>
#include <stddef.h>

// Wall clock kept in RTC user memory. It survives OTA restarts, watchdog and
// exception resets (not a power loss), so the light resumes before NTP answers.
struct SavedClock {
    uint32_t magic;
    uint32_t epoch;
    uint32_t check;
};
constexpr uint32_t SAVED_CLOCK_MAGIC = 0x41514C54; // "AQLT"
constexpr uint32_t SAVED_CLOCK_MIN_EPOCH = 1700000000;

inline SavedClock packSavedClock(uint32_t epoch) {
    return {SAVED_CLOCK_MAGIC, epoch, ~(SAVED_CLOCK_MAGIC ^ epoch)};
}

inline bool unpackSavedClock(const SavedClock& saved, uint32_t& epoch) {
    if (saved.magic != SAVED_CLOCK_MAGIC || saved.check != ~(saved.magic ^ saved.epoch) ||
        saved.epoch <= SAVED_CLOCK_MIN_EPOCH) return false;
    epoch = saved.epoch;
    return true;
}

// Setup access point: shown only after Wi-Fi has been unreachable this long.
constexpr uint32_t WIFI_PORTAL_DELAY_MS = 3 * 60 * 1000;

// WPA2 limits: SSID 1…32 bytes; password empty (open network) or 8…63 bytes.
inline bool wifiCredentialsValid(size_t ssidLength, size_t passLength) {
    return ssidLength >= 1 && ssidLength <= 32 &&
        (passLength == 0 || (passLength >= 8 && passLength <= 63));
}
