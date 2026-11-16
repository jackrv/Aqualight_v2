#include <cassert>
#include <iostream>
#include "recovery.h"

int main() {
    uint32_t epoch = 0;
    SavedClock saved = packSavedClock(1794839527);
    assert(unpackSavedClock(saved, epoch) && epoch == 1794839527);
    SavedClock blank{};
    assert(!unpackSavedClock(blank, epoch));
    SavedClock garbage{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    assert(!unpackSavedClock(garbage, epoch));
    SavedClock flipped = saved; flipped.epoch ^= 1;
    assert(!unpackSavedClock(flipped, epoch));
    SavedClock badCheck = saved; badCheck.check ^= 0x100;
    assert(!unpackSavedClock(badCheck, epoch));
    assert(!unpackSavedClock(packSavedClock(1000), epoch)); // before any real NTP time
    assert(epoch == 1794839527);

    assert(wifiCredentialsValid(1, 0));
    assert(wifiCredentialsValid(32, 8));
    assert(wifiCredentialsValid(10, 63));
    assert(!wifiCredentialsValid(0, 8));
    assert(!wifiCredentialsValid(33, 8));
    assert(!wifiCredentialsValid(10, 7));
    assert(!wifiCredentialsValid(10, 64));
    std::cout << "Clock backup and Wi-Fi credential rules: PASS\n";
}
