#pragma once
#include <Arduino.h>
#include "secrets.h"
constexpr char FIRMWARE_VERSION[] = "2026.11.16-ota-test-1";
// Wiring of the installed aquarium light: white, red, green, blue.
constexpr uint8_t PWM_PINS[] = {D1, D7, D5, D6};
// DS18B20 water temperature sensor (data line).
constexpr uint8_t WATER_TEMP_PIN = D2;
constexpr char TIMEZONE[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
#include "schedule.h"
