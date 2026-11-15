# How ets-spotify works

The mod is an SCS SDK telemetry plugin: a DLL that the game loads from `bin\win_x64\plugins` and initializes through `scs_telemetry_init`. The official SDK only provides telemetry (speed, pause, truck), so the picture gets onto the cab screen around the SDK: the plugin hooks Direct3D 11 and replaces the texture of the built-in navigation screen with its own.

## Overview

```mermaid
flowchart LR
    subgraph game[eurotrucks2.exe process]
        SDK[SCS SDK<br/>events and channels] --> plugin[plugin.cpp<br/>entry point]
        plugin --> state[game/game_state<br/>pause, speed, ignition]
        plugin --> vehicle[game/vehicle<br/>current truck]
        D3D[Direct3D 11<br/>game frame] <--> hooks[d3d/hooks]
        hooks --> nav[screen/nav_screen<br/>find the screen, swap the texture]
        nav --> graph[screen/display_graph<br/>the choice algorithm]
        ui[ui/ painting thread<br/>Direct2D on the CPU] --> nav
        spotify[spotify/<br/>Web API client] --> ui
        cabin[cabin/<br/>auto-pause, volume] --> spotify
        state --> cabin
        input[input/<br/>hotkeys] --> spotify
        input --> nav
    end
    spotify <--> api[(api.spotify.com)]
    cabin --> mixer[Windows mixer<br/>Spotify.exe session]
    app[Spotify app<br/>on the PC or phone] <--> api
```

The mod does not carry audio. Music plays in the regular Spotify app; the mod reads the player state and sends commands through the Web API. So the phone, the PC app and the mod are three remotes for one Spotify Connect session, and they stay in sync by design.

## Folders

| Folder | Contents |
|---|---|
| `src/plugin.cpp` | the entry point: the game calls `scs_telemetry_init` here; it subscribes to the SDK and wires all modules together |
| `src/core` | everything that knows nothing about the game: log, ini, JSON, HTTPS client and the OAuth redirect listener, PKCE, UTF-8, background threads (`Worker`), never-destroyed globals (`Immortal`) |
| `src/game` | the SDK ABI, game state, the current truck |
| `src/d3d` | Direct3D 11 hooks by vtable patching |
| `src/screen` | finding the navigation screen, swapping its texture, the visible window, per-truck screen settings, the fitting mode |
| `src/ui` | painting the picture (Direct2D + DirectWrite into a WIC bitmap), texts in English and Russian, the scrolling of long lines |
| `src/spotify` | OAuth PKCE login, token storage (DPAPI), Web API requests, parsing the answers, the worker thread |
| `src/cabin` | auto-pause and speed-dependent volume |
| `src/input` | hotkeys and their names in the ini |
| `src/diagnostics` | the tools used to find the screen: test patterns on every texture and a frame capture |
| `tests/unit` | unit tests of the pure logic |
| `tests/harness.cpp` | the offline harness: plays the game with its own D3D11 device |

Dependencies point downwards: `core` depends on nothing, `plugin.cpp` knows everybody. The pure logic (choosing the screen, parsing Spotify answers, cabin rules, key names, the SDK truck configuration, the picture layout, scrolling) lives in headers without Windows dependencies and is covered by unit tests.

## Threads

| Thread | What it does | How it shares data |
|---|---|---|
| game thread | SDK callbacks: paused, started, speed and ignition channels, truck configuration | writes the `game_state` atomics, `vehicle::Set` |
| game render thread | our D3D hooks: watch the frame, swap the texture; on `Present` decide which display is the navigation screen and upload the picture | `FrameGraph` under a mutex, atomics on the fast path |
| `ui` | paints the picture when something changes, every second while a track plays, 20 times a second while a long line scrolls; nothing on a timer while the screen is off | publishes an immutable `shared_ptr<const Frame>` |
| `spotify` | login, polling `/me/player` once a second while the screen is on, commands | publishes an immutable `shared_ptr<const State>`, a command queue |
| `cabin` | every 250 ms: auto-pause and volume | reads `game_state`, sends commands to Spotify, drives the mixer |
| `input` | polls the keys every 25 ms while the game window is in front | calls the modules |

The main rule: the render thread never waits for the network or the disk. Everything slow (HTTP, decoding the cover, Direct2D) runs on its own thread; the render thread only takes finished data.

## Finding the navigation screen

