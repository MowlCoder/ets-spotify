#pragma once
// A line of text that does not fit its box moves back and forth: where it is at a given moment.
// Pure code: unit-tested in tests/unit.

#include <cstdint>

namespace ui {

struct Marquee {
    float speed;                  // units per second
    uint64_t holdStartMs = 2000;  // still at the start, so the beginning can be read
    uint64_t holdEndMs = 1500;    // still at the end before going back
};

// How far the line is shifted left, `elapsedMs` after it appeared, when it is `overflow` units
// wider than its box: hold at the start, move to the end, hold, move back, repeat.
inline float MarqueeOffset(float overflow, uint64_t elapsedMs, const Marquee& m) {
    if (overflow <= 0.0f || m.speed <= 0.0f) { return 0.0f; }
    const uint64_t travel = static_cast<uint64_t>(overflow / m.speed * 1000.0f) + 1;
    const uint64_t cycle = m.holdStartMs + travel + m.holdEndMs + travel;

    uint64_t t = elapsedMs % cycle;
    if (t < m.holdStartMs) { return 0.0f; }
    t -= m.holdStartMs;
    if (t < travel) { return overflow * static_cast<float>(t) / static_cast<float>(travel); }
    t -= travel;
    if (t < m.holdEndMs) { return overflow; }
    t -= m.holdEndMs;
    return overflow * (1.0f - static_cast<float>(t) / static_cast<float>(travel));
}

}  // namespace ui
