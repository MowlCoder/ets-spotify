// ui/marquee.hpp: a title that does not fit moves back and forth.

#include <cmath>

#include "check.hpp"
#include "ui/marquee.hpp"

using test::Check;

namespace {
bool Approx(float a, float b) { return std::fabs(a - b) < 0.5f; }
}  // namespace

TEST_CASE("marquee", MarqueeTest) {
    const ui::Marquee m{100.0f};  // 100 units per second, holds of 2 s and 1.5 s

    // 200 units too wide: 2 s to travel each way. Cycle: 2 s hold, 2 s there, 1.5 s hold, 2 s back.
    Check(ui::MarqueeOffset(0.0f, 5000, m) == 0.0f && ui::MarqueeOffset(-3.0f, 5000, m) == 0.0f,
          "a line that fits does not move");
    Check(ui::MarqueeOffset(200.0f, 0, m) == 0.0f && ui::MarqueeOffset(200.0f, 1999, m) == 0.0f,
          "holds at the start first");
    Check(Approx(ui::MarqueeOffset(200.0f, 3000, m), 100.0f), "moves at the given speed");
    Check(Approx(ui::MarqueeOffset(200.0f, 4100, m), 200.0f) &&
              Approx(ui::MarqueeOffset(200.0f, 5400, m), 200.0f),
          "holds at the end, the last character visible");
    Check(Approx(ui::MarqueeOffset(200.0f, 6500, m), 100.0f), "moves back");
    Check(Approx(ui::MarqueeOffset(200.0f, 7600, m), 0.0f) &&
              ui::MarqueeOffset(200.0f, 7600 + 1000, m) == 0.0f,
          "back at the start, the cycle repeats");

    bool inside = true;
    for (uint64_t t = 0; t < 20000; t += 37) {
        const float x = ui::MarqueeOffset(200.0f, t, m);
        inside &= x >= 0.0f && x <= 200.0f;
    }
    Check(inside, "never beyond either end");
}
