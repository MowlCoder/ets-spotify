// Offline test that plays the role of the game.
// Creates its own D3D11 device first (as the game does), loads the plugin through the SDK entry
// point and drives it like the game would: the diagnostic probe, the frame capture, the navigation
// screen (display chains as captured from ETS2, pause-menu and texture-pool cases), truck changes
// and the calibration mode. Ends with a measurement of the hook overhead.
//
// usage: probe_harness <path to ets_spotify.dll> <output dir>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "game/scs_sdk.hpp"

using Microsoft::WRL::ComPtr;

namespace {
int g_failures = 0;

void Check(bool condition, const char* what) {
    printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) { ++g_failures; }
}

void SCSAPIFUNC HarnessLog(const scs_log_type_t type, const scs_string_t message) {
    printf("  [game log %d] %s\n", type, message);
}
scs_telemetry_event_callback_t g_configurationCallback = nullptr;
scs_telemetry_event_callback_t g_startedCallback = nullptr;
scs_result_t SCSAPIFUNC HarnessRegisterEvent(const scs_event_t event,
                                             const scs_telemetry_event_callback_t callback,
                                             const scs_context_t) {
    if (event == SCS_TELEMETRY_EVENT_configuration) { g_configurationCallback = callback; }
    if (event == SCS_TELEMETRY_EVENT_started) { g_startedCallback = callback; }
    return SCS_RESULT_ok;
}

scs_named_value_t StringAttribute(const char* name, const char* value) {
    scs_named_value_t a{};
    a.name = name;
    a.index = SCS_U32_NIL;
    a.value.type = SCS_VALUE_TYPE_string;
    a.value.value_string = value;
    return a;
}
scs_result_t SCSAPIFUNC HarnessUnregisterEvent(const scs_event_t) { return SCS_RESULT_ok; }

using InitFn = scs_result_t(SCSAPIFUNC*)(scs_u32_t, const void*);
using ShutdownFn = void(SCSAPIFUNC*)();
using DebugSetFn = void (*)(int, int);
using DebugCountFn = int (*)();
using DebugCaptureFn = void (*)(int);

struct TexSrv {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
};

TexSrv MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind) {
    TexSrv t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;

    dev->CreateTexture2D(&td, nullptr, &t.tex);
    if (t.tex) { dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv); }
    return t;
}

struct RenderTarget {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

// Render target like the game's display textures (withMips -> GENERATE_MIPS, full chain).
RenderTarget MakeRenderTarget(ID3D11Device* dev, UINT w, UINT h, bool withMips) {
    RenderTarget t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = withMips ? 0 : 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = withMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    dev->CreateTexture2D(&td, nullptr, &t.tex);
    if (t.tex) {
        dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
        dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv);
    }
    return t;
}

void SetRt(ID3D11DeviceContext* ctx, const RenderTarget& t) {
    ID3D11RenderTargetView* rtv = t.rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
}

// Binds one view and returns what the context actually holds (raw pointer, comparison only).
ID3D11ShaderResourceView* BindAndGet(ID3D11DeviceContext* ctx, UINT slot,
                                     ID3D11ShaderResourceView* view) {
    ctx->PSSetShaderResources(slot, 1, &view);
    ID3D11ShaderResourceView* got = nullptr;
    ctx->PSGetShaderResources(slot, 1, &got);
    if (got) {
        got->Release();  // still bound, so still alive
    }
    return got;
}

