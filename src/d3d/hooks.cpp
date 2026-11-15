#include "d3d/hooks.hpp"

#include <array>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

#include "core/immortal.hpp"
#include "core/log.hpp"
#include "d3d/vtable.hpp"
#include "diagnostics/capture.hpp"
#include "diagnostics/screen_probe.hpp"
#include "screen/nav_screen.hpp"

using Microsoft::WRL::ComPtr;

namespace hooks {
namespace {
// Vtable indices (IUnknown occupies 0..2).
constexpr size_t kPresentIndex = 8;                 // IDXGISwapChain::Present
constexpr size_t kPresent1Index = 22;               // IDXGISwapChain1::Present1
constexpr size_t kCreateDeferredContextIndex = 27;  // ID3D11Device::CreateDeferredContext

// Hooked ID3D11DeviceContext methods. Draw* (12, 13, 20, 21) and GenerateMips (54) are NOT hooked:
// the runtime rewrites those slots of the per-context vtable by itself (seen in the harness:
// patched, then reverted before the next call), so a hook there is lost and a saved "original"
// could be a variant the runtime no longer wants.
enum ContextSlot {
    kPSSetShaderResources,
    kOMSetRenderTargets,
    kRSSetViewports,
    kExecuteCommandList,
    kSlotCount
};
constexpr std::array<size_t, kSlotCount> kContextSlotIndex{8, 33, 44, 58};

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                                 const DXGI_PRESENT_PARAMETERS*);
using PFN_CreateDeferredContext = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT,
                                                              ID3D11DeviceContext**);
using PFN_PSSetShaderResources = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                          ID3D11ShaderResourceView* const*);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                        ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                    const D3D11_VIEWPORT*);
using PFN_ExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*,
                                                        BOOL);

std::atomic<bool> g_enabled{false};
std::mutex& g_installMutex = core::Immortal<std::mutex>();  // guards g_installed
bool g_installed = false;

PFN_Present g_origPresent = nullptr;
PFN_Present1 g_origPresent1 = nullptr;
PFN_CreateDeferredContext g_origCreateDeferredContext = nullptr;
void** g_swapchainVtable = nullptr;  // of the dummy objects, to compare with the game's (log only)
void** g_deviceVtable = nullptr;

// The D3D11 runtime gives each context its own heap-allocated vtable (dummy immediate, game
// immediate and deferred contexts all differ). So every context the game uses is patched on its
// own, and the originals are kept per vtable. Entries are only appended, and published before their
// slots are patched, so readers need no lock.
struct ContextVtable {
    void** vtable;
    std::array<void*, kSlotCount> original;
};
constexpr int kMaxContextVtables = 64;
std::array<ContextVtable, kMaxContextVtables> g_contextVtables;
std::atomic<int> g_contextVtableCount{0};
std::mutex& g_patchMutex = core::Immortal<std::mutex>();  // serializes appends

template <typename T>
T Original(ID3D11DeviceContext* ctx, ContextSlot slot) {
    void** vtable = *reinterpret_cast<void***>(ctx);
    const int n = g_contextVtableCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_contextVtables[i].vtable == vtable) {
            return reinterpret_cast<T>(g_contextVtables[i].original[slot]);
        }
    }
    return nullptr;  // unreachable: a vtable is registered before its slots are patched
}

// ---- Context hooks (hot: called thousands of times per frame) ----

void STDMETHODCALLTYPE HookPSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT num,
                                                ID3D11ShaderResourceView* const* views) {
    const auto original = Original<PFN_PSSetShaderResources>(ctx, kPSSetShaderResources);
    if (!g_enabled.load(std::memory_order_relaxed) || !views || num == 0 ||
        num > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        original(ctx, start, num, views);
        return;
    }

    const bool navOn = nav::Enabled();
    const bool probeOn = probe::Active();
    if (capture::Active()) { capture::OnBindShaderResources(ctx, start, num, views); }
    if (navOn && nav::WantsBinds()) {
        nav::OnBindShaderResources(num, views);  // the game's own views
    }
    if (!navOn && !probeOn) {
        original(ctx, start, num, views);
        return;
    }

    if (probeOn) { probe::CountCall(); }
    std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> replaced{};
    bool any = false;
    for (UINT i = 0; i < num; ++i) {
        ID3D11ShaderResourceView* view = views[i];
        ID3D11ShaderResourceView* substitute = nullptr;
        if (view && navOn) { substitute = nav::Substitute(view); }
        if (view && !substitute && probeOn) { substitute = probe::Substitute(view); }
        replaced[i] = substitute ? substitute : view;
        any |= substitute != nullptr;
    }

    if (!any) {
        original(ctx, start, num, views);
        return;
    }
    original(ctx, start, num, replaced.data());
    for (UINT i = 0; i < num; ++i) {
        if (replaced[i] != views[i]) {
            replaced[i]->Release();  // the reference from Substitute()
        }
    }
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT num,
                                              ID3D11RenderTargetView* const* views,
                                              ID3D11DepthStencilView* depth) {
    if (g_enabled.load(std::memory_order_relaxed)) {
        if (capture::Active()) { capture::OnSetRenderTargets(ctx, num, views); }
        if (nav::Enabled()) { nav::OnSetRenderTargets(num, views); }
    }
    Original<PFN_OMSetRenderTargets>(ctx, kOMSetRenderTargets)(ctx, num, views, depth);
}

