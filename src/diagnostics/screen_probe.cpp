#include "diagnostics/screen_probe.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/immortal.hpp"
#include "core/log.hpp"
#include "diagnostics/pattern.hpp"

using Microsoft::WRL::ComPtr;

namespace probe {
namespace {
constexpr int kNotCandidate = -1;
constexpr int kUnknown = -2;  // backbuffer size not known yet, retry later

struct Candidate {
    void* resource;  // identity only, no reference held
    UINT width;
    UINT height;
    UINT mips;
    UINT bind;
    UINT misc;
    DXGI_FORMAT format;
    bool screenSized;
    ComPtr<ID3D11ShaderResourceView> pattern;
};

// Guards the two containers below. Shared on the per-bind fast path.
SRWLOCK g_lock = SRWLOCK_INIT;
// Immortal (core/immortal.hpp); the patterns are released by Reset() instead, on the game thread.
auto& g_srvToCandidate = core::Immortal<std::unordered_map<ID3D11ShaderResourceView*, int>>();
std::vector<Candidate>& g_candidates = core::Immortal<std::vector<Candidate>>();
// Internal render resolution = largest single-mip candidate. With render scaling it differs from
// the backbuffer (first in-game run: scene 2880x2160, swapchain 1920x1080). Guarded by g_lock.
UINT g_sceneWidth = 0;
UINT g_sceneHeight = 0;

std::atomic<int> g_mode{kModeOff};
std::atomic<int> g_selected{0};
std::atomic<UINT> g_bbWidth{0};
std::atomic<UINT> g_bbHeight{0};
std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_substitutions{0};
std::atomic<unsigned long long> g_statsStart{0};

const char* ModeName(int mode) {
    switch (mode) {
        case kModeAll: return "ALL";
        case kModeSingle: return "SINGLE";
        default: return "OFF";
    }
}

bool IsColorFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: return true;
        default: return false;
    }
}

bool Near(UINT a, UINT b) { return a + 1 >= b && b + 1 >= a; }

// Full-screen buffers and their 1/2..1/32 mip chain (bloom, SSAO...): replacing those in ALL
// mode would wreck the picture, so ALL mode skips them. SINGLE mode still visits them.
bool IsScreenSized(UINT w, UINT h, UINT bw, UINT bh) {
    for (UINT k = 0; k <= 5; ++k) {
        const UINT floorW = bw >> k;
        const UINT floorH = bh >> k;
        const UINT ceilW = (bw + (1u << k) - 1) >> k;
        const UINT ceilH = (bh + (1u << k) - 1) >> k;
        if ((Near(w, floorW) || Near(w, ceilW)) && (Near(h, floorH) || Near(h, ceilH))) {
            return true;
        }
    }
    return false;
}

std::string Describe(int idx, const Candidate& c) {
    return std::format("#{} {}x{} fmt={} mips={} bind=0x{:X} misc=0x{:X}{}", idx, c.width, c.height,
                       static_cast<int>(c.format), c.mips, c.bind, c.misc,
                       c.screenSized ? " screen-sized" : "");
}

// Exclusive lock held.
int ClassifyLocked(ID3D11ShaderResourceView* view) {
    const auto it = g_srvToCandidate.find(view);
    if (it != g_srvToCandidate.end()) { return it->second; }

    const UINT bw = g_bbWidth.load();
    const UINT bh = g_bbHeight.load();
    if (bw == 0 || bh == 0) { return kUnknown; }

    int result = kNotCandidate;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    view->GetDesc(&sd);
    if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && IsColorFormat(sd.Format)) {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource && SUCCEEDED(resource.As(&texture))) {
            D3D11_TEXTURE2D_DESC td{};
            texture->GetDesc(&td);
            const bool shape = (td.BindFlags & D3D11_BIND_RENDER_TARGET) &&
                               td.SampleDesc.Count == 1 && td.ArraySize == 1 &&
                               !(td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) &&
                               td.Width >= 64 && td.Height >= 64;
            if (shape) {
                for (size_t i = 0; i < g_candidates.size(); ++i) {
                    if (g_candidates[i].resource == resource.Get()) {
                        result = static_cast<int>(i);
                        break;
                    }
                }
                if (result == kNotCandidate) {
                    g_candidates.push_back({resource.Get(), td.Width, td.Height, td.MipLevels,
                                            td.BindFlags, td.MiscFlags, sd.Format,
                                            IsScreenSized(td.Width, td.Height, bw, bh), nullptr});
                    result = static_cast<int>(g_candidates.size()) - 1;
                    logx::Log("[probe] new candidate {}", Describe(result, g_candidates.back()));
                    if (td.MipLevels == 1 && td.Width * td.Height > g_sceneWidth * g_sceneHeight &&
                        td.Width * td.Height > bw * bh) {
                        g_sceneWidth = td.Width;
                        g_sceneHeight = td.Height;
                        logx::Log("[probe] internal render resolution looks like {}x{}", td.Width,
                                  td.Height);
                    }
                }
            }
        }
    }

    g_srvToCandidate.emplace(view, result);
    return result;
}

