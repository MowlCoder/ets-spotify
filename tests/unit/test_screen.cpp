// screen/: which display is the navigation screen, and the visible window of its texture.

#include <cmath>

#include "check.hpp"
#include "screen/display_graph.hpp"
#include "screen/screen_window.hpp"

using test::Check;

namespace {
bool Approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }
bool NearPx(float a, float b) { return std::fabs(a - b) < 0.05f; }  // texture pixels

// Textures of one cab, as in the frame capture from the game (Iveco, dash-top GPS).
struct Cab {
    int map;
    int fontCache;
    int navInter;
    int navFinal;
    int clusterInter;
    int clusterFinal;
    static nav::TextureId Id(const int& texture) { return &texture; }

    // fontCache: both chains also sample a font cache that is not redrawn (as after the pause
    // menu). clusterMap: the cluster samples a target redrawn this frame too (as logged in game
    // after the menu).
    nav::FrameGraph Frame(bool fontCacheSampled, bool clusterMap) const {
        nav::FrameGraph f;
        f.rendered = {Id(map), Id(navInter), Id(navFinal), Id(clusterInter), Id(clusterFinal)};
        f.displays = {{Id(navInter), {256, 256}},
                      {Id(navFinal), {256, 256}},
                      {Id(clusterInter), {512, 512}},
                      {Id(clusterFinal), {512, 512}}};

        f.plainSources[Id(navInter)] = {Id(map)};
        if (fontCacheSampled) {
            f.plainSources[Id(navInter)].insert(Id(fontCache));
            f.plainSources[Id(clusterInter)].insert(Id(fontCache));
        }
        if (clusterMap) { f.plainSources[Id(clusterInter)].insert(Id(map)); }

        f.displaySources[Id(navFinal)] = {Id(navInter)};
        f.displaySources[Id(clusterFinal)] = {Id(clusterInter)};
        return f;
    }
};
}  // namespace

TEST_CASE("display graph", DisplayGraph) {
    using nav::Analyze;
    using nav::Choose;
    const Cab cab{};
    const nav::TextureId navFinal = Cab::Id(cab.navFinal);
    const nav::TextureId clusterFinal = Cab::Id(cab.clusterFinal);

    const nav::FrameGraph normal = cab.Frame(false, false);
    nav::Analysis a = Analyze(normal);
    Check(a.candidates.size() == 1 && a.candidates[0] == navFinal,
          "the map display at the end of its chain");
    Check(a.showsMap.count(Cab::Id(cab.navInter)) && !a.showsMap.count(clusterFinal),
          "the map flows along the nav chain, not into the cluster");
    Check(Choose(normal, a, nullptr, {}, 0) == navFinal, "found from scratch");
    Check(!nav::Describe(normal, a).empty(), "described for the log");

    const nav::FrameGraph menu = cab.Frame(true, false);
    a = Analyze(menu);
    Check(a.candidates.size() == 1 && a.candidates[0] == navFinal,
          "a font cache that is not redrawn does not make the cluster a map screen");

    const nav::FrameGraph both = cab.Frame(false, true);
    a = Analyze(both);
    Check(a.candidates.size() == 2 && a.candidates[0] == navFinal,
          "two map screens: smallest first");
    Check(Choose(both, a, nullptr, {}, 0) == navFinal, "no history: the smallest");
    Check(Choose(both, a, nullptr, nav::Preference{{512, 512}}, 0) == clusterFinal,
          "the size saved for this truck wins");
    Check(Choose(both, a, clusterFinal, {}, 0) == clusterFinal, "the current texture sticks");
    Check(Choose(both, a, navFinal, {}, +1) == clusterFinal &&
              Choose(both, a, clusterFinal, {}, +1) == navFinal,
          "picked by hand: cycles by size");
    Check(Choose(both, a, navFinal, {}, -1) == clusterFinal,
          "picked by hand backwards: wraps around");

    nav::FrameGraph reassigned =
        cab.Frame(false, false);  // the nav texture now shows something else
    reassigned.plainSources.clear();
    a = Analyze(reassigned);
    Check(a.candidates.empty() && Choose(reassigned, a, navFinal, {}, 0) == nullptr,
          "the texture is drawn without the map: dropped");

    const nav::FrameGraph quiet;  // nothing drawn: the screen is off or another camera is active
    a = Analyze(quiet);
    Check(Choose(quiet, a, navFinal, {}, 0) == navFinal, "not drawn this frame: kept");

    // A truck without a navigation screen (an older Mercedes Actros): only the cluster chain, no
    // map.
    nav::FrameGraph noNav = cab.Frame(false, false);
    noNav.plainSources.clear();
    noNav.displays.erase(Cab::Id(cab.navInter));
    noNav.displays.erase(navFinal);
    noNav.displaySources.erase(navFinal);
    a = Analyze(noNav);
    Check(a.candidates.empty() && a.ends.size() == 1 && a.ends[0] == clusterFinal,
          "no map screen: the cluster is the only display at the end of a chain");
    Check(Choose(noNav, a, nullptr, {}, 0) == nullptr,
          "a display without the map is not taken by itself");
    Check(Choose(noNav, a, nullptr, {}, +1) == clusterFinal, "but it can be picked by hand");

    const nav::Preference pinned{{512, 512}, true};
    Check(Choose(noNav, a, nullptr, pinned, 0) == clusterFinal &&
              Choose(noNav, a, clusterFinal, pinned, 0) == clusterFinal,
          "pinned: used and kept without the map");

    a = Analyze(normal);
    Check(a.ends.size() == 2 && a.ends[0] == navFinal &&
              Choose(normal, a, navFinal, {}, +1) == clusterFinal,
          "picking by hand reaches displays without the map too, after the map screens");
    Check(Choose(normal, a, clusterFinal, pinned, 0) == clusterFinal,
          "a pinned cluster stays even where the navigation screen shows the map");
}

