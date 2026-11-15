#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// The picture for the cab screen, painted on its own thread (ui/painter.hpp) whenever something
// changes and every second while a track plays. The render thread only uploads finished pixels
// (screen/nav_screen.cpp).
//
// The texture is square like the game's own navigation texture; the screen mesh shows only a window
// of it, which comes from the truck's screen settings (screen/screen_profile.hpp).
namespace ui {

struct Frame {
    uint64_t version;
    unsigned width;
    unsigned height;
    // BGRA8 (sRGB-encoded) pixels, rows already flipped for the display; mips[0] is full size.
    std::vector<std::vector<uint32_t>> mips;
};

// Plenty for a screen that is about 140x60 px at 1080p, and cheap to re-upload every second.
constexpr unsigned kWidth = 512;
constexpr unsigned kHeight = 512;

void Start();
void Stop();
void SetVisible(bool visible);  // the picture is on the cab screen (timer repaints only then)
void RequestRedraw();           // new state to show
void ReloadSettings();  // also re-read the screen window and the language (ini edited, truck
                        // changed)
std::shared_ptr<const Frame> Latest();  // nullptr until the first picture is ready

}  // namespace ui
