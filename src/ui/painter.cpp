#include "ui/painter.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "core/log.hpp"
#include "core/text.hpp"
#include "ui/marquee.hpp"
#include "ui/texts.hpp"

using Microsoft::WRL::ComPtr;

namespace ui {
namespace {
// Colours (RGB).
constexpr uint32_t kBackground = 0x121212;
constexpr uint32_t kGreen = 0x1DB954;  // Spotify green
constexpr uint32_t kDarkGreen = 0x0B3D20;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kGrey = 0xB3B3B3;
constexpr uint32_t kTrack = 0x3A3A3A;  // progress bar background
// Calibration.
constexpr uint32_t kOutside = 0xFF2BD6;  // pink: must not be visible when the frame is right
constexpr uint32_t kInside = 0x0E1622;
constexpr uint32_t kGrid = 0x2C3E55;
constexpr uint32_t kEdge = 0xFFD400;
constexpr uint32_t kSelected = 0x1DFF6A;
constexpr uint32_t kCircle = 0x6F8FB5;  // proportion guides

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                        (rgb & 0xFF) / 255.0f, alpha);
}

D2D1_RECT_F WindowRect(const screen::Window& w) {
    return D2D1::RectF(w.u0 * kWidth, w.v0 * kHeight, w.u1 * kWidth, w.v1 * kHeight);
}
}  // namespace

bool Painter::Init() {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic_));
    if (FAILED(hr)) { return Fail("WIC factory", hr); }
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    if (FAILED(hr)) { return Fail("D2D factory", hr); }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(hr)) { return Fail("DWrite factory", hr); }

    hr = wic_->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                            &bitmap_);
    if (FAILED(hr)) { return Fail("WIC bitmap", hr); }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = d2d_->CreateWicBitmapRenderTarget(bitmap_.Get(), props, &rt_);
    if (FAILED(hr)) { return Fail("D2D render target", hr); }
    return true;
}

// Draws in screen units (screen/screen_window.hpp): from here on a square looks square on the cab
// screen.
void Painter::SetScreenUnits(float left, float top, float aspect) {
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / aspect, 1.0f) *
                      D2D1::Matrix3x2F::Translation(left, top));
}

// The screen is tiny in the cab (about 140x60 px at 1080p): big cover, big title, artist, thick
// bar.
bool Painter::PaintPlayer(const screen::Window& window, const spotify::State& s,
                          unsigned long long now, const std::wstring& screenKey) {
    rt_->BeginDraw();
    rt_->Clear(Rgb(kBackground));
    scrolling_ = false;

    const screen::LayoutBox box = screen::Layout(window, kWidth, kHeight);
    SetScreenUnits(box.left, box.top, box.aspect);
    const float w = box.width;
    const float h = box.height;
    const float m = h * 0.08f;  // margin

    // Cover (or a placeholder), dimmed with pause bars while paused.
    const bool track = s.status == spotify::Status::Track;
    const float side = h - 2 * m;
    const D2D1_RECT_F cover = D2D1::RectF(m, m, m + side, m + side);
    const float radius = side * 0.05f;
    if (ComPtr<ID2D1Bitmap> art = track ? CoverBitmap(s) : nullptr) {
        DrawCover(art.Get(), cover, radius);
    } else {
        DrawPlaceholderCover(cover, radius);
    }
    if (track && !s.isPlaying) {
        rt_->FillRoundedRectangle(D2D1::RoundedRect(cover, radius, radius), Brush(0x000000, 0.5f));
        const float cx = (cover.left + cover.right) / 2;
        const float cy = (cover.top + cover.bottom) / 2;
        const float bar = side * 0.09f;
        rt_->FillRectangle(
            D2D1::RectF(cx - bar * 1.6f, cy - bar * 2.2f, cx - bar * 0.4f, cy + bar * 2.2f),
            Brush(kWhite));
        rt_->FillRectangle(
            D2D1::RectF(cx + bar * 0.4f, cy - bar * 2.2f, cx + bar * 1.6f, cy + bar * 2.2f),
            Brush(kWhite));
    }

    // Title and artist, or a status message; a notice replaces the second line for a few seconds.
    std::wstring title;
    std::wstring line;
    if (track) {
        title = text::Widen(s.title);
        line = text::Widen(s.artist);
    } else {
        texts::Status(s, screenKey, &title, &line);
    }
    const bool notice = now < s.noticeUntil && s.notice != spotify::Notice::None;
    if (notice) { line = texts::Notice(s); }

    // Lines that do not fit move back and forth, at about a third of the box height per second.
    const float tx = cover.right + m * 1.2f;
    const float tr = w - m;
    const float speed = h * 0.3f;
    const float titleSize =
        FitOrScroll(titleScroll_, title, L"Segoe UI", h * 0.25f, h * 0.14f, DWRITE_FONT_WEIGHT_BOLD,
                    D2D1::RectF(tx, m, tr, m + h * 0.34f), Brush(kWhite), speed, now);
    const float lineMax = std::min(h * 0.18f, titleSize * 0.8f);  // never louder than the title
    FitOrScroll(lineScroll_, line, L"Segoe UI", lineMax, std::min(h * 0.11f, lineMax),
                DWRITE_FONT_WEIGHT_SEMI_BOLD, D2D1::RectF(tx, m + h * 0.34f, tr, m + h * 0.58f),
                Brush(notice ? kGreen : kGrey), speed, now);

    // Progress bar.
    if (track && s.durationMs > 0) {
        const float barH = h * 0.06f;
        const float barY = h - m - barH * 1.5f;
        const float fraction =
            std::clamp(spotify::ProgressNow(s, now) / static_cast<float>(s.durationMs), 0.0f, 1.0f);
        const float played = tx + (tr - tx) * fraction;
        const float r = barH / 2;

        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tx, barY, tr, barY + barH), r, r),
                                  Brush(kTrack));
        if (played > tx + r) {
            rt_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(tx, barY, played, barY + barH), r, r), Brush(kGreen));
        }
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(played, barY + r), barH, barH), Brush(kWhite));
    }

    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = rt_->EndDraw();
    return SUCCEEDED(hr) || Fail("EndDraw", hr);
}

