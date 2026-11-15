#pragma once
#include <string>

#include "screen/screen_window.hpp"

// Calibration mode: fits the picture to the screen of the current truck.
//
// Hold the screen key (F8) to start. The screen shows a frame: pink outside it, coloured lines on
// its edges, and two circles. The play/pause key (F9) picks what to adjust: the left, right, top
// and bottom edge, the proportions (until the circles look round) or which display; next/previous
// ('.', ',') change it. The screen key saves the result to ets_spotify.ini [nav.<truck id>]
// (screen/screen_profile.hpp) and leaves.
namespace calibration {
enum class Item { Left, Right, Top, Bottom, Proportions, Screen, Count };

struct View {
    bool active = false;
    Item item = Item::Left;
    screen::Window window = screen::kDefaultWindow;
    std::string truck;  // "Iveco S-Way"
    unsigned displayWidth = 0;
    unsigned displayHeight = 0;
};

bool Active();
View Current();
void Begin();
void Finish();
void NextItem();
void Adjust(int direction);  // +1: edge right/down, wider, or the next display; -1: the other way
}  // namespace calibration