// Writes mip 0 as a 32-bit BMP. flipForView: the texture stores rows bottom-up (nav display),
// so write the BMP bottom-up too and it shows the right way round.
bool DumpToBmp(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv,
               const std::wstring& path, UINT* outW, UINT* outH, bool* nonUniform,
               bool flipForView = false, uint32_t* firstPixel = nullptr,
               uint64_t* checksum = nullptr) {
    ComPtr<ID3D11Resource> res;
    srv->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) { return false; }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    *outW = td.Width;
    *outH = td.Height;
    const bool sourceIsBgra =
        td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) { return false; }
    ctx->CopyResource(staging.Get(), tex.Get());

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { return false; }
    std::vector<uint8_t> bgra(static_cast<size_t>(td.Width) * td.Height * 4);
    const uint32_t first = *static_cast<const uint32_t*>(map.pData);
    if (firstPixel) { *firstPixel = first; }

    uint64_t hash = 1469598103934665603ull;  // FNV-1a over all pixels
    *nonUniform = false;
    for (UINT y = 0; y < td.Height; ++y) {
        const uint8_t* row =
            static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
        for (UINT x = 0; x < td.Width; ++x) {
            const uint8_t* s = row + x * 4;
            uint8_t* d = &bgra[(static_cast<size_t>(y) * td.Width + x) * 4];
            d[0] = sourceIsBgra ? s[0] : s[2];
            d[1] = s[1];
            d[2] = sourceIsBgra ? s[2] : s[0];
            d[3] = s[3];
            if (*reinterpret_cast<const uint32_t*>(s) != first) { *nonUniform = true; }
            hash = (hash ^ *reinterpret_cast<const uint32_t*>(s)) * 1099511628211ull;
        }
    }

    ctx->Unmap(staging.Get(), 0);
    if (checksum) { *checksum = hash; }

    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(td.Width);
    ih.biHeight = flipForView ? static_cast<LONG>(td.Height) : -static_cast<LONG>(td.Height);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(bgra.size());

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) { return false; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
    return true;
}