// Pink outside the frame, a faint grid inside, the four edges as lines (the one being adjusted
// green and thick), two circles and what is being adjusted in big letters. The frame is right when
// no pink is visible and every edge line still is; the proportions are right when the circles look
// round.
bool Painter::PaintCalibration(const calibration::View& v) {
    using calibration::Item;
    rt_->BeginDraw();
    rt_->Clear(Rgb(kOutside));
    scrolling_ = false;

    const screen::Window& win = v.window;
    const D2D1_RECT_F r = WindowRect(win);
    const float h = r.bottom - r.top;
    rt_->FillRectangle(r, Brush(kInside));
    for (int k = 1; k < 20; ++k) {  // grid every 0.05 of the texture: shows scale and stretch
        const float x = k * 0.05f * kWidth;
        const float y = k * 0.05f * kHeight;
        if (x > r.left && x < r.right) {
            rt_->DrawLine({x, r.top}, {x, r.bottom}, Brush(kGrid), 1.5f);
        }
        if (y > r.top && y < r.bottom) {
            rt_->DrawLine({r.left, y}, {r.right, y}, Brush(kGrid), 1.5f);
        }
    }

    // texture pixels
    const float thin = 5.0f;
    const float thick = 12.0f;
    auto edge = [&](Item item, float left, float top, float right, float bottom) {
        rt_->FillRectangle(D2D1::RectF(left, top, right, bottom),
                           Brush(item == v.item ? kSelected : kEdge));
    };
    auto t = [&](Item item) { return item == v.item ? thick : thin; };
    edge(Item::Left, r.left, r.top, r.left + t(Item::Left), r.bottom);
    edge(Item::Right, r.right - t(Item::Right), r.top, r.right, r.bottom);
    edge(Item::Top, r.left, r.top, r.right, r.top + t(Item::Top));
    edge(Item::Bottom, r.left, r.bottom - t(Item::Bottom), r.right, r.bottom);

    std::wstring value;
    switch (v.item) {
        case Item::Left: value = std::format(L"{:.3f}", win.u0); break;
        case Item::Right: value = std::format(L"{:.3f}", win.u1); break;
        case Item::Top: value = std::format(L"{:.3f}", win.v0); break;
        case Item::Bottom: value = std::format(L"{:.3f}", win.v1); break;
        case Item::Proportions: value = std::format(L"{:.2f}", win.aspect); break;
        default:
            value = v.displayWidth ? std::format(L"{}×{}", v.displayWidth, v.displayHeight)
                                   : texts::DisplayNotFound();
            break;
    }

    // Circles and words in screen units, so both look right once the proportions are.
    SetScreenUnits(r.left, r.top, win.aspect);
    const float w = (r.right - r.left) * win.aspect;
    const bool proportions = v.item == Item::Proportions;
    const float radius = h * 0.3f;
    for (float cx : {radius + h * 0.12f, w - radius - h * 0.12f}) {
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, h / 2), radius, radius),
                         Brush(proportions ? kSelected : kCircle), proportions ? 6.0f : 3.0f);
    }

    const float m = thick + h * 0.06f;
    FitText(texts::CalibrationLabel(v.item), L"Segoe UI", h * 0.24f, h * 0.12f,
            DWRITE_FONT_WEIGHT_BOLD, D2D1::RectF(m, m, w - m, m + h * 0.3f), Brush(kWhite),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    FitText(value, L"Segoe UI", h * 0.24f, h * 0.12f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            D2D1::RectF(m, m + h * 0.3f, w - m, m + h * 0.6f), Brush(kSelected),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    FitText(text::Widen(v.truck), L"Segoe UI", h * 0.14f, h * 0.09f, DWRITE_FONT_WEIGHT_NORMAL,
            D2D1::RectF(m, h - m - h * 0.2f, w - m, h - m), Brush(kGrey),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());

    const HRESULT hr = rt_->EndDraw();
    return SUCCEEDED(hr) || Fail("EndDraw", hr);
}

std::shared_ptr<Frame> Painter::Snapshot() {
    auto frame = std::make_shared<Frame>();
    frame->width = kWidth;
    frame->height = kHeight;

    WICRect rect{0, 0, static_cast<INT>(kWidth), static_cast<INT>(kHeight)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap_->Lock(&rect, WICBitmapLockRead, &lock))) { return nullptr; }
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &data);

    std::vector<uint32_t> level(static_cast<size_t>(kWidth) * kHeight);
    for (unsigned y = 0; y < kHeight; ++y) {
        const uint32_t* src =
            reinterpret_cast<const uint32_t*>(data + static_cast<size_t>(kHeight - 1 - y) * stride);
        std::copy(src, src + kWidth, level.begin() + static_cast<size_t>(y) * kWidth);
    }
    frame->mips.push_back(std::move(level));

    unsigned w = kWidth;
    unsigned h = kHeight;
    while (w > 1 || h > 1) {
        const unsigned nw = std::max(1u, w / 2);
        const unsigned nh = std::max(1u, h / 2);
        const std::vector<uint32_t>& prev = frame->mips.back();
        std::vector<uint32_t> next(static_cast<size_t>(nw) * nh);
        for (unsigned y = 0; y < nh; ++y) {
            const unsigned y0 = std::min(2 * y, h - 1);
            const unsigned y1 = std::min(2 * y + 1, h - 1);
            for (unsigned x = 0; x < nw; ++x) {
                const unsigned x0 = std::min(2 * x, w - 1);
                const unsigned x1 = std::min(2 * x + 1, w - 1);
                const std::array<uint32_t, 4> p{prev[y0 * w + x0], prev[y0 * w + x1],
                                                prev[y1 * w + x0], prev[y1 * w + x1]};
                uint32_t out = 0;
                for (int c = 0; c < 4; ++c) {  // each byte channel: average of the 2x2 block
                    unsigned sum = 0;
                    for (uint32_t v : p) { sum += (v >> (8 * c)) & 0xFF; }
                    out |= ((sum + 2) / 4) << (8 * c);
                }
                next[static_cast<size_t>(y) * nw + x] = out;
            }
        }
        frame->mips.push_back(std::move(next));
        w = nw;
        h = nh;
    }
    return frame;
}

