#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "screen/calibration.hpp"
#include "screen/screen_window.hpp"
#include "spotify/spotify.hpp"
#include "ui/ui_renderer.hpp"

namespace ui {

// Paints the cab screen picture with Direct2D + DirectWrite into a WIC bitmap (CPU only, so it
// never touches the game's D3D device). Everything is laid out inside the visible window of the
// texture. One thread only.
class Painter {
public:
    bool Init();
    // Cover, title, artist and progress, or a status message.
    bool PaintPlayer(const screen::Window& window, const spotify::State& state,
                     unsigned long long now, const std::wstring& screenKey);
    // The calibration frame (screen/calibration.hpp).
    bool PaintCalibration(const calibration::View& view);
    // The finished picture: rows flipped (the game samples its display textures upside down) and a
    // full mip chain (box filter).
    std::shared_ptr<Frame> Snapshot();
    // The last picture has a line moving back and forth: repaint often until it stops.
    bool Scrolling() const { return scrolling_; }

private:
    using Format = Microsoft::WRL::ComPtr<IDWriteTextFormat>;

    // A line that may move: its text and since when it is shown (it starts at the beginning).
    struct Scroller {
        std::wstring text;
        unsigned long long since = 0;
    };

    void SetScreenUnits(float left, float top, float aspect);
    ID2D1SolidColorBrush* Brush(uint32_t rgb, float alpha = 1.0f);
    Format MakeFormat(const wchar_t* font, float size, DWRITE_FONT_WEIGHT weight,
                      DWRITE_TEXT_ALIGNMENT align);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text,
                                                         const wchar_t* font, float size,
                                                         DWRITE_FONT_WEIGHT weight, float height);
    float FitSize(const std::wstring& text, const wchar_t* font, float maxSize, float minSize,
                  DWRITE_FONT_WEIGHT weight, float width, float height, float* textWidth);
    float FitOrScroll(Scroller& scroller, const std::wstring& text, const wchar_t* font,
                      float maxSize, float minSize, DWRITE_FONT_WEIGHT weight, D2D1_RECT_F rect,
                      ID2D1Brush* brush, float speed, unsigned long long now);
    float FitText(const std::wstring& text, const wchar_t* font, float maxSize, float minSize,
                  DWRITE_FONT_WEIGHT weight, D2D1_RECT_F rect, ID2D1Brush* brush,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> CoverBitmap(const spotify::State& state);
    void DrawCover(ID2D1Bitmap* art, const D2D1_RECT_F& rect, float radius);
    void DrawPlaceholderCover(const D2D1_RECT_F& rect, float radius);
    bool Fail(const char* what, HRESULT hr);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt_;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushes_;
    std::shared_ptr<const std::string> coverKey_;  // the cover bytes coverBitmap_ was decoded from
    Microsoft::WRL::ComPtr<ID2D1Bitmap> coverBitmap_;
    Scroller titleScroll_;
    Scroller lineScroll_;
    bool scrolling_ = false;
};

}  // namespace ui
