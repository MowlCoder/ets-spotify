#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "game/game_state.hpp"

// Pure rules for the cabin behaviour (unit-tested in tests/unit).
namespace cabin {

struct Settings {
    bool pauseOnGamePause = true;    // [autopause] game_pause
    bool pauseOnIgnitionOff = true;  // [autopause] ignition_off
    bool speedVolume = true;         // [volume] speed_volume
    float standstill = 0.70f;        // [volume] standstill: volume factor when stopped
    float fullKmh = 90.0f;           // [volume] full_kmh: speed where the factor reaches 1.0
};

// Volume factor for a speed: `standstill` when stopped, 1.0 from fullKmh up, linear in between.
// Reverse counts like forward.
inline float SpeedFactor(float kmh, const Settings& s) {
    const float low = std::clamp(s.standstill, 0.0f, 1.0f);
    if (!(s.fullKmh > 0.0f) || std::isnan(kmh)) { return 1.0f; }
    const float t = std::clamp(std::fabs(kmh) / s.fullKmh, 0.0f, 1.0f);
    return low + (1.0f - low) * t;
}

// Moves `current` towards `target` by at most `maxStep` (smooth volume changes).
inline float Approach(float current, float target, float maxStep) {
    return current < target ? std::min(current + maxStep, target)
                            : std::max(current - maxStep, target);
}

// What the cabin knows about the game at a moment.
struct Situation {
    bool hasDriven =
        false;  // the player has been in the truck this session (not the start-up menu)
    bool gamePaused = false;
    game::Ignition ignition = game::Ignition::Unknown;
};

inline bool WantAutoPause(const Situation& now, const Settings& s) {
    if (!now.hasDriven) { return false; }
    if (s.pauseOnGamePause && now.gamePaused) { return true; }
    return s.pauseOnIgnitionOff && now.ignition == game::Ignition::Off;
}

// One Spotify audio session in the Windows mixer: the player's own volume and what the mod set.
// The player (Spotify app, the phone via Spotify Connect, the mixer) may change the volume at any
// time; such a change becomes the player's new volume. Spotify Connect also syncs the volume
// through its servers, so a value the mod set a moment ago can come back late: that echo is not the
// player's choice.
class VolumeTracker {
public:
    enum class Seen { Ours, Echo, Foreign };
    static constexpr float kForeignDelta =
        0.01f;  // the mixer differs from our value by more: not ours
    static constexpr float kEchoTolerance = 0.006f;  // Spotify keeps the volume in whole percent
    static constexpr uint64_t kEchoWindowMs = 4000;

    explicit VolumeTracker(float playerVolume = 1.0f)
        : player_(playerVolume), applied_(playerVolume) {}

    // The mixer shows `current` while the speed factor is `factor`.
    Seen Observe(float current, float factor, uint64_t now) {
        if (std::fabs(current - applied_) <= kForeignDelta) { return Seen::Ours; }
        std::erase_if(recent_, [now](const std::pair<uint64_t, float>& r) {
            return now - r.first > kEchoWindowMs;
        });
        for (const auto& r : recent_) {
            if (std::fabs(r.second - current) <= kEchoTolerance) {
                applied_ = current;  // an older value of ours: Want() re-applies the current one
                return Seen::Echo;
            }
        }

        // The player set `current` while hearing `factor` of their volume.
        player_ = std::clamp(factor > 0.05f ? current / factor : current, 0.0f, 1.0f);
        applied_ = current;
        return Seen::Foreign;
    }

    // The mixer value for this factor, or a negative number when the mixer already has it.
    float Want(float factor) const {
        const float target = std::clamp(player_ * factor, 0.0f, 1.0f);
        return std::fabs(target - applied_) < 0.004f ? -1.0f : target;
    }

    void Applied(float value, uint64_t now) {
        applied_ = value;
        recent_.emplace_back(now, value);
    }

    float Player() const { return player_; }
    float LastApplied() const { return applied_; }

private:
    float
        player_;  // the player's own volume: what plays at full speed and what is restored on exit
    float applied_;  // the value the mixer had after our last set (or the last value we saw)
    std::vector<std::pair<uint64_t, float>> recent_;  // values we set in the last few seconds
};

}  // namespace cabin
