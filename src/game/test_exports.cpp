// Entry points for the offline test harness (tests/harness.cpp), which plays the game: it loads the
// plugin into its own D3D11 device and drives these instead of hotkeys. Not used by the game.

#include "d3d/hooks.hpp"
#include "diagnostics/capture.hpp"
#include "diagnostics/screen_probe.hpp"
#include "screen/calibration.hpp"
#include "screen/nav_screen.hpp"
#include "spotify/spotify.hpp"

extern "C" __declspec(dllexport) void ets_spotify_debug_set_probe(int mode, int selected) {
    probe::DebugSet(mode, selected);
}

extern "C" __declspec(dllexport) int ets_spotify_debug_candidate_count() {
    return probe::CandidateCount();
}

extern "C" __declspec(dllexport) unsigned ets_spotify_debug_hooked_slots(void* context) {
    return hooks::HookedSlotMask(context);
}

extern "C" __declspec(dllexport) void ets_spotify_debug_nav(int enabled) {
    nav::SetEnabled(enabled != 0);
}

extern "C" __declspec(dllexport) void ets_spotify_debug_capture(int frames) {
    capture::Request(frames);
}

// A paused track with this title and artist, as if Spotify reported it (UTF-8).
extern "C" __declspec(dllexport) void ets_spotify_debug_show_track(const char* title,
                                                                   const char* artist) {
    spotify::State state;
    state.status = spotify::Status::Track;
    state.title = title;
    state.artist = artist;
    state.durationMs = 200000;
    state.progressMs = 60000;
    spotify::PublishForTest(state);
}

// 1 begin, 2 next item, 3 '.', 4 ',', 0 finish (save).
extern "C" __declspec(dllexport) void ets_spotify_debug_calibrate(int action) {
    switch (action) {
        case 1: calibration::Begin(); break;
        case 2: calibration::NextItem(); break;
        case 3: calibration::Adjust(+1); break;
        case 4: calibration::Adjust(-1); break;
        default: calibration::Finish(); break;
    }
}
