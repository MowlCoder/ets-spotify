#include "screen/nav_screen.hpp"

#include <array>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/immortal.hpp"
#include "core/log.hpp"
#include "game/game_state.hpp"
#include "screen/display_graph.hpp"
#include "ui/ui_renderer.hpp"

using Microsoft::WRL::ComPtr;

namespace nav {
namespace {
constexpr int kLearnFrames = 3;            // scene-pass view learning window...
constexpr ULONGLONG kLearnEveryMs = 2000;  // ...repeated this often
constexpr int kMaxTargetViews = 4;
constexpr size_t kMaxKnownTextures = 64;
// A new truck's displays are logged once the cab works: while driving with the ignition on, at the
// first map screen, or after this long without one.
constexpr ULONGLONG kTruckLogPatienceMs = 8000;

// ---- Shared state ----

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_learning{false};
std::atomic<bool> g_forceLearn{false};
std::atomic<bool> g_displayPass{false};  // the current render target is a display texture
std::atomic<bool> g_vehicleChanged{false};
std::atomic<uint32_t> g_preferredSize{0};  // Pack()ed, from the truck's settings
std::atomic<bool> g_preferredPinned{false};
std::atomic<int> g_pick{0};               // pending manual switch (calibration)
std::atomic<uint32_t> g_targetSize{0};    // Pack()ed size of the navigation texture
std::atomic<bool> g_targetPinned{false};  // that display was picked by hand

uint32_t Pack(Size s) { return (s.width << 16) | (s.height & 0xFFFF); }
Size Unpack(uint32_t v) { return {v >> 16, v & 0xFFFF}; }

// Read on the bind fast path. Raw pointers: identity only, lifetime held by the render thread.
std::atomic<ID3D11ShaderResourceView*> g_uiViewRaw{nullptr};
std::atomic<TextureId> g_target{nullptr};
std::array<std::atomic<ID3D11ShaderResourceView*>, kMaxTargetViews> g_targetViews{};

// Guarded by g_mutex: the hooks of one frame fill the graph, Present reads and clears it.
std::mutex& g_mutex = core::Immortal<std::mutex>();
FrameGraph& g_frame = core::Immortal<FrameGraph>();
TextureId g_currentRt = nullptr;
bool g_currentIsDisplay = false;
// Views the scene pass binds for each display texture (learned).
auto& g_views =
    core::Immortal<std::unordered_map<TextureId, std::vector<ID3D11ShaderResourceView*>>>();

// ---- Render-thread state ----

int g_learnFramesLeft = 0;
ULONGLONG g_lastLearn = 0;
bool g_describePending = false;  // the new truck's displays are not logged yet
ULONGLONG g_describeDeadline = 0;
Preference
    g_preference;  // size of the last navigation texture (preferred after a reassignment), pinned?
std::vector<TextureId>& g_loggedCandidates = core::Immortal<std::vector<TextureId>>();
uint64_t g_uploadedVersion = 0;
ComPtr<ID3D11Texture2D>& g_uiTexture = core::Immortal<ComPtr<ID3D11Texture2D>>();
ComPtr<ID3D11ShaderResourceView>& g_uiView = core::Immortal<ComPtr<ID3D11ShaderResourceView>>();

// ---- Texture classification ----

bool DescribeTexture(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC* desc) {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (!resource) { return false; }
    resource->GetType(&dimension);  // cheap filter for buffers and other textures
    ComPtr<ID3D11Texture2D> texture;
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
        FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
        return false;
    }
    texture->GetDesc(desc);
    return true;
}

bool IsDisplay(const D3D11_TEXTURE2D_DESC& d) {
    return (d.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) &&
           (d.BindFlags & D3D11_BIND_RENDER_TARGET) && d.ArraySize == 1 && d.Width >= 64 &&
           d.Height >= 64 && std::max(d.Width, d.Height) <= 1024;
}

// A plain render target (no mips): the map render, but also e.g. a font cache. Which one is the map
// is decided at the end of the frame: only targets redrawn in the same frame count.
bool IsPlainRenderTarget(const D3D11_TEXTURE2D_DESC& d) {
    return !(d.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) &&
           (d.BindFlags & D3D11_BIND_RENDER_TARGET) && d.ArraySize == 1 &&
           d.SampleDesc.Count == 1 && std::max(d.Width, d.Height) >= 256 &&
           std::max(d.Width, d.Height) <= 2048;
}

// ---- Decision ----

// Points the bind fast path at the views of `target`. g_mutex held.
void PublishTargetViews(TextureId target) {
    const auto it = target ? g_views.find(target) : g_views.end();
    for (int i = 0; i < kMaxTargetViews; ++i) {
        ID3D11ShaderResourceView* view = nullptr;
        if (it != g_views.end() && i < static_cast<int>(it->second.size())) {
            view = it->second[i];
        }
        g_targetViews[i].store(view);
    }
    if (target && it == g_views.end()) {
        g_forceLearn.store(true);  // views of this texture not seen yet
    }
}

void SetTarget(TextureId target) {
    g_target.store(target);
    PublishTargetViews(target);
}

// Decides the navigation texture from this frame's display passes. Render thread, g_mutex held.
void EndFrame(ULONGLONG now) {
    FrameGraph& f = g_frame;
    if (g_vehicleChanged.exchange(false)) {  // another truck: its screens are other textures
        g_preference = {Unpack(g_preferredSize.load()), g_preferredPinned.load()};
        g_targetSize.store(0);
        g_targetPinned.store(g_preference.pinned);
        SetTarget(nullptr);
        g_loggedCandidates.clear();
        g_describePending = true;
        g_describeDeadline = now + kTruckLogPatienceMs;
    }

    // Menus and a switched-off cab say nothing about the truck's screens: wait.
    const bool cabWorking = !game::Paused() && game::IgnitionState() != game::Ignition::Off;
    if (g_describePending && !cabWorking) { g_describeDeadline = now + kTruckLogPatienceMs; }

    const int pick = g_pick.exchange(0);
    if (f.displays.empty()) {
        if (pick != 0) { logx::Log("[nav] no displays rendered, nothing to switch to"); }
        if (g_describePending && cabWorking && now >= g_describeDeadline) {
            g_describePending = false;
            logx::Log(
                "[nav] no display textures rendered in this truck (no cab screens, or not the cab "
                "camera)");
        }
        return;
    }

    const Analysis a = Analyze(f);
    const TextureId current = g_target.load();
    const TextureId chosen = Choose(f, a, current, g_preference, pick);

    if (pick != 0 && chosen) {  // picked by hand: this display stays, map or not
        g_preference.pinned = true;
        g_targetPinned.store(true);
        const auto index = std::find(a.ends.begin(), a.ends.end(), chosen) - a.ends.begin();
        logx::Log("[nav] display {} of {} picked by hand ({})", index + 1, a.ends.size(),
                  a.showsMap.count(chosen) ? "shows the map" : "no map");
    }
    if (g_describePending && cabWorking && (!a.candidates.empty() || now >= g_describeDeadline)) {
        g_describePending = false;
        logx::Log("[nav] displays in this truck: {}, map screens at the end of a chain: {}:{}",
                  f.displays.size(), a.candidates.size(), Describe(f, a));
    }
    if (a.candidates.size() >
        1) {  // logged once per distinct set: after the pause menu it stays for the session
        std::vector<TextureId> set = a.candidates;
        std::sort(set.begin(), set.end());
        if (set != g_loggedCandidates) {
            g_loggedCandidates = set;
            logx::Log("[nav] {} candidate map displays:{}", set.size(), Describe(f, a));
        }
    }

    if (chosen == current) { return; }
    if (!chosen) {
        logx::Log(
            "[nav] nav texture reassigned to another display, pausing substitution until it shows "
            "up again");
        SetTarget(nullptr);
        return;
    }

    g_preference.size = f.displays.at(chosen);
    g_targetSize.store(Pack(g_preference.size));
    logx::Log("[nav] nav display: {}x{} ({} display(s) this frame){}{}", g_preference.size.width,
              g_preference.size.height, f.displays.size(), g_preference.pinned ? ", pinned" : "",
              current ? " - switched" : "");
    SetTarget(chosen);
    ui::RequestRedraw();  // the calibration screen shows the size
}

// ---- Our texture ----

void UploadUi(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    const std::shared_ptr<const ui::Frame> frame = ui::Latest();
    if (!frame || frame->version == g_uploadedVersion || frame->mips.empty()) { return; }

    if (g_uiTexture) {
        D3D11_TEXTURE2D_DESC current{};
        g_uiTexture->GetDesc(&current);
        if (current.Width != frame->width || current.Height != frame->height ||
            current.MipLevels != frame->mips.size()) {
            g_uiViewRaw.store(nullptr);  // render thread: no bind is in flight right now
            g_uiView.Reset();
            g_uiTexture.Reset();
        }
    }
    if (!g_uiTexture) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = frame->width;
        td.Height = frame->height;
        td.MipLevels = static_cast<UINT>(frame->mips.size());
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // Direct2D output is sRGB-encoded
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&td, nullptr, &g_uiTexture);
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(g_uiTexture.Get(), nullptr, &g_uiView);
        }
        if (FAILED(hr)) {
            logx::Log("[nav] UI texture creation failed: 0x{:08X}", static_cast<unsigned long>(hr));
            g_uiTexture.Reset();
            g_uiView.Reset();
            g_uploadedVersion = frame->version;  // don't retry every frame
            return;
        }
    }

    UINT width = frame->width;
    for (UINT level = 0; level < frame->mips.size(); ++level) {
        ctx->UpdateSubresource(g_uiTexture.Get(), level, nullptr, frame->mips[level].data(),
                               width * 4, 0);
        width = std::max(1u, width / 2);
    }

    if (g_uploadedVersion == 0) {
        logx::Log("[nav] UI texture uploaded: {}x{}, {} mips", frame->width, frame->height,
                  frame->mips.size());
    }
    g_uploadedVersion = frame->version;
    g_uiViewRaw.store(g_uiView.Get(), std::memory_order_release);
}
}  // namespace

