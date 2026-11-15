#pragma once
// Which of the cab's displays is the navigation screen: the heart of the mod, as pure code.
//
// How ETS2 draws its displays (frame capture, ETS2 1.62, Iveco with a dash-top GPS):
//
//   map render (1024x1024 render target, no mips, redrawn every frame)
//     -> display texture 256x256 -> display texture 256x256   (navigation)
//   instrument cluster UI -> display texture 512x512 -> display texture 512x512   (cluster)
//   scene pass: the last display textures of each chain are sampled by the screen meshes.
//
// "Display texture" = render target with GENERATE_MIPS, at most 1024 px. Display textures come from
// a pool and the game may hand them to another display (seen after the pause menu), so the
// navigation texture is found again every frame, from that frame's display passes only:
//
//   1. a display pass that samples a plain render target redrawn in the same frame shows the map
//      (a font cache or another rarely updated target does not count);
//   2. that property flows along display -> display sampling;
//   3. candidates are map displays that no other display pass samples (the end of a chain);
//   4. the choice sticks to the current texture; otherwise the size this truck used before wins,
//      otherwise the smallest (navigation screens are small).
//
// Trucks without a navigation screen still have displays, e.g. in the instrument cluster. In the
// calibration mode the player can pick any display at the end of a chain; such a choice is pinned:
// the display of that size is used whether it shows the map or not.
//
// screen/nav_screen.cpp records each frame into a FrameGraph and calls Analyze() and Choose().
// Unit-tested in tests/unit.

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nav {

using TextureId = const void*;  // a texture's address: identity only, never dereferenced

struct Size {
    unsigned width = 0;
    unsigned height = 0;
    unsigned Area() const { return width * height; }
    bool operator==(const Size&) const = default;
};

// What one frame did with render targets.
struct FrameGraph {
    std::unordered_set<TextureId> rendered;        // every render target bound this frame
    std::unordered_map<TextureId, Size> displays;  // display textures rendered this frame
    std::unordered_map<TextureId, std::unordered_set<TextureId>>
        displaySources;  // display -> displays it sampled
    std::unordered_map<TextureId, std::unordered_set<TextureId>>
        plainSources;  // display -> plain RTs it sampled

    void Clear() {
        rendered.clear();
        displays.clear();
        displaySources.clear();
        plainSources.clear();
    }
};

struct Analysis {
    std::unordered_set<TextureId> showsMap;  // displays fed by the map (steps 1-2)
    std::vector<TextureId> candidates;       // step 3, smallest first (ties by address: stable)
    std::vector<TextureId> ends;  // every display at the end of a chain: candidates first
};

// What this truck's settings say about its screen.
struct Preference {
    Size size;            // the display size used before (0x0: unknown)
    bool pinned = false;  // picked by hand: use the display of that size, map or not
};

inline Analysis Analyze(const FrameGraph& f) {
    Analysis a;
    for (const auto& [display, sources] : f.plainSources) {
        for (TextureId s : sources) {
            if (f.rendered.count(s)) {
                a.showsMap.insert(display);
                break;
            }
        }
    }

    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [display, sources] : f.displaySources) {
            if (a.showsMap.count(display)) { continue; }
            for (TextureId s : sources) {
                if (a.showsMap.count(s)) {
                    a.showsMap.insert(display);
                    changed = true;
                    break;
                }
            }
        }
    }

    std::unordered_set<TextureId> sampledByDisplay;
    for (const auto& [display, sources] : f.displaySources) {
        sampledByDisplay.insert(sources.begin(), sources.end());
    }

    std::vector<TextureId> others;  // chain ends without the map
    for (const auto& [display, size] : f.displays) {
        if (sampledByDisplay.count(display)) { continue; }
        (a.showsMap.count(display) ? a.candidates : others).push_back(display);
    }

    const auto bySize = [&f](TextureId x, TextureId y) {
        const unsigned ax = f.displays.at(x).Area();
        const unsigned ay = f.displays.at(y).Area();
        return ax != ay ? ax < ay : x < y;
    };
    std::sort(a.candidates.begin(), a.candidates.end(), bySize);
    std::sort(others.begin(), others.end(), bySize);
    a.ends = a.candidates;
    a.ends.insert(a.ends.end(), others.begin(), others.end());
    return a;
}

inline bool Contains(const std::vector<TextureId>& list, TextureId t) {
    return std::find(list.begin(), list.end(), t) != list.end();
}

// The display after `current` (+1) or before it (-1) in `list`, wrapping around.
inline TextureId Step(const std::vector<TextureId>& list, TextureId current, int direction) {
    const auto it = std::find(list.begin(), list.end(), current);
    const int n = static_cast<int>(list.size());
    int i = it == list.end() ? (direction > 0 ? -1 : 0) : static_cast<int>(it - list.begin());
    i = ((i + direction) % n + n) % n;
    return list[static_cast<size_t>(i)];
}

// The texture to put the picture on after this frame. Returns `current` when nothing changes and
// nullptr when there is none (or `current` is now drawn without the map: the game gave it to
// another display). pick: +1 / -1 switches to the next / previous display at the end of a chain
// (calibration; map displays first), 0 otherwise.
inline TextureId Choose(const FrameGraph& f, const Analysis& a, TextureId current,
                        const Preference& preference, int pick) {
    if (pick != 0 && !a.ends.empty()) { return Step(a.ends, current, pick); }

    if (preference.pinned && preference.size.Area() > 0) {  // the display picked for this truck
        if (Contains(a.ends, current) && f.displays.at(current) == preference.size) {
            return current;
        }
        for (TextureId t : a.ends) {
            if (f.displays.at(t) == preference.size) { return t; }
        }
        return f.displays.count(current) ? nullptr : current;
    }

    const std::vector<TextureId>& c = a.candidates;
    if (Contains(c, current)) {
        return current;  // sticky
    }
    if (!c.empty()) {
        for (TextureId t : c) {
            if (f.displays.at(t) == preference.size) { return t; }
        }
        return c.front();  // smallest
    }
    if (current && f.displays.count(current) && !a.showsMap.count(current)) { return nullptr; }
    return current;  // not drawn this frame (screen off, another camera): keep it
}

// " [00000123 256x256 map=1 samples: 1 display, 0 fresh plain, 0 stale plain] ..." for the log.
inline std::string Describe(const FrameGraph& f, const Analysis& a) {
    std::string out;
    for (const auto& [display, size] : f.displays) {
        size_t fresh = 0;
        size_t stale = 0;
        if (const auto p = f.plainSources.find(display); p != f.plainSources.end()) {
            fresh = static_cast<size_t>(
                std::count_if(p->second.begin(), p->second.end(),
                              [&f](TextureId s) { return f.rendered.contains(s); }));
            stale = p->second.size() - fresh;
        }
        const auto d = f.displaySources.find(display);
        out += std::format(" [{} {}x{} map={} samples: {} display, {} fresh plain, {} stale plain]",
                           display, size.width, size.height, a.showsMap.contains(display) ? 1 : 0,
                           d == f.displaySources.end() ? 0 : d->second.size(), fresh, stale);
    }
    return out;
}

}  // namespace nav