void STDMETHODCALLTYPE HookRSSetViewports(ID3D11DeviceContext* ctx, UINT num,
                                          const D3D11_VIEWPORT* viewports) {
    if (g_enabled.load(std::memory_order_relaxed) && capture::Active()) {
        capture::OnSetViewports(ctx, num, viewports);
    }
    Original<PFN_RSSetViewports>(ctx, kRSSetViewports)(ctx, num, viewports);
}

void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list,
                                              BOOL restore) {
    if (g_enabled.load(std::memory_order_relaxed)) { capture::OnExecuteCommandList(ctx); }
    Original<PFN_ExecuteCommandList>(ctx, kExecuteCommandList)(ctx, list, restore);
}

const std::array<void*, kSlotCount> kContextHooks{
    reinterpret_cast<void*>(&HookPSSetShaderResources),
    reinterpret_cast<void*>(&HookOMSetRenderTargets),
    reinterpret_cast<void*>(&HookRSSetViewports),
    reinterpret_cast<void*>(&HookExecuteCommandList),
};

void EnsureContextHooked(ID3D11DeviceContext* ctx, const char* what) {
    void** vtable = *reinterpret_cast<void***>(ctx);
    std::lock_guard<std::mutex> lock(g_patchMutex);
    const int n = g_contextVtableCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_contextVtables[i].vtable == vtable) { return; }
    }
    if (n >= kMaxContextVtables) {
        logx::Log("[d3d] too many context vtables, {} not hooked", what);
        return;
    }

    ContextVtable& entry = g_contextVtables[n];
    entry.vtable = vtable;
    for (int s = 0; s < kSlotCount; ++s) { entry.original[s] = vtable[kContextSlotIndex[s]]; }
    g_contextVtableCount.store(n + 1, std::memory_order_release);  // publish before patching

    int patched = 0;
    for (int s = 0; s < kSlotCount; ++s) {
        if (vt::PatchSlot(&vtable[kContextSlotIndex[s]], kContextHooks[s])) { ++patched; }
    }
    logx::Log("[d3d] {} context vtable {}: {}/{} methods hooked", what, static_cast<void*>(vtable),
              patched, static_cast<int>(kSlotCount));
}

// ---- Device and swapchain hooks ----

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(ID3D11Device* device, UINT flags,
                                                    ID3D11DeviceContext** out) {
    const HRESULT hr = g_origCreateDeferredContext(device, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) { logx::Log("[d3d] game creates deferred contexts"); }
        EnsureContextHooked(*out, "deferred");
    }
    return hr;
}

void OnNewGameDevice(IDXGISwapChain* sc, ID3D11Device* device) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d))) {
        logx::Log(
            "[d3d] game swapchain: {}x{} fmt={} buffers={} windowed={} swap_effect={} "
            "vtable_matches_dummy={}",
            d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format),
            d.BufferCount, d.Windowed, static_cast<int>(d.SwapEffect),
            *reinterpret_cast<void***>(sc) == g_swapchainVtable);
    }
    logx::Log(
        "[d3d] game device {}: feature_level=0x{:X} creation_flags=0x{:X} vtable_matches_dummy={}",
        static_cast<void*>(device), static_cast<unsigned>(device->GetFeatureLevel()),
        device->GetCreationFlags(), *reinterpret_cast<void***>(device) == g_deviceVtable);

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (ctx) { EnsureContextHooked(ctx.Get(), "game immediate"); }
}