void SetEnabled(bool enabled) {
    if (enabled && !g_enabled.load()) { g_forceLearn.store(true); }
    g_enabled.store(enabled);
    ui::SetVisible(enabled);
    if (!enabled) {
        g_learning.store(false);
        g_displayPass.store(false);
    }
}

void Toggle() {
    SetEnabled(!g_enabled.load());
    if (g_enabled.load()) {
        ui::ReloadSettings();  // picks up edits to ets_spotify.ini
    }
    logx::Log("[nav] Spotify screen {}", g_enabled.load() ? "ON" : "OFF");
    MessageBeep(g_enabled.load() ? MB_OK : MB_ICONASTERISK);
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

bool WantsBinds() {
    return g_displayPass.load(std::memory_order_relaxed) ||
           g_learning.load(std::memory_order_relaxed);
}

void OnPresent(ID3D11Device* device, ID3D11DeviceContext* immediate) {
    if (!g_enabled.load()) { return; }
    UploadUi(device, immediate);

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        EndFrame(now);
        g_frame.Clear();
        g_currentRt = nullptr;
        g_currentIsDisplay = false;
    }
    g_displayPass.store(false);

    if (g_learning.load()) {
        if (--g_learnFramesLeft <= 0) {
            g_learning.store(false);
            g_lastLearn = now;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_views.size() > kMaxKnownTextures) {
                g_views.clear();  // stale entries from freed textures
            }
            PublishTargetViews(g_target.load());
        }
    } else if (g_forceLearn.exchange(false) || now - g_lastLearn >= kLearnEveryMs) {
        g_learnFramesLeft = kLearnFrames;
        g_learning.store(true);
    }
}