std::string ReadFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: probe_harness <ets_spotify.dll> <output dir>\n");
        return 2;
    }

    const std::wstring dllPath = argv[1];
    const std::wstring outDir = argv[2];
    const std::wstring logPath = outDir + L"\\harness_plugin.log";
    const std::wstring iniPath = outDir + L"\\ets_spotify.ini";  // next to the log
    DeleteFileW(iniPath.c_str());  // start without saved truck settings
    SetEnvironmentVariableW(L"ETS_SPOTIFY_LOG", logPath.c_str());

    // 1. The "game" creates its device and swapchain before plugins are loaded.
    printf("game device\n");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"probe_harness";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe harness", WS_OVERLAPPEDWINDOW, 0, 0,
                                1280, 720, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 1280;
    scd.BufferDesc.Height = 720;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = E_FAIL;
    D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
    for (D3D_DRIVER_TYPE type :
         {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {  // WARP: machines without a GPU
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &scd, &sc, &dev, nullptr, &ctx);
        driverType = type;
        if (SUCCEEDED(hr)) { break; }
    }

    if (driverType == D3D_DRIVER_TYPE_WARP) { printf("  info no hardware device, using WARP\n"); }
    Check(SUCCEEDED(hr), "game device and flip-model swapchain created");
    if (FAILED(hr)) { return 1; }

    // 2. Load the plugin the way the game does.
    printf("plugin load\n");
    HMODULE plugin = LoadLibraryW(dllPath.c_str());
    Check(plugin != nullptr, "plugin DLL loads");
    if (!plugin) { return 1; }

    const auto init = reinterpret_cast<InitFn>(GetProcAddress(plugin, "scs_telemetry_init"));
    const auto shutdown =
        reinterpret_cast<ShutdownFn>(GetProcAddress(plugin, "scs_telemetry_shutdown"));
    const auto debugSet =
        reinterpret_cast<DebugSetFn>(GetProcAddress(plugin, "ets_spotify_debug_set_probe"));
    const auto debugCount =
        reinterpret_cast<DebugCountFn>(GetProcAddress(plugin, "ets_spotify_debug_candidate_count"));
    const auto debugCapture =
        reinterpret_cast<DebugCaptureFn>(GetProcAddress(plugin, "ets_spotify_debug_capture"));
    Check(init && shutdown, "exports scs_telemetry_init / scs_telemetry_shutdown");
    Check(debugSet && debugCount && debugCapture, "exports debug helpers");
    if (!init || !shutdown || !debugSet || !debugCount || !debugCapture) { return 1; }

    scs_telemetry_init_params_v101_t params{};
    params.common.game_name = "Euro Truck Simulator 2";
    params.common.game_id = "eut2";
    params.common.game_version = scs_make_version(1, 18);
    params.common.log = HarnessLog;
    params.register_for_event = HarnessRegisterEvent;
    params.unregister_from_event = HarnessUnregisterEvent;

    Check(init(scs_make_version(2, 0), &params) == SCS_RESULT_unsupported,
          "rejects unknown API version 2.0");
    Check(init(SCS_TELEMETRY_VERSION_1_01, &params) == SCS_RESULT_ok,
          "init with API 1.01 succeeds");

    sc->Present(0, 0);  // lets the plugin learn the backbuffer size

    TexSrv screen = MakeTexture(dev.Get(), 1024, 600, DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    TexSrv full = MakeTexture(dev.Get(), 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    TexSrv depthLike = MakeTexture(dev.Get(), 1024, 600, DXGI_FORMAT_R32_FLOAT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    TexSrv plain =
        MakeTexture(dev.Get(), 1024, 600, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    Check(screen.srv && full.srv && depthLike.srv && plain.srv, "test textures created");

    printf("mode OFF\n");
    Check(BindAndGet(ctx.Get(), 0, screen.srv.Get()) == screen.srv.Get(), "binding untouched");
    Check(debugCount() == 0, "nothing classified while off");

    printf("mode ALL\n");
    debugSet(1, 0);
    ID3D11ShaderResourceView* replaced = BindAndGet(ctx.Get(), 0, screen.srv.Get());
    Check(replaced && replaced != screen.srv.Get(), "off-screen render target replaced by pattern");
    Check(BindAndGet(ctx.Get(), 1, full.srv.Get()) == full.srv.Get(),
          "screen-sized render target left alone");
    Check(BindAndGet(ctx.Get(), 2, depthLike.srv.Get()) == depthLike.srv.Get(),
          "non-colour render target ignored");
    Check(BindAndGet(ctx.Get(), 3, plain.srv.Get()) == plain.srv.Get(),
          "non-render-target texture ignored");
    Check(debugCount() == 2, "two candidates: off-screen #0 and screen-sized #1");

    // Render scaling: the game renders the scene above the backbuffer size (2880x2160 vs
    // 1920x1080 in ETS2). That buffer and its half-size chain must not be replaced in ALL mode.
    TexSrv scene = MakeTexture(dev.Get(), 1920, 1440, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    TexSrv sceneHalf = MakeTexture(dev.Get(), 960, 720, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    Check(BindAndGet(ctx.Get(), 0, scene.srv.Get()) == scene.srv.Get(),
          "scene-resolution buffer left alone");
    Check(BindAndGet(ctx.Get(), 0, sceneHalf.srv.Get()) == sceneHalf.srv.Get(),
          "half scene-resolution buffer left alone");
    Check(debugCount() == 4, "scene buffers still listed as candidates #2 and #3 for SINGLE mode");

    const std::array<ID3D11ShaderResourceView*, 3> batch{plain.srv.Get(), screen.srv.Get(),
                                                         full.srv.Get()};
    ctx->PSSetShaderResources(4, 3, batch.data());
    std::array<ID3D11ShaderResourceView*, 3> gotBatch{};
    ctx->PSGetShaderResources(4, 3, gotBatch.data());
    Check(gotBatch[0] == batch[0] && gotBatch[1] && gotBatch[1] != batch[1] &&
              gotBatch[2] == batch[2],
          "multi-slot bind: only the candidate slot replaced");
    for (ID3D11ShaderResourceView* v : gotBatch) {
        if (v) { v->Release(); }
    }

    ComPtr<ID3D11DeviceContext> deferred;
    if (SUCCEEDED(dev->CreateDeferredContext(0, &deferred))) {
        ID3D11ShaderResourceView* got = BindAndGet(deferred.Get(), 0, screen.srv.Get());
        Check(got && got != screen.srv.Get(), "deferred context is hooked too");
    } else {
        printf("  skip deferred contexts not supported here\n");
    }

    UINT pw = 0;
    UINT ph = 0;
    bool nonUniform = false;
    const std::wstring bmp = outDir + L"\\pattern_0.bmp";
    Check(replaced && DumpToBmp(dev.Get(), ctx.Get(), replaced, bmp, &pw, &ph, &nonUniform),
          "pattern texture read back");
    Check(pw == 512 && ph == 256 && nonUniform, "pattern is 512x256 and not a flat colour");

    printf("mode SINGLE\n");
    debugSet(2, 1);
    Check(BindAndGet(ctx.Get(), 0, full.srv.Get()) != full.srv.Get(),
          "selected #1 (screen-sized) replaced");
    Check(BindAndGet(ctx.Get(), 1, screen.srv.Get()) == screen.srv.Get(),
          "unselected #0 untouched");

    for (int i = 0; i < 3; ++i) { sc->Present(0, 0); }

    printf("mode OFF again\n");
    debugSet(0, 0);
    Check(BindAndGet(ctx.Get(), 0, screen.srv.Get()) == screen.srv.Get(), "binding untouched");

    printf("frame capture\n");
    ComPtr<ID3D11RenderTargetView> screenRtv;
    dev->CreateRenderTargetView(screen.tex.Get(), nullptr, &screenRtv);
    Check(screenRtv != nullptr, "capture test render target view created");

    using SlotsFn = unsigned (*)(void*);
    const auto slots =
        reinterpret_cast<SlotsFn>(GetProcAddress(plugin, "ets_spotify_debug_hooked_slots"));
    Check(slots && slots(ctx.Get()) == 0xF, "all 4 context hooks in place before drawing");

    debugCapture(2);
    sc->Present(0, 0);  // capture starts at the next frame boundary

    ID3D11RenderTargetView* const rtv = screenRtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, 1024.0f, 600.0f, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    constexpr std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(screenRtv.Get(), clear.data());
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* screenView = screen.srv.Get();
    ctx->PSSetShaderResources(2, 1, &screenView);
    Check(slots && slots(ctx.Get()) == 0xF,
          "all 4 context hooks still in place after state changes");

    sc->Present(0, 0);
    sc->Present(0, 0);  // second recorded frame ends -> record is written

    printf("nav screen\n");
    Check(dev->GetDeviceRemovedReason() == S_OK, "device still alive");

    // Same chain as the frame capture from the game (Iveco, dash-top GPS).
    RenderTarget map = MakeRenderTarget(dev.Get(), 1024, 1024, false);
    RenderTarget navInter = MakeRenderTarget(dev.Get(), 256, 256, true);
    RenderTarget navFinal = MakeRenderTarget(dev.Get(), 256, 256, true);
    RenderTarget clusterInter = MakeRenderTarget(dev.Get(), 512, 512, true);
    RenderTarget clusterFinal = MakeRenderTarget(dev.Get(), 512, 512, true);
    RenderTarget sceneRt = MakeRenderTarget(dev.Get(), 1920, 1440, false);
    Check(map.rtv && navInter.rtv && navFinal.rtv && clusterInter.rtv && clusterFinal.rtv &&
              sceneRt.rtv,
          "display chain textures created");

    using NavFn = void (*)(int);
    const auto debugNav = reinterpret_cast<NavFn>(GetProcAddress(plugin, "ets_spotify_debug_nav"));
    Check(debugNav != nullptr, "exports ets_spotify_debug_nav");

    // A plain render target the display passes sample but the game does not redraw every frame
    // (like a font cache filled when the pause menu opens). It must not count as "the map".
    RenderTarget fontCache = MakeRenderTarget(dev.Get(), 1024, 1024, false);

    // The game's display textures come from a pool and can change roles (seen after the pause
    // menu).
    struct Roles {
        RenderTarget* navInter;
        RenderTarget* navFinal;
        RenderTarget* clusterInter;
        RenderTarget* clusterFinal;
        bool fontCacheSampled;
        bool clusterSamplesFreshRt =
            false;           // as logged in game after the menu: both chains look map-fed
        bool noMap = false;  // a truck without a navigation screen: nothing renders a map
    };
    const Roles normal{&navInter, &navFinal, &clusterInter, &clusterFinal, false};
    const Roles afterMenu{&navInter, &navFinal, &clusterInter, &clusterFinal, true};
    const Roles swapped{&clusterInter, &clusterFinal, &navInter, &navFinal, true};
    const Roles twoMapScreens{&navInter, &navFinal, &clusterInter, &clusterFinal, false, true};
    const Roles noMapTruck{&navInter, &navFinal, &clusterInter, &clusterFinal, false, false, true};

    ID3D11ShaderResourceView* navSeen = nullptr;      // what the nav screen got in the scene pass
    ID3D11ShaderResourceView* clusterSeen = nullptr;  // what the cluster got
    ID3D11ShaderResourceView* interSeen = nullptr;
    auto simulateFrame = [&](const Roles& r) {
        if (!r.noMap) { SetRt(ctx.Get(), map); }
        SetRt(ctx.Get(), *r.navInter);
        if (!r.noMap) { BindAndGet(ctx.Get(), 0, map.srv.Get()); }
        if (r.fontCacheSampled) { BindAndGet(ctx.Get(), 1, fontCache.srv.Get()); }
        SetRt(ctx.Get(), *r.navFinal);
        interSeen = BindAndGet(ctx.Get(), 0, r.navInter->srv.Get());
        SetRt(ctx.Get(), *r.clusterInter);
        if (r.fontCacheSampled) { BindAndGet(ctx.Get(), 1, fontCache.srv.Get()); }
        if (r.clusterSamplesFreshRt) { BindAndGet(ctx.Get(), 1, map.srv.Get()); }
        SetRt(ctx.Get(), *r.clusterFinal);
        BindAndGet(ctx.Get(), 0, r.clusterInter->srv.Get());
        SetRt(ctx.Get(), sceneRt);
        navSeen = BindAndGet(ctx.Get(), 6, r.navFinal->srv.Get());
        clusterSeen = BindAndGet(ctx.Get(), 7, r.clusterFinal->srv.Get());
        sc->Present(0, 0);
    };

    if (debugNav) { debugNav(1); }
    int frames = 0;
    for (; frames < 400;
         ++frames) {  // view learning takes a few frames; the UI thread needs Direct2D start-up
        simulateFrame(normal);
        if (navSeen != navFinal.srv.Get()) { break; }
        Sleep(10);
    }

    printf("  info substituted after %d frame(s)\n", frames);
    Check(navSeen && navSeen != navFinal.srv.Get(),
          "nav: final map display replaced by the UI texture");
    Check(clusterSeen == clusterFinal.srv.Get(),
          "nav: cluster display (no map in its chain) untouched");
    Check(interSeen == navInter.srv.Get(), "nav: intermediate display texture untouched");
    ID3D11ShaderResourceView* uiView = navSeen;

    bool clusterHit = false;
    bool navLost = false;
    for (int i = 0; i < 30; ++i) {
        simulateFrame(afterMenu);
        clusterHit |= clusterSeen != clusterFinal.srv.Get();
        navLost |= navSeen == navFinal.srv.Get();
    }

    Check(!clusterHit, "after menu (font cache sampled by both chains): cluster never replaced");
    Check(!navLost, "after menu: nav keeps the UI");

    int clusterGlitchFrames = 0;
    int navBackAt = -1;
    for (int i = 0; i < 60; ++i) {
        simulateFrame(swapped);
        if (clusterSeen != navFinal.srv.Get()) {
            ++clusterGlitchFrames;  // 256 texture now shows the cluster
        }
        if (navBackAt < 0 && navSeen != clusterFinal.srv.Get()) { navBackAt = i; }
        Sleep(5);
    }

    printf("  info after the pool swap: cluster replaced for %d frame(s), nav back at frame %d\n",
           clusterGlitchFrames, navBackAt);
    Check(clusterGlitchFrames <= 1,
          "pool swap: the cluster's new texture is replaced for at most one frame");
    Check(navBackAt >= 0 && navBackAt <= 2,
          "pool swap: the nav's new texture gets the UI within two frames");

    if (uiView && uiView != navFinal.srv.Get()) {
        navSeen = uiView;
        ComPtr<ID3D11Resource> uiRes;
        navSeen->GetResource(&uiRes);
        ComPtr<ID3D11Texture2D> uiTex;
        D3D11_TEXTURE2D_DESC ud{};
        if (uiRes && SUCCEEDED(uiRes.As(&uiTex))) { uiTex->GetDesc(&ud); }
        Check(ud.Width == 512 && ud.Height == 512 && ud.MipLevels == 10 &&
                  ud.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
              "nav: UI texture is 512x512 sRGB with 10 mips (square like the game's)");

        UINT uw = 0;
        UINT uh = 0;
        bool uiNonUniform = false;
        Check(DumpToBmp(dev.Get(), ctx.Get(), navSeen, outDir + L"\\nav_ui.bmp", &uw, &uh,
                        &uiNonUniform, true) &&
                  uiNonUniform,
              "nav: UI image read back and not empty");
    }

    printf("trucks\n");
    Check(g_configurationCallback != nullptr, "plugin listens to configuration events");

    const std::array<scs_named_value_t, 5> truckAttrs{StringAttribute("brand_id", "iveco"),
                                                      StringAttribute("brand", "Iveco"),
                                                      StringAttribute("id", "iveco.sway"),
                                                      StringAttribute("name", "S-Way"),
                                                      {}};
    const scs_telemetry_configuration_t truckConfig{"truck", truckAttrs.data()};
    if (g_configurationCallback) {
        g_configurationCallback(SCS_TELEMETRY_EVENT_configuration, &truckConfig, nullptr);
    }
    if (g_startedCallback) {
        g_startedCallback(SCS_TELEMETRY_EVENT_started, nullptr, nullptr);  // driving
    }

    bool navKept = true;
    for (int i = 0; i < 60; ++i) {
        simulateFrame(normal);
        if (i >= 2) {
            navKept &= navSeen != navFinal.srv.Get() && clusterSeen == clusterFinal.srv.Get();
        }
        Sleep(20);
    }

    Check(navKept, "after a truck change the nav screen is found again at once");

    using CalibFn = void (*)(int);
    const auto debugCalib =
        reinterpret_cast<CalibFn>(GetProcAddress(plugin, "ets_spotify_debug_calibrate"));
    Check(debugCalib != nullptr, "exports ets_spotify_debug_calibrate");
    if (debugCalib) {
        debugCalib(1);  // begin
        bool pink = false;
        for (int i = 0; i < 100 && !pink; ++i) {
            simulateFrame(normal);
            Sleep(10);

            UINT cw = 0;
            UINT ch = 0;
            bool cNonUniform = false;
            uint32_t corner = 0;
            DumpToBmp(dev.Get(), ctx.Get(), navSeen, outDir + L"\\nav_calibration.bmp", &cw, &ch,
                      &cNonUniform, true, &corner);
            pink = corner == 0xFFFF2BD6;  // outside the frame
        }
        Check(pink, "calibration: picture on the nav screen, pink outside the frame");

        for (int i = 0; i < 3; ++i) {
            debugCalib(3);  // left edge +3 steps
        }
        for (int i = 0; i < 4; ++i) {
            debugCalib(2);  // left -> right -> top -> bottom -> proportions
        }
        for (int i = 0; i < 5; ++i) {
            debugCalib(3);  // proportions +5 steps
        }

        debugCalib(2);  // -> screen
        debugCalib(3);  // next screen by size: the 512x512 one
        bool clusterPicked = false;
        for (int i = 0; i < 10 && !clusterPicked; ++i) {
            simulateFrame(
                twoMapScreens);  // both screens look map-fed, as in game after the pause menu
            clusterPicked = clusterSeen != clusterFinal.srv.Get() && navSeen == navFinal.srv.Get();
        }
        Check(clusterPicked, "calibration: '.' switches to the other map screen");

        debugCalib(4);  // and back
        bool navPicked = false;
        for (int i = 0; i < 10 && !navPicked; ++i) {
            simulateFrame(twoMapScreens);
            navPicked = navSeen != navFinal.srv.Get() && clusterSeen == clusterFinal.srv.Get();
        }
        Check(navPicked, "calibration: ',' switches back");

        debugCalib(0);  // finish: save
        auto saved = [&](const wchar_t* key) {
            std::array<wchar_t, 32> value{};
            GetPrivateProfileStringW(L"nav.iveco.sway", key, L"", value.data(),
                                     static_cast<DWORD>(value.size()), iniPath.c_str());
            return std::wstring(value.data());
        };
        Check(saved(L"u0") == L"0.052" && saved(L"aspect") == L"1.100" &&
                  saved(L"display") == L"256x256",
              "calibration: saved to [nav.iveco.sway] (u0 +3 steps, proportions +5 steps, screen "
              "256x256)");

        bool spotifyBack = false;
        for (int i = 0; i < 100 && !spotifyBack; ++i) {
            simulateFrame(normal);
            Sleep(10);

            UINT cw = 0;
            UINT ch = 0;
            bool cNonUniform = false;
            uint32_t corner = 0;
            DumpToBmp(dev.Get(), ctx.Get(), navSeen, outDir + L"\\nav_after_calibration.bmp", &cw,
                      &ch, &cNonUniform, true, &corner);
            spotifyBack = corner != 0xFFFF2BD6;
        }
        Check(spotifyBack, "calibration: the player is back after saving");
    }

    // A title too long for the screen: still at first, then moving back and forth, repainted often.
    printf("long titles\n");
    using ShowTrackFn = void (*)(const char*, const char*);
    const auto showTrack =
        reinterpret_cast<ShowTrackFn>(GetProcAddress(plugin, "ets_spotify_debug_show_track"));
    Check(showTrack != nullptr, "exports ets_spotify_debug_show_track");
    if (showTrack) {
        showTrack("A Very Long Song Title That Cannot Possibly Fit On The Cab Screen", "2hollis");
        const ULONGLONG shown = GetTickCount64();
        auto pictureAt = [&](ULONGLONG ms, const wchar_t* name) {
            while (GetTickCount64() - shown < ms) { Sleep(5); }
            simulateFrame(normal);  // Present uploads the latest picture, the scene pass shows it

            UINT pw = 0;
            UINT ph = 0;
            bool nonUniform = false;
            uint64_t checksum = 0;
            DumpToBmp(dev.Get(), ctx.Get(), navSeen, outDir + name, &pw, &ph, &nonUniform, true,
                      nullptr, &checksum);
            return checksum;
        };

        const uint64_t start1 = pictureAt(600, L"\\title_start.bmp");
        const uint64_t start2 = pictureAt(1500, L"\\title_start2.bmp");

        int changes = 0;
        uint64_t previous = pictureAt(3000, L"\\title_moving.bmp");
        const uint64_t moving = previous;
        for (int i = 1; i <= 5; ++i) {
            const uint64_t next = pictureAt(3000 + i * 120, L"\\title_moving2.bmp");
            changes += next != previous;
            previous = next;
        }

        Check(start1 == start2, "long title: holds still at the start");
        Check(moving != start1, "long title: moves after the hold");
        Check(changes >= 3, "long title: moves smoothly (repainted many times a second)");
    }

    // A truck without a navigation screen whose cluster display was picked by hand
    // (display_pinned=1): the picture goes there although nothing shows the map.
    printf("truck without a navigation screen\n");
    WritePrivateProfileStringW(L"nav.mercedes.actros", L"display", L"512x512", iniPath.c_str());
    WritePrivateProfileStringW(L"nav.mercedes.actros", L"display_pinned", L"1", iniPath.c_str());
    WritePrivateProfileStringW(L"nav.mercedes.actros", L"u0", L"0.040", iniPath.c_str());

    const std::array<scs_named_value_t, 4> actrosAttrs{StringAttribute("brand", "Mercedes-Benz"),
                                                       StringAttribute("id", "mercedes.actros"),
                                                       StringAttribute("name", "Actros"),
                                                       {}};
    const scs_telemetry_configuration_t actrosConfig{"truck", actrosAttrs.data()};
    if (g_configurationCallback) {
        g_configurationCallback(SCS_TELEMETRY_EVENT_configuration, &actrosConfig, nullptr);
    }

    bool clusterHasPlayer = false;
    bool navUntouched = true;
    for (int i = 0; i < 20; ++i) {
        simulateFrame(noMapTruck);
        if (i >= 3) {
            clusterHasPlayer = clusterSeen != clusterFinal.srv.Get();
            navUntouched &= navSeen == navFinal.srv.Get();
        }
    }
    Check(clusterHasPlayer && navUntouched,
          "pinned cluster display gets the picture without any map");

    bool kept = true;
    for (int i = 0; i < 10;
         ++i) {  // a pause menu and back: the cluster now looks map-fed, then not again
        simulateFrame(i < 5 ? twoMapScreens : noMapTruck);
        kept &= clusterSeen != clusterFinal.srv.Get() && navSeen == navFinal.srv.Get();
    }
    Check(kept, "pinned cluster display keeps the picture through menus");

    // What the hook costs per PSSetShaderResources call (the game makes thousands per frame). The
    // baseline is a second device, whose context the plugin never hooks. Reported, not checked:
    // timings depend on the machine.
    printf("hook overhead\n");
    {
        ComPtr<ID3D11Device> plainDev;
        ComPtr<ID3D11DeviceContext> plainCtx;
        D3D11CreateDevice(nullptr, driverType, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &plainDev,
                          nullptr, &plainCtx);
        const TexSrv plainTex = MakeTexture(plainDev.Get(), 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            D3D11_BIND_SHADER_RESOURCE);
        const TexSrv sceneTex = MakeTexture(dev.Get(), 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM,
                                            D3D11_BIND_SHADER_RESOURCE);

        auto nsPerCall = [](ID3D11DeviceContext* c, ID3D11ShaderResourceView* view) {
            constexpr int kCalls = 200000;
            LARGE_INTEGER frequency;
            LARGE_INTEGER t0;
            LARGE_INTEGER t1;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < kCalls; ++i) { c->PSSetShaderResources(i & 7, 1, &view); }
            QueryPerformanceCounter(&t1);
            return (t1.QuadPart - t0.QuadPart) * 1e9 / frequency.QuadPart / kCalls;
        };

        SetRt(ctx.Get(), sceneRt);  // a scene pass, as most of the game's binds are
        const double unhooked = nsPerCall(plainCtx.Get(), plainTex.srv.Get());
        if (debugNav) { debugNav(0); }
        const double screenOff = nsPerCall(ctx.Get(), sceneTex.srv.Get());
        if (debugNav) { debugNav(1); }
        const double screenOn = nsPerCall(ctx.Get(), sceneTex.srv.Get());

        printf(
            "  info PSSetShaderResources: %.0f ns unhooked, %.0f ns hooked with the screen off, "
            "%.0f ns on\n",
            unhooked, screenOff, screenOn);
        Check(plainTex.srv && sceneTex.srv, "measured");
    }

    if (debugNav) { debugNav(0); }
    simulateFrame(normal);
    Check(navSeen == navFinal.srv.Get() && clusterSeen == clusterFinal.srv.Get(),
          "nav off: game's own textures back");

    printf("shutdown\n");
    shutdown();

    const std::string log = ReadFile(logPath);
    Check(log.find("[d3d] game swapchain: 1280x720") != std::string::npos,
          "log: game swapchain seen by Present hook");
    Check(log.find("[truck] Iveco S-Way (id iveco.sway): no screen settings yet") !=
              std::string::npos,
          "log: truck from the configuration event");
    Check(log.find("[nav] displays in this truck: 4,") != std::string::npos,
          "log: the new truck's displays described");
    Check(log.find("new candidate #0 1024x600") != std::string::npos,
          "log: candidate #0 described");
    Check(log.find("[cap] begin: recording 2 frame(s)") != std::string::npos,
          "capture: begins on Present");
    Check(log.find("RT #0 1024x600") != std::string::npos,
          "capture: render target switch to candidate");
    Check(log.find("VP x=0 y=0 1024x600") != std::string::npos,
          "capture: viewport inside candidate");
    Check(log.find("PS t2 <- #0") != std::string::npos,
          "capture: candidate bind recorded with slot");
    Check(log.find("[cap] end") != std::string::npos,
          "capture: record written after the last frame");

    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    DestroyWindow(hwnd);
    return g_failures ? 1 : 0;
}