// Lock (shared or exclusive) held.
bool ShouldReplace(int idx, int mode) {
    if (mode == kModeAll) {
        const Candidate& c = g_candidates[idx];
        return !c.screenSized && !IsScreenSized(c.width, c.height, g_sceneWidth, g_sceneHeight);
    }
    if (mode == kModeSingle) { return idx == g_selected.load(std::memory_order_relaxed); }
    return false;
}

size_t CandidateCountLocked() {
    AcquireSRWLockShared(&g_lock);
    const size_t n = g_candidates.size();
    ReleaseSRWLockShared(&g_lock);
    return n;
}

std::string DescribeSelected() {
    std::string text;
    AcquireSRWLockShared(&g_lock);
    const int sel = g_selected.load();
    if (sel >= 0 && sel < static_cast<int>(g_candidates.size())) {
        text = Describe(sel, g_candidates[sel]);
    }
    ReleaseSRWLockShared(&g_lock);
    return text.empty() ? std::string("(none)") : text;
}

void ResetStats() {
    g_calls.store(0);
    g_substitutions.store(0);
    g_statsStart.store(GetTickCount64());
}
}  // namespace

bool Active() { return g_mode.load(std::memory_order_relaxed) != kModeOff; }

void CountCall() { g_calls.fetch_add(1, std::memory_order_relaxed); }

ID3D11ShaderResourceView* Substitute(ID3D11ShaderResourceView* view) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == kModeOff) { return nullptr; }

    // Fast path: view already classified and pattern already created.
    AcquireSRWLockShared(&g_lock);
    const auto it = g_srvToCandidate.find(view);
    const bool known = it != g_srvToCandidate.end();
    ID3D11ShaderResourceView* substitute = nullptr;
    bool needSlowPath = !known;
    if (known && it->second >= 0 && ShouldReplace(it->second, mode)) {
        substitute = g_candidates[it->second].pattern.Get();
        needSlowPath = substitute == nullptr;
        if (substitute) { substitute->AddRef(); }
    }
    ReleaseSRWLockShared(&g_lock);

    if (needSlowPath) {
        AcquireSRWLockExclusive(&g_lock);
        const int idx = ClassifyLocked(view);
        if (idx >= 0 && ShouldReplace(idx, mode)) {
            Candidate& c = g_candidates[idx];
            if (!c.pattern) {
                ComPtr<ID3D11Device> device;
                view->GetDevice(&device);
                c.pattern = pattern::CreateSrv(device.Get(), idx);
                if (!c.pattern) { logx::Log("[probe] failed to create test pattern for #{}", idx); }
            }
            substitute = c.pattern.Get();
            if (substitute) { substitute->AddRef(); }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }

    if (substitute) { g_substitutions.fetch_add(1, std::memory_order_relaxed); }
    return substitute;
}

void SetBackbuffer(UINT width, UINT height) {
    g_bbWidth.store(width, std::memory_order_relaxed);
    g_bbHeight.store(height, std::memory_order_relaxed);
}

void OnPresentTick() {
    if (!Active()) { return; }
    const unsigned long long now = GetTickCount64();
    const unsigned long long start = g_statsStart.load();
    if (now - start < 5000) { return; }

    const unsigned long long calls = g_calls.exchange(0);
    const unsigned long long subs = g_substitutions.exchange(0);
    g_statsStart.store(now);
    logx::Log(
        "[probe] mode={} selected=#{} candidates={} | last {:.1f}s: PSSetShaderResources calls={}, "
        "replaced={}",
        ModeName(g_mode.load()), g_selected.load(), CandidateCountLocked(), (now - start) / 1000.0,
        calls, subs);
}