// End of a frame, on the game's render thread (so plain statics are enough here).
void OnPresent(IDXGISwapChain* sc) {
    if (!g_enabled.load(std::memory_order_relaxed)) { return; }

    static void* s_lastDevice = nullptr;
    ComPtr<ID3D11Device> device;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device)))) {
        static bool s_warned = false;
        if (!s_warned) { logx::Log("[d3d] swapchain has no D3D11 device (game not on DX11?)"); }
        s_warned = true;
        return;
    }
    if (device.Get() !=
        s_lastDevice) {  // first frame, or the game recreated its device (graphics settings)
        if (s_lastDevice) {
            probe::Reset("game device changed");
            nav::Reset("game device changed");
        }
        OnNewGameDevice(sc, device.Get());
        s_lastDevice = device.Get();
    }

    DXGI_SWAP_CHAIN_DESC d{};  // the diagnostics need the backbuffer size (cheap: a struct copy)
    if (SUCCEEDED(sc->GetDesc(&d))) {
        probe::SetBackbuffer(d.BufferDesc.Width, d.BufferDesc.Height);
    }
    probe::OnPresentTick();
    capture::OnPresent();

    ComPtr<ID3D11DeviceContext> immediate;
    device->GetImmediateContext(&immediate);
    nav::OnPresent(device.Get(), immediate.Get());
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    OnPresent(sc);
    return g_origPresent(sc, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* params) {
    OnPresent(sc);
    return g_origPresent1(sc, syncInterval, flags, params);
}

// ---- Installation ----

template <typename T>
bool PatchOnce(void* object, size_t index, void* hook, T* original, const char* name) {
    void** slot = vt::SlotOf(object, index);
    if (*slot == hook) { return true; }
    *original = reinterpret_cast<T>(*slot);
    const bool ok = vt::PatchSlot(slot, hook) != nullptr;
    logx::Log("[d3d] {} hook {}", name, ok ? "installed" : "FAILED");
    return ok;
}

HMODULE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}

// Swapchain and device vtables are shared by every instance (they belong to the runtime), so
// hooking throwaway objects on a hidden window hooks the game's objects too.
bool InstallLocked() {
    const HMODULE module = ThisModule();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = module;
    wc.lpszClassName = L"ets_spotify_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, module, nullptr);
    if (!hwnd) {
        logx::Log("[d3d] dummy window failed: {}", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 64;
    scd.BufferDesc.Height = 64;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr std::array<D3D_FEATURE_LEVEL, 3> levels{
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type :
         {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {  // WARP: same runtime vtables
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels.data(),
                                           static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                                           &scd, &sc, &device, nullptr, &ctx);
        if (SUCCEEDED(hr)) { break; }
    }

    bool ok = false;
    if (FAILED(hr)) {
        logx::Log("[d3d] dummy device creation failed: 0x{:08X}", static_cast<unsigned long>(hr));
    } else {
        g_swapchainVtable = *reinterpret_cast<void***>(sc.Get());
        g_deviceVtable = *reinterpret_cast<void***>(device.Get());
        ok = PatchOnce(sc.Get(), kPresentIndex, reinterpret_cast<void*>(&HookPresent),
                       &g_origPresent, "Present");
        ComPtr<IDXGISwapChain1> sc1;
        if (SUCCEEDED(sc.As(&sc1))) {
            PatchOnce(sc1.Get(), kPresent1Index, reinterpret_cast<void*>(&HookPresent1),
                      &g_origPresent1, "Present1");
        }
        PatchOnce(device.Get(), kCreateDeferredContextIndex,
                  reinterpret_cast<void*>(&HookCreateDeferredContext), &g_origCreateDeferredContext,
                  "CreateDeferredContext");
        // The game's immediate context has its own vtable: it is hooked on the first Present.
    }

    ctx.Reset();
    device.Reset();
    sc.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, module);
    return ok;
}
}  // namespace

bool Install() {
    bool installed;
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if (!g_installed) { g_installed = InstallLocked(); }
        installed = g_installed;
    }
    g_enabled.store(installed);
    return installed;
}

unsigned HookedSlotMask(void* context) {
    void** vtable = *reinterpret_cast<void***>(context);
    unsigned mask = 0;
    for (int s = 0; s < kSlotCount; ++s) {
        if (vtable[kContextSlotIndex[s]] == kContextHooks[s]) { mask |= 1u << s; }
    }
    return mask;
}

void Disable() {
    g_enabled.store(false);
    nav::SetEnabled(false);  // its D3D objects stay: the render thread may still be running
    probe::DebugSet(probe::kModeOff, 0);
    probe::Reset("plugin shutdown");
}
}  // namespace hooks