ID2D1SolidColorBrush* Painter::Brush(uint32_t rgb, float alpha) {
    const uint64_t key = (static_cast<uint64_t>(rgb) << 8) | static_cast<uint64_t>(alpha * 255.0f);
    ComPtr<ID2D1SolidColorBrush>& brush = brushes_[key];
    if (!brush) { rt_->CreateSolidColorBrush(Rgb(rgb, alpha), &brush); }
    return brush.Get();
}

Painter::Format Painter::MakeFormat(const wchar_t* font, float size, DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align) {
    Format format;
    if (FAILED(dwrite_->CreateTextFormat(font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) {
        return nullptr;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return format;
}

ComPtr<IDWriteTextLayout> Painter::MakeLayout(const std::wstring& text, const wchar_t* font,
                                              float size, DWRITE_FONT_WEIGHT weight, float height) {
    Format format = MakeFormat(font, size, weight, DWRITE_TEXT_ALIGNMENT_LEADING);
    ComPtr<IDWriteTextLayout> layout;
    if (format) {
        dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                                  100000.0f, height, &layout);
    }
    return layout;
}

// The font size at which `text` fits `width`, from maxSize down to minSize, and its width at that
// size.
float Painter::FitSize(const std::wstring& text, const wchar_t* font, float maxSize, float minSize,
                       DWRITE_FONT_WEIGHT weight, float width, float height, float* textWidth) {
    float size = maxSize;
    *textWidth = 0.0f;
    for (int pass = 0; pass < 3; ++pass) {
        ComPtr<IDWriteTextLayout> layout = MakeLayout(text, font, size, weight, height);
        if (!layout) { break; }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        *textWidth = metrics.widthIncludingTrailingWhitespace;
        if (*textWidth <= width || size <= minSize) { break; }
        size = std::max(minSize, size * width / *textWidth * 0.98f);
    }
    return size;
}

// One line of text in `rect`: starts at maxSize, shrinks to fit the width down to minSize, then
// cuts with an ellipsis. Returns the font size used.
float Painter::FitText(const std::wstring& text, const wchar_t* font, float maxSize, float minSize,
                       DWRITE_FONT_WEIGHT weight, D2D1_RECT_F rect, ID2D1Brush* brush,
                       DWRITE_TEXT_ALIGNMENT align) {
    if (text.empty()) { return maxSize; }
    float textWidth = 0.0f;
    const float size = FitSize(text, font, maxSize, minSize, weight, rect.right - rect.left,
                               rect.bottom - rect.top, &textWidth);

    Format format = MakeFormat(font, size, weight, align);
    if (!format) { return size; }
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
        format->SetTrimming(&trimming, ellipsis.Get());
    }

    rt_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect, brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return size;
}

// Like FitText, but a line that does not fit even at minSize moves back and forth (ui/marquee.hpp)
// instead of being cut. `speed` is in units per second. Returns the font size used.
float Painter::FitOrScroll(Scroller& scroller, const std::wstring& text, const wchar_t* font,
                           float maxSize, float minSize, DWRITE_FONT_WEIGHT weight,
                           D2D1_RECT_F rect, ID2D1Brush* brush, float speed,
                           unsigned long long now) {
    if (text != scroller.text) {  // a new line starts from the beginning
        scroller.text = text;
        scroller.since = now;
    }

    if (text.empty()) { return maxSize; }
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    float textWidth = 0.0f;
    const float size = FitSize(text, font, maxSize, minSize, weight, width, height, &textWidth);
    ComPtr<IDWriteTextLayout> layout = MakeLayout(text, font, size, weight, height);
    if (!layout) { return size; }

    float offset = 0.0f;
    if (textWidth > width) {
        offset = MarqueeOffset(textWidth - width, now - scroller.since, Marquee{speed});
        scrolling_ = true;
    }

    rt_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt_->DrawTextLayout(D2D1::Point2F(rect.left - offset, rect.top), layout.Get(), brush);
    rt_->PopAxisAlignedClip();
    return size;
}

// Decodes the cover JPEG once per track (cached by the shared buffer).
ComPtr<ID2D1Bitmap> Painter::CoverBitmap(const spotify::State& s) {
    if (!s.cover || s.cover->empty()) { return nullptr; }
    if (s.cover.get() == coverKey_.get()) { return coverBitmap_; }
    coverKey_ = s.cover;
    coverBitmap_.Reset();

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = wic_->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(s.cover->data())),
            static_cast<DWORD>(s.cover->size()));
    }
    if (SUCCEEDED(hr)) {
        hr = wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                           &decoder);
    }
    if (SUCCEEDED(hr)) { hr = decoder->GetFrame(0, &frame); }
    if (SUCCEEDED(hr)) { hr = wic_->CreateFormatConverter(&converter); }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut);
    }
    if (SUCCEEDED(hr)) {
        hr = rt_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &coverBitmap_);
    }

    if (FAILED(hr)) {
        logx::Log("[ui] cover decode failed: 0x{:08X}", static_cast<unsigned long>(hr));
        coverBitmap_.Reset();
    }
    return coverBitmap_;
}

