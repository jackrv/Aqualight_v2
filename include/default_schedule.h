#pragma once
#ifndef PROGMEM
#define PROGMEM
#endif

// Starting schedule, written to /schedule.json when the board has none (first
// start after an OTA from another firmware, or a damaged file). It must stay
// identical to data/schedule.json (the default preset of web/presets.js);
// tests/schedule_test.cpp and tests/web_test.js compare them.
static const char DEFAULT_SCHEDULE_JSON[] PROGMEM =
    R"JSON({"ch0":[{"t":0,"v":0},{"t":480,"v":0},{"t":600,"v":240},{"t":1080,"v":240},{"t":1200,"v":0},{"t":1440,"v":0}],"ch1":[{"t":0,"v":0},{"t":450,"v":0},{"t":570,"v":220},{"t":1110,"v":220},{"t":1230,"v":0},{"t":1440,"v":0}],"ch2":[{"t":0,"v":0},{"t":480,"v":0},{"t":600,"v":160},{"t":1080,"v":160},{"t":1200,"v":0},{"t":1440,"v":0}],"ch3":[{"t":0,"v":0},{"t":480,"v":0},{"t":600,"v":200},{"t":1080,"v":200},{"t":1200,"v":40},{"t":1320,"v":40},{"t":1380,"v":0},{"t":1440,"v":0}]})JSON";
