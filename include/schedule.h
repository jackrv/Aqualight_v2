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

// The curve through the points is a monotone cubic spline (PCHIP): it passes
// exactly through every point, never overshoots, has no corners, and does not
// stop at a point that lies on a continuing rise or fall. It levels off only
// where the direction changes or a plateau begins. The day wraps around, so
// midnight is as smooth as any other point. web/schedule.js draws the same curve.
inline float segmentSlope(const Channel& ch, uint8_t k) {
    return (float(ch.points[k + 1].v) - float(ch.points[k].v)) /
        float(ch.points[k + 1].t - ch.points[k].t);
}

// Slope of the curve at point k, in schedule units per minute.
inline float pointTangent(const Channel& ch, uint8_t k) {
    const uint8_t last = ch.count - 1;              // points[0] and points[last] are both midnight
    const uint8_t before = k == 0 ? last - 1 : k - 1;   // segment that ends at the point
    const uint8_t after = k == last ? 0 : k;            // segment that starts at it
    const float d0 = segmentSlope(ch, before), d1 = segmentSlope(ch, after);
    if (d0 * d1 <= 0) return 0;
    const float h0 = ch.points[before + 1].t - ch.points[before].t;
    const float h1 = ch.points[after + 1].t - ch.points[after].t;
    const float w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
    return (w1 + w2) / (w1 / d0 + w2 / d1);
}

inline uint16_t interpolateFine(const Channel& ch, uint32_t second) {
    for (uint8_t i = 1; i < ch.count; ++i) {
        const Point& a = ch.points[i - 1];
        const Point& b = ch.points[i];
        if (second <= uint32_t(b.t) * 60) {
            if (a.v == b.v) return uint16_t(a.v) * FINE_STEPS;
            const float h = b.t - a.t;
            const float p = float(second - a.t * 60UL) / (h * 60.0f);
            const float p2 = p * p, p3 = p2 * p;
            float value = (2 * p3 - 3 * p2 + 1) * a.v + (p3 - 2 * p2 + p) * h * pointTangent(ch, i - 1) +
                (3 * p2 - 2 * p3) * b.v + (p3 - p2) * h * pointTangent(ch, i);
            const float low = a.v < b.v ? a.v : b.v, high = a.v < b.v ? b.v : a.v;
            if (value < low) value = low;
            if (value > high) value = high;
            return static_cast<uint16_t>(value * FINE_STEPS + 0.5f);
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
