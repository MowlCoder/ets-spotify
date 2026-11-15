#include "screen/screen_profile.hpp"

#include <cstdlib>
#include <format>
#include <string>

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "game/vehicle.hpp"

namespace screen_profile {
namespace {
constexpr const wchar_t* kShared = L"nav";

// "[nav.<id>]" for a known truck, "[nav]" before the game reported one.
std::wstring OwnSection() {
    const vehicle::Info truck = vehicle::Current();
    return truck.id.empty() ? kShared : text::Widen(vehicle::SectionFor(truck.id));
}

std::wstring Format(float value) { return std::format(L"{:.3f}", value); }
}  // namespace

bool HasOwn() {
    const std::wstring section = OwnSection();
    std::wstring value;
    return section != kShared && config::Find(section.c_str(), L"u0", &value);
}

screen::Window LoadWindow() {
    const std::wstring own = OwnSection();
    const bool hasOwn = HasOwn();
    auto get = [&](const wchar_t* key, float fallback) {
        std::wstring value;
        if (hasOwn && config::Find(own.c_str(), key, &value) && !value.empty()) {
            return static_cast<float>(_wtof(value.c_str()));
        }
        return config::GetFloat(kShared, key, fallback);
    };

    const screen::Window w{
        get(L"u0", screen::kDefaultWindow.u0), get(L"u1", screen::kDefaultWindow.u1),
        get(L"v0", screen::kDefaultWindow.v0), get(L"v1", screen::kDefaultWindow.v1),
        get(L"aspect", screen::kDefaultWindow.aspect)};
    if (!screen::Sane(w)) {
        logx::Log(
            "[screen] bad window in ets_spotify.ini [{}] (u {:.3f}..{:.3f}, v {:.3f}..{:.3f}, "
            "aspect {:.2f}), using the default",
            text::Narrow(hasOwn ? own : kShared), w.u0, w.u1, w.v0, w.v1, w.aspect);
        return screen::kDefaultWindow;
    }
    return w;
}

bool PreferredDisplay(unsigned* width, unsigned* height, bool* pinned) {
    const std::wstring section = OwnSection();
    std::wstring value;
    if (section == kShared || !config::Find(section.c_str(), L"display", &value) ||
        !screen::ParseSize(value, width, height)) {
        return false;
    }
    *pinned = config::Find(section.c_str(), L"display_pinned", &value) && value == L"1";
    return true;
}

void Save(const screen::Window& w, unsigned displayWidth, unsigned displayHeight, bool pinned) {
    const std::wstring section = OwnSection();
    config::Set(section.c_str(), L"truck", text::Widen(vehicle::Describe(vehicle::Current())));
    config::Set(section.c_str(), L"u0", Format(w.u0));
    config::Set(section.c_str(), L"u1", Format(w.u1));
    config::Set(section.c_str(), L"v0", Format(w.v0));
    config::Set(section.c_str(), L"v1", Format(w.v1));
    config::Set(section.c_str(), L"aspect", Format(w.aspect));
    if (displayWidth && displayHeight) {
        config::Set(section.c_str(), L"display",
                    std::format(L"{}x{}", displayWidth, displayHeight));
        config::Set(section.c_str(), L"display_pinned", pinned ? L"1" : L"0");
    }

    logx::Log(
        "[screen] settings saved to [{}]{}: u {:.3f}..{:.3f} v {:.3f}..{:.3f} aspect {:.2f} "
        "display {}x{}{}",
        text::Narrow(section), section == kShared ? " (truck unknown)" : "", w.u0, w.u1, w.v0, w.v1,
        w.aspect, displayWidth, displayHeight, pinned ? " pinned" : "");
}
}  // namespace screen_profile