void OnSetRenderTargets(UINT num, ID3D11RenderTargetView* const* views) {
    TextureId texture = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    bool display = false;
    if (num > 0 && views && views[0]) {
        ComPtr<ID3D11Resource> r;
        views[0]->GetResource(&r);
        texture = r.Get();
        display = DescribeTexture(r.Get(), &d) && IsDisplay(d);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_currentRt = texture;
    g_currentIsDisplay = display;
    if (texture) { g_frame.rendered.insert(texture); }
    if (display) { g_frame.displays[texture] = {d.Width, d.Height}; }
    g_displayPass.store(display, std::memory_order_relaxed);
}

void OnBindShaderResources(UINT num, ID3D11ShaderResourceView* const* views) {
    if (!views) { return; }

    // Classify outside the lock (COM calls), then record everything under one lock.
    struct Bound {
        ID3D11ShaderResourceView* view;
        TextureId texture;
        bool display;
    };
    std::array<Bound, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> bound{};
    UINT count = 0;
    for (UINT i = 0; i < num && count < bound.size(); ++i) {
        if (!views[i]) { continue; }
        ComPtr<ID3D11Resource> r;
        views[i]->GetResource(&r);
        D3D11_TEXTURE2D_DESC d{};
        if (!DescribeTexture(r.Get(), &d)) { continue; }
        const bool display = IsDisplay(d);
        if (display || IsPlainRenderTarget(d)) {
            bound[count] = {views[i], r.Get(), display};
            ++count;
        }
    }
    if (count == 0) { return; }

    const bool learning = g_learning.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (UINT i = 0; i < count; ++i) {
        const Bound& b = bound[i];
        if (g_currentIsDisplay) {  // a display pass: what feeds this display?
            if (b.texture == g_currentRt) { continue; }
            (b.display ? g_frame.displaySources : g_frame.plainSources)[g_currentRt].insert(
                b.texture);
        } else if (learning && g_currentRt &&
                   b.display) {  // scene pass: which views show this display?
            std::vector<ID3D11ShaderResourceView*>& list = g_views[b.texture];
            if (std::find(list.begin(), list.end(), b.view) == list.end()) {
                if (list.size() >= static_cast<size_t>(kMaxTargetViews)) {
                    list.erase(list.begin());
                }
                list.push_back(b.view);
            }
        }
    }
}

ID3D11ShaderResourceView* Substitute(ID3D11ShaderResourceView* view) {
    if (!g_enabled.load(std::memory_order_relaxed)) { return nullptr; }
    ID3D11ShaderResourceView* ui = g_uiViewRaw.load(std::memory_order_acquire);
    if (!ui) { return nullptr; }
    for (const auto& slot : g_targetViews) {
        if (slot.load(std::memory_order_relaxed) != view) { continue; }
        // The view pointer may have been recycled for another texture: confirm the resource.
        ComPtr<ID3D11Resource> r;
        view->GetResource(&r);
        if (r.Get() != g_target.load()) { return nullptr; }
        ui->AddRef();
        return ui;
    }
    return nullptr;
}

void Reset(const char* reason) {
    // Render thread only (device change): nothing else binds while Present runs.
    g_uiViewRaw.store(nullptr);
    g_target.store(nullptr);
    for (auto& slot : g_targetViews) { slot.store(nullptr); }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_views.clear();
        g_frame.Clear();
        g_currentRt = nullptr;
        g_currentIsDisplay = false;
    }

    g_uiView.Reset();
    g_uiTexture.Reset();
    g_uploadedVersion = 0;
    g_learning.store(false);
    g_forceLearn.store(true);
    logx::Log("[nav] reset ({})", reason);
}

void OnVehicleChanged(unsigned preferredWidth, unsigned preferredHeight, bool pinned) {
    g_preferredSize.store(Pack({preferredWidth, preferredHeight}));
    g_preferredPinned.store(pinned);
    g_vehicleChanged.store(true);  // applied by the render thread at the end of the next frame
}

void CycleCandidate(int direction) { g_pick.store(direction); }

void TargetSize(unsigned* width, unsigned* height) {
    const Size s = Unpack(g_targetSize.load());
    *width = s.width;
    *height = s.height;
}

bool TargetPinned() { return g_targetPinned.load(); }
}  // namespace nav
