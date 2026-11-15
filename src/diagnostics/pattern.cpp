#include "diagnostics/pattern.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pattern {
namespace {
constexpr int kWidth = 512;
constexpr int kHeight = 256;

// 5x7 glyphs; each row is 5 bits, most significant bit = leftmost pixel.
struct Glyph {
    char ch;
    std::array<uint8_t, 7> rows;
};
constexpr std::array<Glyph, 11> kGlyphs{{
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'#', {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010}},
}};

const Glyph* FindGlyph(char c) {
    const auto it =
        std::find_if(kGlyphs.begin(), kGlyphs.end(), [c](const Glyph& g) { return g.ch == c; });
    return it == kGlyphs.end() ? nullptr : &*it;
}

// R8G8B8A8_UNORM, little-endian: byte order R, G, B, A.
constexpr uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b) {
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

void FillRect(std::vector<uint32_t>& px, int x0, int y0, int x1, int y1, uint32_t color) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, kWidth);
    y1 = std::min(y1, kHeight);

    for (int y = y0; y < y1; ++y) {
        std::fill(px.begin() + static_cast<ptrdiff_t>(y) * kWidth + x0,
                  px.begin() + static_cast<ptrdiff_t>(y) * kWidth + x1, color);
    }
}

std::vector<uint32_t> Render(int number) {
    std::vector<uint32_t> px(static_cast<size_t>(kWidth) * kHeight);
    const uint32_t stripeA = Rgba(255, 0, 200);
    const uint32_t stripeB = Rgba(60, 0, 90);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            px[static_cast<size_t>(y) * kWidth + x] = (((x + y) / 24) % 2 != 0) ? stripeA : stripeB;
        }
    }

    const uint32_t border = Rgba(255, 220, 0);
    FillRect(px, 0, 0, kWidth, 10, border);
    FillRect(px, 0, kHeight - 10, kWidth, kHeight, border);
    FillRect(px, 0, 0, 10, kHeight, border);
    FillRect(px, kWidth - 10, 0, kWidth, kHeight, border);

    const std::string text = "#" + std::to_string(number);
    const int length = static_cast<int>(text.size());
    const int unitsW = length * 6 - 1;
    const int unitsH = 7;
    const int scale = std::max(1, std::min(kWidth * 8 / 10 / unitsW, kHeight * 6 / 10 / unitsH));
    const int textW = unitsW * scale;
    const int textH = unitsH * scale;
    const int x0 = (kWidth - textW) / 2;
    const int y0 = (kHeight - textH) / 2;

    FillRect(px, x0 - scale, y0 - scale, x0 + textW + scale, y0 + textH + scale, Rgba(0, 0, 0));
    for (int i = 0; i < length; ++i) {
        const Glyph* glyph = FindGlyph(text[static_cast<size_t>(i)]);
        if (!glyph) { continue; }
        for (int r = 0; r < unitsH; ++r) {
            for (int c = 0; c < 5; ++c) {
                if ((glyph->rows[static_cast<size_t>(r)] >> (4 - c)) & 1) {
                    const int px0 = x0 + (i * 6 + c) * scale;
                    const int py0 = y0 + r * scale;
                    FillRect(px, px0, py0, px0 + scale, py0 + scale, Rgba(255, 255, 255));
                }
            }
        }
    }
    return px;
}
}  // namespace

ComPtr<ID3D11ShaderResourceView> CreateSrv(ID3D11Device* device, int number) {
    if (!device) { return nullptr; }
    const std::vector<uint32_t> px = Render(number);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = px.data();
    init.SysMemPitch = kWidth * 4;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&td, &init, &texture))) { return nullptr; }
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) { return nullptr; }
    return srv;
}
}  // namespace pattern
