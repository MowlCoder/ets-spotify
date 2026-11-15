#pragma once

// Hotkeys, polled on a background thread while the game window is in the foreground.
// Keys come from [keys] in ets_spotify.ini (names: see input/keymap.hpp):
//
//   screen=F8          tap: Spotify on the navigation screen on/off (first time: login in the
//   browser)
//                      hold: fit the picture to this truck's screen (screen/calibration.hpp)
//   play_pause=F9      play / pause            (only while driving, not in menus)
//   next=PERIOD        next track              (only while driving, not in menus)
//   previous=COMMA     previous track          (only while driving, not in menus)
//
// While fitting the picture, play_pause picks the edge and next/previous move it.
// Diagnostics, only with [debug] tools=1: Shift+screen probe mode, Shift+play_pause next candidate,
// Insert confirm, Home frame capture (diagnostics/).
#include <string>

namespace input {
void Start();
void Stop();
std::wstring ScreenKeyName();  // "F8", for texts on the screen; any thread
}  // namespace input
