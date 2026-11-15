// cabin/cabin_logic.hpp: auto-pause rules, speed volume, the player's volume vs. ours.

#include <cmath>

#include "cabin/cabin_logic.hpp"
#include "check.hpp"

using test::Check;

namespace {
bool Approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }
}  // namespace

TEST_CASE("cabin rules", CabinRules) {
    using game::Ignition;
    const cabin::Settings s;  // defaults: 70% when stopped, full volume from 90 km/h

    Check(Approx(cabin::SpeedFactor(0, s), 0.70f), "stopped: standstill factor");
    Check(Approx(cabin::SpeedFactor(45, s), 0.85f), "half way: linear");
    Check(cabin::SpeedFactor(90, s) == 1.0f && cabin::SpeedFactor(130, s) == 1.0f,
          "full volume from 90 km/h");
    Check(Approx(cabin::SpeedFactor(-45, s), 0.85f), "reverse counts like forward");
    Check(cabin::SpeedFactor(NAN, s) == 1.0f, "unknown speed: no change");

    Check(Approx(cabin::Approach(0.70f, 1.0f, 0.03f), 0.73f) &&
              Approx(cabin::Approach(0.80f, 0.70f, 0.03f), 0.77f),
          "Approach moves by one step");
    Check(cabin::Approach(0.99f, 1.0f, 0.03f) == 1.0f, "Approach does not overshoot");

    Check(!cabin::WantAutoPause({false, true, Ignition::Off}, s),
          "not driven yet (start-up menu): never pauses");
    Check(cabin::WantAutoPause({true, true, Ignition::On}, s), "game paused: pause");
    Check(cabin::WantAutoPause({true, false, Ignition::Off}, s), "ignition off: pause");
    Check(!cabin::WantAutoPause({true, false, Ignition::Unknown}, s), "ignition unknown: no pause");
    Check(!cabin::WantAutoPause({true, false, Ignition::On}, s), "driving: no pause");

    cabin::Settings off = s;
    off.pauseOnGamePause = false;
    off.pauseOnIgnitionOff = false;
    Check(!cabin::WantAutoPause({true, true, Ignition::Off}, off),
          "both rules switched off: never pauses");
}

TEST_CASE("volume tracker", VolumeTracker) {
    using cabin::VolumeTracker;

    VolumeTracker t(1.0f);
    Check(Approx(t.Want(0.70f), 0.70f), "stopped: sets 70% of the player's volume");
    t.Applied(0.70f, 0);
    Check(t.Observe(0.70f, 0.70f, 250) == VolumeTracker::Seen::Ours && t.Want(0.70f) < 0,
          "own value in the mixer: nothing to do");

    // The phone (Spotify Connect) or the Windows mixer puts Spotify back to 100% while the truck
    // stands.
    Check(t.Observe(1.00f, 0.70f, 20000) == VolumeTracker::Seen::Foreign,
          "outside change detected");
    Check(Approx(t.Player(), 1.0f) && Approx(t.Want(0.70f), 0.70f),
          "after an outside reset to 100%: 70% applied again (seen in game)");
    t.Applied(0.70f, 20250);

    // The player turns it down to 35% while hearing 70%: their volume is now 50%.
    Check(
        t.Observe(0.35f, 0.70f, 30000) == VolumeTracker::Seen::Foreign && Approx(t.Player(), 0.5f),
        "turned down while stopped: becomes the player's volume");
    Check(t.Want(0.70f) < 0, "no jump right after the player's change");
    Check(Approx(t.Want(1.0f), 0.5f), "full speed / restore: the player's volume");

    // Spotify Connect sends an older value of ours back while the volume ramps up.
    VolumeTracker r(1.0f);
    r.Applied(0.73f, 1000);
    r.Applied(0.76f, 1250);
    Check(r.Observe(0.73f, 0.76f, 1800) == VolumeTracker::Seen::Echo && Approx(r.Player(), 1.0f),
          "late echo of our own value: not the player's choice");
    Check(Approx(r.Want(0.76f), 0.76f), "after an echo the current value is applied again");

    r.Applied(0.784f, 2000);
    Check(r.Observe(0.78f, 0.79f, 2300) == VolumeTracker::Seen::Ours,
          "whole-percent rounding is ours");
    Check(r.Observe(0.73f, 0.79f, 9000) == VolumeTracker::Seen::Foreign,
          "old values stop counting as echoes");
    Check(r.Observe(0.0f, 0.79f, 9500) == VolumeTracker::Seen::Foreign && r.Player() == 0.0f &&
              r.Want(1.0f) < 0,
          "muted in the mixer: stays muted");
}
