#pragma once

// The truck's radio behaviour, on its own thread (reads game/game_state.hpp):
//  - auto-pause: Spotify pauses while the game is paused (menu, loading) or the ignition is off,
//  and
//    resumes afterwards, but only if it was playing when the mod paused it;
//  - speed volume: Spotify gets louder with speed, through its session in the Windows mixer
//  (instant
//    and local, so only when Spotify plays on this PC). The player's volume is restored on exit.
// Settings: [autopause] and [volume] in ets_spotify.ini, re-read every few seconds.
namespace cabin {
void Start();
void Stop();  // restores Spotify's volume
}  // namespace cabin
