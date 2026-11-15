#pragma once
// The visible part of a cab screen's texture, how the screen stretches it, and where the player is
// laid out in it. Pure code: unit-tested in tests/unit.

#include <algorithm>
#include <cwchar>
#include <string>

namespace screen {

// Visible part of the texture on the screen mesh, in upright image coordinates (0..1), and how wide
// one texture pixel looks on that screen compared to its height (some screens stretch the texture).
struct Window {
    float u0;
    float u1;
    float v0;
    float v1;
    float aspect = 1.0f;
};

// Measured from an in-game screenshot (Iveco, dash-top GPS, ETS2 1.62).
constexpr Window kDefaultWindow{0.040f, 0.977f, 0.252f, 0.676f, 1.0f};
constexpr float kMinWindowSide = 0.1f;
constexpr float kMinAspect = 0.5f;
constexpr float kMaxAspect = 2.0f;

inline bool Sane(const Window& w) {
    return w.u0 >= 0 && w.u1 <= 1 && w.v0 >= 0 && w.v1 <= 1 &&
           w.u1 - w.u0 >= kMinWindowSide - 1e-4f && w.v1 - w.v0 >= kMinWindowSide - 1e-4f &&
           w.aspect >= kMinAspect && w.aspect <= kMaxAspect;
}

enum class Edge { Left, Right, Top, Bottom };

// Moves one edge by `delta`, keeping it inside the texture and the window at least kMinWindowSide
// wide.
inline Window MoveEdge(Window w, Edge edge, float delta) {
    switch (edge) {
        case Edge::Left: w.u0 = std::clamp(w.u0 + delta, 0.0f, w.u1 - kMinWindowSide); break;
        case Edge::Right: w.u1 = std::clamp(w.u1 + delta, w.u0 + kMinWindowSide, 1.0f); break;
        case Edge::Top: w.v0 = std::clamp(w.v0 + delta, 0.0f, w.v1 - kMinWindowSide); break;
        case Edge::Bottom: w.v1 = std::clamp(w.v1 + delta, w.v0 + kMinWindowSide, 1.0f); break;
    }
    return w;
}

inline Window ChangeAspect(Window w, float delta) {
    w.aspect = std::clamp(w.aspect + delta, kMinAspect, kMaxAspect);
    return w;
}

// Where the player is drawn, for a texture of texWidth x texHeight pixels.
//
// Sizes are in "screen units": texture pixels, with the horizontal ones multiplied by the window's
// aspect, so a square in screen units looks square on the cab screen. The painter draws in screen
// units with a horizontal scale of 1 / aspect.
//
// The box takes the window's full width but at most width / kDesignAspect of its height (the shape
// of the screen the layout was made for), centred vertically: on a tall screen the cover stays the
// same size relative to the text, with empty bands above and below.
struct LayoutBox {
    // texture pixels
    float left;
    float top;

    // screen units
    float width;
    float height;

    float aspect;
};

constexpr float kDesignAspect = 2.2f;  // Iveco dash-top GPS: 0.937 x 0.424 of a square texture

inline LayoutBox Layout(const Window& w, float texWidth, float texHeight) {
    const float width = (w.u1 - w.u0) * texWidth * w.aspect;
    const float fullHeight = (w.v1 - w.v0) * texHeight;
    const float height = std::min(fullHeight, width / kDesignAspect);
    return {w.u0 * texWidth, w.v0 * texHeight + (fullHeight - height) / 2, width, height, w.aspect};
}

// "256x256" -> 256, 256.
inline bool ParseSize(const std::wstring& text, unsigned* width, unsigned* height) {
    unsigned w = 0;
    unsigned h = 0;
    wchar_t tail = 0;
    if (swscanf_s(text.c_str(), L"%ux%u%c", &w, &h, &tail, 1) != 2 || w == 0 || h == 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

}  // namespace screen
