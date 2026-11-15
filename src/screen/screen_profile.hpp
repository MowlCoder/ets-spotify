#pragma once

#include "screen/screen_window.hpp"

// Screen settings of the current truck in ets_spotify.ini.
//
//   [nav.<truck id>]   written by the calibration mode (screen/calibration.hpp)
//   u0 u1 v0 v1        visible window of the texture
//   aspect=1.000       on-screen width / height of a texture pixel (the screen may stretch it)
//   display=256x256    which display texture carries the picture
//   display_pinned=1   that display was picked by hand: use it even when it does not show the map
//                      (trucks without a navigation screen: the instrument cluster display)
//
// Trucks without their own section use [nav], and without that the built-in default window.
namespace screen_profile {
bool HasOwn();  // the current truck has its own section
screen::Window LoadWindow();
bool PreferredDisplay(unsigned* width, unsigned* height, bool* pinned);
void Save(const screen::Window& window, unsigned displayWidth, unsigned displayHeight, bool pinned);
}  // namespace screen_profile