void CycleMode() {
    const int mode = (g_mode.load() + 1) % 3;
    g_mode.store(mode);
    ResetStats();
    logx::Log("[probe] mode -> {} (candidates so far: {})", ModeName(mode), CandidateCountLocked());
    if (mode == kModeSingle) { logx::Log("[probe] selected {}", DescribeSelected()); }
    MessageBeep(mode == kModeOff ? MB_ICONASTERISK : MB_OK);
}

void Step(int direction) {
    const int n = static_cast<int>(CandidateCountLocked());
    if (n == 0) {
        logx::Log("[probe] no candidates yet: press F8 while sitting in the cab first");
        MessageBeep(MB_ICONHAND);
        return;
    }

    if (g_mode.load() != kModeSingle) {
        g_mode.store(kModeSingle);
        ResetStats();
    }
    const int sel = ((g_selected.load() + direction) % n + n) % n;
    g_selected.store(sel);
    logx::Log("[probe] selected {}", DescribeSelected());
    MessageBeep(MB_OK);
}

void Confirm() {
    if (g_mode.load() != kModeSingle) {
        logx::Log("[probe] confirm ignored: select a single candidate with F9 first");
        MessageBeep(MB_ICONHAND);
        return;
    }

    const std::string desc = DescribeSelected();
    logx::Log("[probe] CONFIRMED in-cab screen texture: {} (backbuffer {}x{})", desc,
              g_bbWidth.load(), g_bbHeight.load());

    std::ofstream file(logx::Dir() / L"ets_spotify_screen.txt", std::ios::app);
    file << std::format("{} backbuffer={}x{}\n", desc, g_bbWidth.load(), g_bbHeight.load());
    MessageBeep(MB_ICONEXCLAMATION);
}

void Reset(const char* reason) {
    AcquireSRWLockExclusive(&g_lock);
    const size_t dropped = g_candidates.size();
    g_srvToCandidate.clear();
    g_candidates.clear();  // releases patterns; bindings in flight hold their own reference
    g_sceneWidth = 0;
    g_sceneHeight = 0;
    ReleaseSRWLockExclusive(&g_lock);

    g_selected.store(0);
    if (dropped) { logx::Log("[probe] reset ({}): dropped {} candidates", reason, dropped); }
}

int Lookup(ID3D11ShaderResourceView* view) {
    AcquireSRWLockShared(&g_lock);
    const auto it = g_srvToCandidate.find(view);
    const bool known = it != g_srvToCandidate.end();
    int idx = known ? it->second : kNotCandidate;
    ReleaseSRWLockShared(&g_lock);

    if (!known) {
        AcquireSRWLockExclusive(&g_lock);
        idx = ClassifyLocked(view);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return idx >= 0 ? idx : -1;
}

int DescribeResource(ID3D11Resource* resource, std::string* text) {
    if (!resource) {
        *text = "null";
        return -1;
    }

    AcquireSRWLockShared(&g_lock);
    const auto it = std::find_if(g_candidates.begin(), g_candidates.end(),
                                 [resource](const Candidate& c) { return c.resource == resource; });
    const int idx = it == g_candidates.end() ? -1 : static_cast<int>(it - g_candidates.begin());
    if (idx >= 0) { *text = std::format("#{} {}x{}", idx, it->width, it->height); }
    ReleaseSRWLockShared(&g_lock);
    if (idx >= 0) { return idx; }

    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
        D3D11_TEXTURE2D_DESC td{};
        texture->GetDesc(&td);
        *text = std::format("{}x{} fmt={}{}", td.Width, td.Height, static_cast<int>(td.Format),
                            td.ArraySize > 1 ? " array" : "");
    } else {
        *text = "non-2D resource";
    }
    return -1;
}

bool IsInteresting(int idx) {
    AcquireSRWLockShared(&g_lock);
    bool result = false;
    if (idx >= 0 && idx < static_cast<int>(g_candidates.size())) {
        const Candidate& c = g_candidates[idx];
        result = !c.screenSized && !IsScreenSized(c.width, c.height, g_sceneWidth, g_sceneHeight);
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

void DebugSet(int mode, int selected) {
    g_mode.store(mode);
    g_selected.store(selected);
    ResetStats();
}

int CandidateCount() { return static_cast<int>(CandidateCountLocked()); }
}  // namespace probe
