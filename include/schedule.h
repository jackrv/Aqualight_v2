#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <ArduinoJson.h>
constexpr size_t MAX_POINTS = 12;
constexpr size_t MAX_SCHEDULE_BYTES = 2048;
struct Point { uint16_t t; uint8_t v; };
struct Channel { Point points[MAX_POINTS]; uint8_t count = 0; };
struct Schedule { Channel channels[4]; };

// The entire candidate is validated before touching the active schedule or flash.
inline bool parseSchedule(JsonVariantConst root, Schedule& candidate) {
    if (!root.is<JsonObjectConst>() || root.size() != 4) return false;
    for (uint8_t c = 0; c < 4; ++c) {
        char key[] = {'c', 'h', char('0' + c), '\0'};
        JsonArrayConst points = root[key].as<JsonArrayConst>();
        if (points.size() < 2 || points.size() > MAX_POINTS) return false;
        Channel& channel = candidate.channels[c];
        channel.count = points.size();
        for (size_t i = 0; i < points.size(); ++i) {
            JsonVariantConst p = points[i];
            if (!p.is<JsonObjectConst>() || p.size() != 2 ||
                !p["t"].is<int>() || !p["v"].is<int>()) return false;
            int t = p["t"].as<int>(), v = p["v"].as<int>();
            if (t < 0 || t > 1440 || v < 0 || v > 255 ||
                (i && t <= channel.points[i - 1].t)) return false;
            channel.points[i] = {static_cast<uint16_t>(t), static_cast<uint8_t>(v)};
        }
        if (channel.points[0].t != 0 || channel.points[channel.count - 1].t != 1440 ||
            channel.points[0].v != channel.points[channel.count - 1].v) return false;
    }
    return true;
}


// Direction of the segment that follows `second`: +1 rising, -1 falling, 0 steady.
inline int8_t trend(const Channel& ch, uint32_t second) {
    for (uint8_t i = 1; i < ch.count; ++i) {
        if (second < uint32_t(ch.points[i].t) * 60) {
            int delta = int(ch.points[i].v) - int(ch.points[i - 1].v);
            return delta > 0 ? 1 : delta < 0 ? -1 : 0;
        }
    }
    return 0;
}

// The schedule stays 0…255; the output is finer. Interpolating in 1/64 steps and
// applying gamma on a 10-bit duty hides the stairs of a slow sunrise.
constexpr uint16_t FINE_STEPS = 64;
constexpr uint16_t FINE_MAX = 255 * FINE_STEPS;
constexpr uint16_t PWM_RANGE = 1023;

// Cosine ease-in-out: a transition starts and ends with zero speed, so the
// brightness has no corners at the schedule points. The web editor draws the
// same curve (web/schedule.js).
inline float easeInOut(float progress) {
    return (1.0f - cosf(progress * 3.14159265f)) * 0.5f;
}

inline uint16_t interpolateFine(const Channel& ch, uint32_t second) {
    for (uint8_t i = 1; i < ch.count; ++i) {
        const Point& a = ch.points[i - 1];
        const Point& b = ch.points[i];
        if (second <= uint32_t(b.t) * 60) {
            if (a.v == b.v) return uint16_t(a.v) * FINE_STEPS;
            float progress = float(second - a.t * 60UL) / float((b.t - a.t) * 60UL);
            float value = (a.v + (int(b.v) - int(a.v)) * easeInOut(progress)) * FINE_STEPS;
            return static_cast<uint16_t>(value + 0.5f);
        }
    }
    return 0;
}

// The same level in schedule units 0…255.
inline uint8_t interpolate(const Channel& ch, uint32_t second) {
    return static_cast<uint8_t>((interpolateFine(ch, second) + FINE_STEPS / 2) / FINE_STEPS);
}

// Perceived brightness -> PWM duty (gamma 2.2). Anything above zero stays lit.
inline uint16_t gammaDuty(uint16_t fine) {
    if (!fine) return 0;
    if (fine >= FINE_MAX) return PWM_RANGE;
    uint16_t duty = static_cast<uint16_t>(powf(float(fine) / FINE_MAX, 2.2f) * PWM_RANGE + 0.5f);
    return duty ? duty : 1;
}