void Painter::DrawCover(ID2D1Bitmap* art, const D2D1_RECT_F& rect, float radius) {
    ComPtr<ID2D1BitmapBrush> brush;
    if (FAILED(rt_->CreateBitmapBrush(art, &brush))) { return; }
    brush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    const D2D1_SIZE_F size = art->GetSize();
    const float sx = (rect.right - rect.left) / size.width;
    const float sy = (rect.bottom - rect.top) / size.height;
    brush->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) *
                        D2D1::Matrix3x2F::Translation(rect.left, rect.top));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
}

void Painter::DrawPlaceholderCover(const D2D1_RECT_F& rect, float radius) {
    const std::array<D2D1_GRADIENT_STOP, 2> stops{{{0.0f, Rgb(kGreen)}, {1.0f, Rgb(kDarkGreen)}}};
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(rt_->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()),
                                                 &stopCollection)) ||
        FAILED(rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top),
                                                D2D1::Point2F(rect.right, rect.bottom)),
            stopCollection.Get(), &gradient))) {
        return;
    }

    rt_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), gradient.Get());
    const float side = rect.right - rect.left;
    FitText(L"♪", L"Segoe UI Symbol", side * 0.5f, side * 0.5f, DWRITE_FONT_WEIGHT_NORMAL, rect,
            Brush(kWhite), DWRITE_TEXT_ALIGNMENT_CENTER);
}

bool Painter::Fail(const char* what, HRESULT hr) {
    logx::Log("[ui] {} failed: 0x{:08X}", what, static_cast<unsigned long>(hr));
    return false;
}

}  // namespace ui
