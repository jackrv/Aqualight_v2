#include <cassert>
#include <iostream>
#include <string>
#include "ota_policy.h"

int main() {
    OtaWindow window;
    assert(!window.active(0));
    window.arm(50);
    assert(window.active(50));
    assert(window.active(50 + OTA_WINDOW_MS - 1));
    assert(!window.active(50 + OTA_WINDOW_MS));
    window.arm(UINT32_MAX - 10);
    assert(window.active(10)); // millis rollover
    window.cancel();
    assert(!window.active(11));
    char md5[33];
    assert(parseFirmwareMd5("md5:0123456789ABCDEF0123456789abcdef", md5));
    assert(std::string(md5) == "0123456789abcdef0123456789abcdef");
    assert(!parseFirmwareMd5(nullptr, md5));
    assert(!parseFirmwareMd5("md5:0123", md5));
    assert(!parseFirmwareMd5("md5:0123456789abcdef0123456789abcdef ", md5));
    assert(!parseFirmwareMd5("md5:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", md5));
    const std::string marker = FIRMWARE_MARKER;
    for (size_t split = 0; split <= marker.size(); ++split) {
        FirmwareMarkerScanner scanner;
        scanner.add(reinterpret_cast<const uint8_t*>(marker.data()), split);
        scanner.add(reinterpret_cast<const uint8_t*>(marker.data()+split), marker.size()-split);
        assert(scanner.valid());
    }
    FirmwareMarkerScanner bytes;
    const std::string stream = "AAAxAQU" + marker + "trailing binary";
    for (auto c : stream) { auto value=static_cast<uint8_t>(c); bytes.add(&value,1); }
    assert(bytes.valid());
    FirmwareMarkerScanner other;
    const std::string wrong="ESP32 firmware, filesystem image, AQUALIGHT:ESP8266:OTHER";
    other.add(reinterpret_cast<const uint8_t*>(wrong.data()),wrong.size());
    assert(!other.valid());
    int64_t cursor = 0;
    assert(parseTelegramCursor("1", cursor) && cursor == 1);
    assert(parseTelegramCursor("2147483648", cursor) && cursor == 2147483648LL);
    assert(!parseTelegramCursor("2147483649", cursor));
    assert(!parseTelegramCursor("99999999999999999999", cursor));
    assert(!parseTelegramCursor("123bad", cursor));
    assert(!parseTelegramCursor("-1", cursor));
    assert(!parseTelegramCursor("0", cursor));
    assert(!parseTelegramCursor("", cursor));
    assert(!parseTelegramCursor(nullptr, cursor));
    OtaProgress progress;
    assert(!progress.advance(0, 1000));
    assert(!progress.advance(99, 1000));
    assert(progress.advance(100, 1000));
    assert(!progress.advance(199, 1000));
    assert(progress.advance(250, 1000));
    assert(!progress.advance(260, 1000));
    assert(progress.advance(999, 1000));
    assert(progress.advance(1000, 1000));
    assert(!progress.advance(1000, 1000));
    assert(progress.percent(500, 1000) == 50);
    assert(progress.percent(1200, 1000) == 100);
    assert(progress.percent(0, 0) == 0);
    std::cout << "OTA window, MD5, image marker and progress thresholds: PASS\n";
}
