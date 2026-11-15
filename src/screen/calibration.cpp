#include "screen/calibration.hpp"

#include <windows.h>

#include <mutex>

#include "core/immortal.hpp"
#include "core/log.hpp"
#include "game/vehicle.hpp"
#include "screen/nav_screen.hpp"
#include "screen/screen_profile.hpp"
#include "ui/ui_renderer.hpp"

namespace calibration {
namespace {
constexpr float kStep = 0.004f;       // about one pixel of a 256 px display texture
constexpr float kAspectStep = 0.02f;  // 2% wider or narrower

std::mutex& g_mutex = core::Immortal<std::mutex>();  // guards g_view and g_screenWasOn
View& g_view = core::Immortal<View>();
bool g_screenWasOn = false;

const char* ItemName(Item item) {
    switch (item) {
        case Item::Left: return "left edge";
        case Item::Right: return "right edge";
        case Item::Top: return "top edge";
        case Item::Bottom: return "bottom edge";
        case Item::Proportions: return "proportions";
        default: return "display";
    }
}
}  // namespace

bool Active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_view.active;
}

View Current() {
    View v;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        v = g_view;
    }
    nav::TargetSize(&v.displayWidth, &v.displayHeight);
    return v;
}

void Begin() {
    const vehicle::Info truck = vehicle::Current();
    const screen::Window window = screen_profile::LoadWindow();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_view.active) { return; }
        g_view.active = true;
        g_view.item = Item::Left;
        g_view.window = window;
        g_view.truck = vehicle::Describe(truck);
        g_screenWasOn = nav::Enabled();
    }
    nav::SetEnabled(true);

    logx::Log(
        "[calib] start: {} (id {}, {}), window u {:.3f}..{:.3f} v {:.3f}..{:.3f} aspect {:.2f}",
        vehicle::Describe(truck), truck.id.empty() ? "?" : truck.id.c_str(),
        screen_profile::HasOwn() ? "own settings" : "using [nav]", window.u0, window.u1, window.v0,
        window.v1, window.aspect);
    MessageBeep(MB_ICONEXCLAMATION);
    ui::RequestRedraw();
}

void Finish() {
    screen::Window window;
    bool screenWasOn;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_view.active) { return; }
        g_view.active = false;
        window = g_view.window;
        screenWasOn = g_screenWasOn;
    }

    unsigned width = 0;
    unsigned height = 0;
    nav::TargetSize(&width, &height);
    screen_profile::Save(window, width, height, nav::TargetPinned());

    if (!screenWasOn) { nav::SetEnabled(false); }
    MessageBeep(MB_OK);
    ui::ReloadSettings();  // the saved window
}

void NextItem() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_view.active) { return; }
        g_view.item =
            static_cast<Item>((static_cast<int>(g_view.item) + 1) % static_cast<int>(Item::Count));
        logx::Log("[calib] adjusting: {}", ItemName(g_view.item));
    }
    ui::RequestRedraw();
}

void Adjust(int direction) {
    Item item;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_view.active) { return; }
        item = g_view.item;
        if (item == Item::Proportions) {
            g_view.window = screen::ChangeAspect(g_view.window, direction * kAspectStep);
        } else if (item != Item::Screen) {
            static_assert(static_cast<int>(Item::Bottom) == static_cast<int>(screen::Edge::Bottom),
                          "Item starts with screen::Edge");
            g_view.window =
                screen::MoveEdge(g_view.window, static_cast<screen::Edge>(item), direction * kStep);
        }
    }

    if (item == Item::Screen) {
        nav::CycleCandidate(direction);  // the render thread switches and asks for a redraw
    } else {
        ui::RequestRedraw();
    }
}
}  // namespace calibration
