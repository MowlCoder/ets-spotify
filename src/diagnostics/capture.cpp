#include "diagnostics/capture.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/immortal.hpp"
#include "core/log.hpp"
#include "diagnostics/screen_probe.hpp"

using Microsoft::WRL::ComPtr;

namespace capture {
namespace {
struct ContextState {
    ID3D11DeviceContext* ctx = nullptr;
    bool rtIsCandidate = false;  // the current render target is a display-sized candidate
};

std::atomic<int> g_pending{0};
std::atomic<bool> g_active{false};
std::atomic<unsigned long long> g_executeCommandLists{0};
int g_frame = 0;   // render thread only
int g_frames = 0;  // render thread only

std::mutex& g_mutex = core::Immortal<std::mutex>();  // guards the two below
std::vector<std::string>& g_lines = core::Immortal<std::vector<std::string>>();
std::vector<ContextState>& g_contexts = core::Immortal<std::vector<ContextState>>();

// g_mutex held.
size_t ContextIndexLocked(ID3D11DeviceContext* ctx) {
    const auto it = std::find_if(g_contexts.begin(), g_contexts.end(),
                                 [ctx](const ContextState& c) { return c.ctx == ctx; });
    if (it != g_contexts.end()) { return static_cast<size_t>(it - g_contexts.begin()); }
    g_contexts.push_back({ctx, false});
    return g_contexts.size() - 1;
}

// g_mutex held.
template <typename... Args>
void EmitLocked(size_t ci, std::format_string<Args...> format, Args&&... args) {
    g_lines.push_back(std::format("[cap] f{} c{} {}", g_frame, ci,
                                  std::format(format, std::forward<Args>(args)...)));
}

void Finish() {
    std::vector<std::string> lines;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        lines.swap(g_lines);
        g_contexts.clear();
    }
    for (const std::string& line : lines) { logx::WriteLine(line); }
    logx::Log("[cap] end: {} lines; ExecuteCommandList calls since start: {}", lines.size(),
              g_executeCommandLists.load());
}
}  // namespace

void Request(int frames) {
    g_pending.store(frames);
    logx::Log("[cap] requested {} frame(s)", frames);
    MessageBeep(MB_OK);
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

void OnPresent() {
    if (g_active.load()) {
        {
            const std::lock_guard<std::mutex> lock(g_mutex);
            g_lines.push_back(std::format("[cap] ---- Present, end of frame f{}", g_frame));
        }
        if (++g_frame >= g_frames) {
            g_active.store(false);
            Finish();
        }
        return;
    }

    const int pending = g_pending.exchange(0);
    if (pending > 0) {
        g_frames = pending;
        g_frame = 0;
        logx::Log("[cap] begin: recording {} frame(s)", pending);
        g_active.store(true);
    }
}

void OnSetRenderTargets(ID3D11DeviceContext* ctx, UINT num, ID3D11RenderTargetView* const* views) {
    std::string description = "none";
    int index = -1;
    if (num > 0 && views && views[0]) {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        index = probe::DescribeResource(resource.Get(), &description);
    }
    const bool candidate = index >= 0 && probe::IsInteresting(index);

    const std::lock_guard<std::mutex> lock(g_mutex);
    const size_t ci = ContextIndexLocked(ctx);
    EmitLocked(ci, "RT {}{}", description, num > 1 ? " (+more)" : "");
    g_contexts[ci].rtIsCandidate = candidate;
}

void OnSetViewports(ID3D11DeviceContext* ctx, UINT num, const D3D11_VIEWPORT* viewports) {
    if (num == 0 || !viewports) { return; }
    const std::lock_guard<std::mutex> lock(g_mutex);
    const size_t ci = ContextIndexLocked(ctx);
    if (!g_contexts[ci].rtIsCandidate) { return; }
    const D3D11_VIEWPORT& v = viewports[0];
    EmitLocked(ci, "VP x={:.0f} y={:.0f} {:.0f}x{:.0f}", v.TopLeftX, v.TopLeftY, v.Width, v.Height);
}

void OnBindShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT num,
                           ID3D11ShaderResourceView* const* views) {
    if (!views) { return; }
    for (UINT i = 0; i < num; ++i) {
        if (!views[i]) { continue; }
        const int index = probe::Lookup(views[i]);
        if (index < 0 || !probe::IsInteresting(index)) { continue; }
        const std::lock_guard<std::mutex> lock(g_mutex);
        EmitLocked(ContextIndexLocked(ctx), "PS t{} <- #{}", start + i, index);
    }
}

void OnExecuteCommandList(ID3D11DeviceContext* ctx) {
    if (g_executeCommandLists.fetch_add(1) == 0) {
        logx::Log(
            "[d3d] game executes command lists: deferred contexts exist (created before the plugin "
            "loaded?)");
    }

    if (!Active()) { return; }
    const std::lock_guard<std::mutex> lock(g_mutex);
    EmitLocked(ContextIndexLocked(ctx), "ExecuteCommandList");
}
}  // namespace capture
