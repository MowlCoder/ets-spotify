#pragma once
#include <memory>
#include <string>

namespace cabin {

// Spotify's audio sessions in the Windows volume mixer (WASAPI), on every active output device.
// Each session keeps the player's own volume (cabin/cabin_logic.hpp: VolumeTracker), so the mod can
// scale it and give it back. Use from one thread with COM initialized.
class SpotifyMixer {
public:
    SpotifyMixer();
    ~SpotifyMixer();
    SpotifyMixer(const SpotifyMixer&) = delete;
    SpotifyMixer& operator=(const SpotifyMixer&) = delete;

    // Finds Spotify's sessions again (Spotify may have started, restarted or switched devices).
    void Refresh();
    // Sets every session to the player's volume x `factor`, first picking up changes the player
    // made. `seenFactor` is the factor the player heard until now.
    void Apply(float factor, float seenFactor);
    // Puts back the player's own volume.
    void Restore(float seenFactor) { Apply(1.0f, seenFactor); }
    // " (mixer 0.70)" as the mixer shows it now, for the log.
    std::string Describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cabin