The game renders every cab display into its own texture and maps it onto the screen model. A frame capture (the Home key in diagnostic mode) showed this chain in an Iveco with a dash-top GPS:

```
map render (1024x1024, no mips, redrawn every frame)
  → display 256x256 → display 256x256      navigation
cluster UI → display 512x512 → display 512x512      instrument cluster
scene pass: the screen models sample the last display of each chain
```

A "display" is a render target with `GENERATE_MIPS`, at most 1024 px. The algorithm (`src/screen/display_graph.hpp`) builds a graph of "which display sampled what" every frame and decides:

1. A display pass that samples a plain render target **redrawn in the same frame** shows the map.
2. "Shows the map" flows along display → display sampling.
3. Candidates are map displays that no other display samples (the end of a chain).
4. The choice is sticky: the current texture stays as long as it is a candidate. Otherwise the size saved for this truck wins, otherwise the smallest one (navigation screens are small).

Trucks without a navigation screen still have displays, e.g. in the instrument cluster. In the fitting mode the player can pick any display at the end of a chain (map displays first). That choice is pinned (`display_pinned=1` in the truck's section): from then on the display of that size is used whether it shows the map or not, so a pause menu or a crash does not move the picture away.

Why every frame, and why "redrawn in the same frame". The first version looked for the screen every few seconds and took the largest candidate. After leaving the pause menu, the player jumped onto the speedometer: the game mixes another texture into the displays (it looks like a font cache), and the speedometer started to look "connected to the map" too. The map is redrawn every frame, the font cache is not, hence rule 1. And display textures come from a pool and can change owners, so the decision is made again every frame: as soon as something without the map is drawn into our texture, the substitution stops. Both cases are reproduced in the tests.

## Swapping the picture

In the scene pass the game binds the navigation texture to the pixel shader with `PSSetShaderResources`. In short windows (3 frames every 2 s) the plugin learns which shader resource views show which display texture. In the `PSSetShaderResources` hook, the navigation texture's view is then replaced with the view of our texture. The substitution confirms the resource, because the address of a view may be reused by another texture.

Details found from screenshots:
- The game samples display textures upside down, so the picture's rows are flipped.
- The screen model shows only a window of the texture (on the Iveco about u 0.04–0.98, v 0.25–0.68). The window is stored in the ini per truck and fitted in game.
- Some screens stretch the texture: on the Mercedes Actros a texture pixel looks about 1.2 times wider than tall. So the window has an `aspect` parameter, and the player is drawn in "screen units" compressed horizontally by `aspect`, which keeps a square cover square.
- The player layout was made for the Iveco screen (aspect ratio about 2.2). On a taller screen it does not grow; it takes a box of the same shape, centred vertically (`Layout()` in `screen/screen_window.hpp`). Otherwise the cover eats the space for the title.
- Lines that do not fit even at the minimum font size scroll back and forth (`ui/marquee.hpp`) instead of being cut with an ellipsis.
- The picture is a square 512x512 texture, `B8G8R8A8_UNORM_SRGB`, with a full mip chain (without mips it shimmers at a distance).

## Hooking Direct3D

COM methods are called through a table of virtual functions, and that table is shared by every object of the same class. The plugin creates its own device and swapchain on a hidden window and replaces `Present`, `Present1` and `CreateDeferredContext` in their vtables. That hooks the game's objects as well.

Device contexts did not work that way: every context has its own vtable on the heap. So the game's immediate context is hooked on the first `Present`, and deferred contexts when they are created. `Draw*` and `GenerateMips` are not hooked: the runtime rewrites those slots itself and a hook there gets lost (visible in the harness).

## Spotify

- **Login**: OAuth 2.0 Authorization Code with PKCE, no client secret. The browser goes to `accounts.spotify.com`, which redirects to `http://127.0.0.1:8888/callback`, where a tiny local server takes the answer. The server starts listening before the browser opens, otherwise a fast redirect hits a closed port.
- **Token**: the refresh token is encrypted with DPAPI (only this Windows user can decrypt it) and stored in `%APPDATA%\ets-spotify\token.dat` together with the client_id it was issued for.
- **Polling**: `GET /me/player` once a second, only while the screen is on. The Web API offers no push notifications to third-party apps. On 401 the token is refreshed once; on 429 the plugin waits for `Retry-After`.
- **Commands**: play/pause, next, previous. If there is no active device, playback is moved to the PC app.
- **Spotify development mode** (since February 2026): the app owner needs Premium and one app has at most 5 users. A shared Client ID for all players is therefore impossible: every player registers their own app, which takes about five minutes.

The Spotify module reports what happens as codes (`Status`, `Problem`, `Notice`), and `ui/texts.cpp` picks the words for the screen. That keeps the client independent of the interface language.

## Cabin

- **Auto-pause** (`cabin/cabin.cpp`): music pauses 0.5 s after the game pauses or the ignition goes off, and resumes when you drive again. It only resumes a pause it made itself and does nothing in the start-up menu. The rules are a pure function, `WantAutoPause` in `cabin_logic.hpp`.
- **Speed-dependent volume**: a factor from 0.70 when standing to 1.0 at 90 km/h is applied to the Spotify.exe session in the Windows mixer (WASAPI `ISimpleAudioVolume`). It is instant and needs no Web API requests.
- **Whose volume**: the player can change the volume in Spotify, on the phone or in the mixer at any time. `VolumeTracker` compares the mixer with its own value every tick and takes a foreign change as the player's new volume. Spotify Connect, however, syncs the volume through its servers and sometimes sends back, late, a value the mod set a second ago. Such an "echo" is recognized by the list of recent values and is not taken as the player's choice. The player's volume is restored on exit.

## Performance

The `PSSetShaderResources` hook is the hottest spot: the game calls it thousands of times per frame. Measured in the harness (200,000 calls on the developer's machine; numbers vary between runs):

| | ns per call |
|---|---|
| not hooked | 11–15 |
| hooked, screen off | 15–21 |
| hooked, screen on | 18–27 |

Everything else:
- While the screen is on, the `OMSetRenderTargets` hook looks up the texture description on every render target switch: hundreds of calls per frame, no allocations.
- The picture is painted on the CPU on its own thread: once a second while a track plays, 20 times a second while a long line scrolls, and on changes. Uploading the finished picture on `Present` is one `UpdateSubresource` per mip.
- The ini file is read again only on a "settings changed" signal (screen on, truck change, saved fitting) and every 5 s by the cabin.

## Tests

- `tests/unit`: JSON, PKCE (the RFC 7636 example), URL helpers, the OAuth listener, choosing the navigation screen (including the "after the menu" and "texture pool" cases), the screen window and the player layout, parsing Spotify answers, cabin rules, `VolumeTracker`, the SDK truck configuration, key names, scrolling.
- `tests/harness.cpp`: the harness creates its own D3D11 device like the game does, loads the DLL through `scs_telemetry_init` and plays frames with display chains like in ETS2. It checks the substitution, the "after the menu" and "texture pool" cases, a truck change, the fitting mode (the picture, switching the display, saving to the ini), a long title that scrolls, the frame capture, and that the hooks are not lost. At the end it prints the hook timing.

```powershell
cmake --workflow --preset default      # or: ctest --preset release -V, to see the harness output
```

## Extending

- **A new cabin feature**: a class with `Tick(now, settings)` in `cabin/cabin.cpp`; the rules as pure functions in `cabin_logic.hpp`, with a test.
- **A new hotkey action**: a field in `Bindings` and a handler in `input/input.cpp`, a key in the `[keys]` section.
- **A new language**: one more `Strings` column in `ui/texts.cpp`.
- **A new screen look**: a `Painter` method. Draw inside the layout box from `Layout()` (in screen units).
- **Another source of truck data**: a channel or event in `Subscribe()` in `plugin.cpp` and a field in `game_state`.
- **DX12 and Vulkan** are not supported: the game has to run on DX11, which is its default.

## Code rules

- Globals with destructors are created with `core::Immortal<T>()` (`core/immortal.hpp`): static destructors run at DLL unload under the loader lock, when the game may already have torn down D3D.
- Every module with its own thread uses `core::Worker` (`core/worker.hpp`, a `std::thread` with a Win32 stop event). The DLL is pinned in memory (`GET_MODULE_HANDLE_EX_FLAG_PIN`), so a thread stuck in a network call can safely finish later.
- No network, disk or long locks on the render thread.
- Log: `logx::Log("...{}...", args)` with `std::format` strings checked at compile time, written to `Documents\Euro Truck Simulator 2\ets_spotify.log`; every module has its prefix (`[nav]`, `[spotify]`, `[cabin]` …); repeated messages are rate-limited.
- Style: `.clang-format` (Google, 4 spaces, 100 columns, braces on every block), checked in CI.

## C++ Best Practices

The code follows [cpp-best-practices](https://github.com/cpp-best-practices/cppbestpractices):

- **Tools:** CMake with presets, CTest, CI on every push; `/permissive- /W4 /WX` plus the guide's extra MSVC warnings (`/w14242 … /w14928`); the MSVC static analyzer (`cmake --workflow --preset analyze`) and AddressSanitizer (`cmake --workflow --preset asan`) run clean and run in CI; `target_compile_features(cxx_std_20)`.
- **Style:** `.hpp`/`.cpp`, `#pragma once`, everything in namespaces, braces on every block, `nullptr`, `//` comments, no `using namespace` in headers, `explicit` single-argument constructors, rule of zero.
- **Safety:** no C-style casts, no C arrays (`std::array`, `std::string_view`), no raw `new`/`delete` outside `core::Immortal`, no C-style variadic functions (`std::format` and variadic templates instead), `std::filesystem` for paths and files, `std::thread` for threads.
- **Maintainability:** no macros except where C++ has no alternative (below); no functions taking several booleans (`cabin::Situation`).

Deliberate deviations, and why:

- **Naming** follows the Google C++ style consistently (`PascalCase` functions, `kConstants`, `trailing_` members) instead of the guide's `camelCase` / `m_` example; the guide asks first of all for one consistent, widely read style.
- **Global state.** The SDK calls plain C functions and the D3D hooks replace vtable slots with fixed signatures and no user context, so each module keeps its state in an anonymous namespace (atomics, a mutex, immortal objects) behind a small function interface.
- **No exceptions across the game's callbacks.** Code called by the game (SDK callbacks, D3D hooks) must never throw into it, so errors are status codes and enums (`spotify::Problem`), and the few library calls that can throw are caught locally (`core::Worker::Start`).
- **`float`** where the APIs take `float` (Direct2D, the Windows mixer); everything else is integer or `double`.
- **Macros left:** `SCSAPIFUNC` (the SDK's calling convention), `ETS_SPOTIFY_VERSION` (the version from CMake, wrapped in `core::kVersion`) and `TEST_CASE` (static test registration).
- **The SDK ABI** (`game/scs_sdk.hpp`) keeps the official C names in the global namespace, so it reads like the SDK documentation.
- **Win32 where the standard library has nothing:** events for waiting on several things at once, the C log stream (shared reading while the game runs), Direct3D/Direct2D/WASAPI/WinHTTP.
- **Own test framework** (`tests/unit/check.hpp`, 40 lines) instead of Catch2 or GoogleTest, to keep the project free of dependencies; tests are still registered with CTest.

## Build

- `CMakeLists.txt` with presets (`CMakePresets.json`): Visual Studio 2022 generator, x64, static CRT (the DLL needs no VC++ redistributable), C++20, no third-party dependencies.
- Presets: `default` (Release build and tests), `analyze` (MSVC static analyzer), `asan` (AddressSanitizer, Debug build and tests). Each runs as a workflow: `cmake --workflow --preset <name>`.
- Step by step: `cmake --preset default`, `cmake --build --preset release` (or `debug`), `ctest --preset release` (`-V` shows the hook timing from the harness).
- IDEs: Visual Studio and CLion pick up `CMakePresets.json` by themselves; VS Code does it through the CMake Tools extension (recommended in `.vscode/extensions.json`), and `.vscode/c_cpp_properties.json` gives IntelliSense the same compiler and standard.
- Targets: `ets_core` (static library of game-independent code), `ets_spotify` (the plugin), `unit_tests` and `probe_harness` (registered with CTest; `ETS_SPOTIFY_TESTS=OFF` skips them).
- `cmake --install` copies the DLL into the game folder, which `cmake/ets2.cmake` finds in the Steam libraries (registry + `libraryfolders.vdf`); `-DETS2_DIR=<game folder>` at configure time or `--prefix <game folder>` at install time overrides it. It refuses a folder without the game and a running game.
- The version is `project(... VERSION)` and reaches the code as `ETS_SPOTIFY_VERSION`. CI (`.github/workflows/build.yml`) checks the style with clang-format 19, builds, tests and runs `analyze` and `asan` on every push.
- Releases: bump the version in `CMakeLists.txt`, then push a matching tag (`git tag v0.1.0 && git push origin v0.1.0`); CI builds, tests and publishes a GitHub release with the DLL attached.