TEST_CASE("screen window", ScreenWindow) {
    const screen::Window w = screen::kDefaultWindow;
    Check(screen::Sane(w), "default window is sane");

    Check(Approx(screen::MoveEdge(w, screen::Edge::Left, 0.004f).u0, 0.044f) &&
              Approx(screen::MoveEdge(w, screen::Edge::Bottom, -0.004f).v1, 0.672f),
          "edges move by the step");
    Check(screen::MoveEdge(w, screen::Edge::Left, -1.0f).u0 == 0.0f &&
              screen::MoveEdge(w, screen::Edge::Right, 1.0f).u1 == 1.0f,
          "edges stop at the texture border");
    const screen::Window squeezed = screen::MoveEdge(w, screen::Edge::Top, 1.0f);
    Check(Approx(squeezed.v1 - squeezed.v0, screen::kMinWindowSide) && screen::Sane(squeezed),
          "window never thinner than the minimum");

    Check(!screen::Sane(screen::Window{0.5f, 0.55f, 0.0f, 1.0f}) &&
              !screen::Sane(screen::Window{-0.1f, 0.9f, 0.1f, 0.9f}),
          "bad windows rejected");
    Check(Approx(screen::ChangeAspect(w, 0.2f).aspect, 1.2f) &&
              screen::ChangeAspect(w, 5.0f).aspect == screen::kMaxAspect &&
              !screen::Sane(screen::Window{0.0f, 1.0f, 0.0f, 1.0f, 3.0f}),
          "proportions change within limits");

    // The player's box: the Iveco window is wider than the design shape, so the box is the whole
    // window.
    screen::LayoutBox box = screen::Layout(w, 512, 512);
    Check(NearPx(box.left, 0.040f * 512) && NearPx(box.top, 0.252f * 512) &&
              NearPx(box.width, 0.937f * 512) && NearPx(box.height, 0.424f * 512),
          "wide screen: the box is the whole window");

    // Mercedes Actros: a taller window that also stretches the texture 1.2x sideways.
    const screen::Window actros{0.008f, 0.995f, 0.134f, 0.794f, 1.2f};
    box = screen::Layout(actros, 512, 512);
    const float fullHeight = (0.794f - 0.134f) * 512;
    Check(NearPx(box.width, 0.987f * 512 * 1.2f) &&
              std::fabs(box.width / box.height - screen::kDesignAspect) < 1e-3f,
          "tall screen: the box keeps the design shape in screen units");
    Check(box.height < fullHeight && NearPx(box.top - 0.134f * 512, (fullHeight - box.height) / 2),
          "tall screen: the box is centred vertically");

    unsigned width = 0;
    unsigned height = 0;
    Check(screen::ParseSize(L"256x256", &width, &height) && width == 256 && height == 256,
          "display size parsed");
    Check(!screen::ParseSize(L"256", &width, &height) &&
              !screen::ParseSize(L"0x256", &width, &height) &&
              !screen::ParseSize(L"256x256px", &width, &height),
          "bad display sizes rejected");
}
