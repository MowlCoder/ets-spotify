# ETS2 × Spotify: the original analysis

> This analysis was written before the work started: which paths exist, what the SDK and Spotify allow, similar mods. The implementation took a different route in the end: instead of an ImGui window over the game, the picture goes straight onto the truck's built-in screen. How it actually works is described in [architecture.md](architecture.md).

Goal: a key in the Euro Truck Simulator 2 cab opens a Spotify-style interface where you can log into your Spotify account and play music.

Date of the analysis: 14 November 2026.

## 1. Starting point

- Euro Truck Simulator 2 1.62 (Steam), DirectX 11 renderer.
- The game looks for `scs_telemetry_init` and `scs_input_init` in the DLLs in `bin\win_x64\plugins`.
- Game input goes through DirectInput 8.
- Tools: Visual Studio 2022 with C++, CMake, the Spotify desktop app.

## 2. What ETS2 modding allows

1. **.scs mods** (the `mod` folder): models, textures, def files, UI layouts. They contain no scripts, so an interactive Spotify interface cannot be built with .scs alone.
2. **SDK plugin**: a DLL in `bin\win_x64\plugins` that the game loads at start-up. The official SDK only provides telemetry (speed, pause, engine and so on) and custom input devices. But the DLL runs inside the game process, so it can hook frame presentation (DXGI `Present`) and draw its own interface. WestPlay, ts-fmod-plugin and similar mods work this way.
3. **The game's radio**: internet radio from `live_streams.sii` (mp3/aac streams by URL) and mp3 files from `music\`. The sound comes from the cab radio.

Conclusion: the mod = an SDK plugin (DLL) + an overlay through DirectX hooks.

## 3. What Spotify allows (as of November 2026)

**Web API.** It can control playback (play, pause, next, seek, volume, queue, device switching), read playlists, search, read "Liked Songs". It needs your own Client ID from developer.spotify.com.

Development Mode limits in force since February 2026:
- The app owner must have Premium. Controlling the player requires Premium too.
- At most 5 users per app. So the mod cannot ship with a shared Client ID: every player registers their own app, which takes about 5 minutes.
- Search returns at most 10 results per request; page with `offset`.
- Playlist items (`/playlists/{id}/items`) are only returned for your own and collaborative playlists. Someone else's playlist can be started by URI, but its track list cannot be read.
- All `/me/player/*` endpoints are still available.

The Web API carries no audio. It only sends commands to a Spotify Connect device: the desktop app, a phone or a speaker.

**Web Playback SDK** (a player in the browser) only works in regular browsers. In embedded CEF and Electron it breaks on Widevine DRM, so it is unreliable.

**librespot**: an unofficial open-source Spotify Connect client. It gives access to the audio itself but goes against Spotify's terms.

## 4. Options

| | Where the sound comes from | What it looks like | Difficulty | Risks |
|---|---|---|---|---|
| **A. Overlay + Web API** (recommended) | Spotify desktop in the background | a Dear ImGui panel over the game on F8 | medium | within Spotify's rules |
| B. A + sound through the game radio | librespot → a local mp3 stream → `live_streams.sii`, sound from the cab radio | same as A | high | 5–15 s delay on track changes because of buffering; breaks Spotify's terms |
| C. Embedded browser (like WestPlay) | open.spotify.com in WebView2 | the real Spotify interface on the cab screen | very high | DRM in the embedded browser, heavy load |

### Similar mods (search of 14 November 2026)

With Spotify:
- **[WestPlay](https://github.com/enes-less/WestPlay)**: free, open source (GPL-3), ETS2 1.61/1.62 and ATS 1.62. A CarPlay-style interface on the cab screen: it replaces the built-in navigation screen, and trucks without one get a tablet. Inside is its own browser (WebView2) with Spotify, YouTube Music, Apple Music and a map. Mouse control while a key is held. Written in Rust with SCS SDK 1.15. The closest to the task, but built as "a phone in the cab" rather than a window over the screen.
- **[OmixPlay UI](https://allmods.net/euro-truck-simulator-2/omixplay-ui-carplay-style-interface-youtube-spotify-for-euro-truck-simulator-2-v1-0/)**: also a CarPlay-style interface with Spotify and YouTube. Only a mod-site card without technical details; not checked.
- **[Massy Spotify App](https://massy007.gumroad.com/l/MassySpotifyapp)**: a paid (£4) transparent overlay for ETS2/ATS and OBS that needs your own Client ID. A separate program rather than a mod, aimed mainly at streamers.

Not Spotify, but useful as examples:
- **[Local Radio](https://github.com/Koenvh1/ets2-local-radio)** (Koenvh1, MPL-2.0): real radio stations of the country the truck is in. A telemetry plugin + a local web page + an overlay. Last updated in November 2024.
- **[ts-fmod-plugin](https://git.tdem.in/dariowouters/ts-fmod-plugin)**: a DLL plugin that adds its own FMOD instance to the game for sound mods. An example of a plugin working with sound.
- **The game's own radio** (the R key): internet streams from `live_streams.sii` and mp3 from `music\`.

The search found no ready mod of exactly this kind (own window over the game on a key + Spotify control through the Web API). For just listening to music, WestPlay is worth trying first. For our own mod, option A fits.

### How to stand out

Control from the phone is not a differentiator: Spotify Connect provides it anyway.
- WestPlay opens the Spotify web player in its browser (README: login "on their own pages"). The web player is a Connect device itself, so it can most likely be controlled from the phone too, although the README does not say so.
- Massy works with a Client ID, i.e. through the Web API, and sees track changes from the phone the same way our mod does. The product page did not load when checked; details unknown.

The READMEs of the alternatives do not mention tying the music to the game. That is where a "Spotify as the truck's built-in radio" mod fits:
- **Steering wheel buttons**: next, previous, pause, volume from the wheel. The plugin could read the wheel through DirectInput.
- **Speed-dependent volume**, like noise compensation in real cars: the speed from telemetry changes the volume of the Spotify.exe session through the Windows Audio Session API. Instant, with no Web API requests.
- **Auto-pause**: when the game is paused, while sleeping at a rest stop, on loading screens, with the ignition off. Resume when driving again.
- **Playlist by country**: cross a border and, say, that country's Top 50 starts. Someone else's playlist can be started by URI. The mapping from coordinates to countries could come from Local Radio (MPL-2.0; files under that license can be included with attribution).
- **No browser and no DRM**: a native interface works in exclusive full-screen mode and on the cab screen.

Caveat: the alternatives were checked only by their READMEs and pages; the mods themselves were not run.

## 5. Architecture of option A (as planned)

```
eurotrucks2.exe
 └─ bin\win_x64\plugins\ets_spotify.dll
     ├─ scs_telemetry_init            ← entry point; pause/start events
     ├─ hook IDXGISwapChain::Present / ResizeBuffers (MinHook)
     │    └─ Dear ImGui (DX11 + Win32 backends) → the "Spotify" window
     ├─ hook WndProc                   ← F8, mouse, text input for ImGui
     ├─ hook IDirectInputDevice8::GetDeviceState / GetDeviceData
     │    └─ while the window is open the game gets an "empty" keyboard and mouse
     └─ background network thread: WinHTTP → api.spotify.com / accounts.spotify.com
          ├─ OAuth Authorization Code + PKCE, redirect http://127.0.0.1:<port>/callback
          ├─ refresh token → %APPDATA%\ets-spotify\ (DPAPI encryption)
          └─ covers: JPEG → WIC → ID3D11Texture2D
Spotify.exe (in the background) ← the Spotify Connect device that plays the sound
```

Planned screens:
- **Now playing**: cover, track, artist, progress, ⏮ ⏯ ⏭, volume, shuffle and repeat;
- **My playlists** and **Liked Songs**;
- **Search**;
- **Queue**;
- **Device picker**: if Spotify is not active, `PUT /me/player` to the desktop client.

Telemetry could add pausing music while the game is paused and ducking on loading screens, both switchable in the settings.

## 5b. Output on the cab screen

The game already renders the cab screens as textures. `eurotrucks2.exe` contains:
- the material `/material/ui/dashboard/bare_map.mat` (the map on the dashboard screen);
- the texture type `display_texture_t`;
- the settings `o_im_dashboard_map` and `o_im_infotainment`.

Every frame the game renders the screen picture into a texture and maps it onto the model. The plugin can replace that picture with its own:
1. Render the same ImGui interface into its own texture (a render target) instead of over the frame.
2. When the game binds the screen texture to the model (`PSSetShaderResources`), substitute our texture. Alternatively, create the screen texture with the render target flag and draw straight into it.

### Where to show it

| | Built-in navigation screen | Own tablet |
|---|---|---|
| What to do | find the screen texture in the renderer and swap it | a new cab accessory: a model (Blender + SCS Blender Tools) with a marker texture the plugin recognizes when loading |
| Pros | nothing to model; works in every truck with a built-in screen | the map stays; fits any truck |
| Cons | no map while Spotify is shown (toggled with a key); after a game update the texture may have to be found again | a 3D model and fitting into every truck's cab |

### Control

The screen is in the 3D scene, so the mouse cannot reach it directly. A scheme like WestPlay's:
- while a key is held, the camera stays still, the mouse moves a cursor on the cab screen and clicks go to the interface;
- release the key and you look around again.

Typing on a small screen is awkward. A proposed hybrid:
- **cab screen**: "Now playing", playlists, big CarPlay-style buttons;
- **F8**: a big window over the game for searching.

The interface is the same; only the place it is drawn changes.

### What it shares with the overlay

About 70% of the work is shared: DirectX hooks, ImGui, the Spotify client, input blocking. The cab screen adds:
- finding the screen texture (once, with RenderDoc or by logging textures from the plugin);
- rendering into a separate texture;
- a cursor on the screen;
- a big design for the low screen resolution.

WestPlay (GPL-3) solves the same problem, and its code can be read as a reference. It must not be copied, otherwise our mod would have to be GPL as well.

## 5c. Syncing with the phone

With option A the sync comes for free through Spotify Connect:
- The desktop app plays the sound and shows up on the phone as a Connect device ("Playing on PC").
- The phone, the mod and the desktop app are three remotes for the same playback session in Spotify's cloud.
- Skip a track on the phone and the sound on the PC changes at once (the official client does that). The mod sees the change on its next `/me/player` poll.

Details:
- **Interface delay.** The Web API offers no push notifications to third-party apps (the official clients use a private websocket). So the mod polls: every 1–2 s while the screen or window is visible, every 5 s in the background. A track change from the phone appears in the mod within 1–2 s.
- **No shared network needed.** The phone and the PC have to use the same Spotify account; the Wi-Fi can be any: devices of your account see each other through the internet.
- **If the sound moved to the phone** ("play on this phone"), the mod can show it and offer "Back to PC" (`PUT /me/player`).
- **Sleeping client.** If the desktop app is not running, the plugin could start it minimized (`%APPDATA%\Spotify\Spotify.exe --minimized`).
- **Search on the phone.** Solves typing on the small cab screen: search on the phone, see what plays in the cab and control it with keys.

## 6. Pitfalls

- **The first login** opens the browser, and a game in exclusive full-screen mode minimizes. It is needed once; after that the refresh token works. Borderless mode is an alternative.
- **Start-up warning.** With an SDK plugin the game shows a window about loaded SDK plugins at start-up; it has to be confirmed.
- **DirectInput.** Without hooking DI8, typing in the search box would steer and honk the truck.
- **Cursor.** In full-screen mode the system cursor is hidden, so ImGui draws its own (`io.MouseDrawCursor`).
- **dx12 and vulkan.** Another renderer needs another ImGui backend. The MVP supports dx11 only, the current setting.
- **Steam overlay** hooks `Present` too. They usually get along, but it has to be checked.
- **TruckersMP.** Compatibility with hooks is unknown; test in single player first.
- **Game updates.** The `Present` hook and the SDK ABI are stable between versions.
- **Network on a background thread only.** Calling the Web API from the render thread would cause stutters.
- **Web API limits.** Poll `/me/player` every 1 s with the window open and every 5 s with it closed.

## 7. Work plan (as planned)

1. DLL skeleton: CMake, MSVC, SCS SDK headers, a log file. Check that the game loads the plugin.
2. `Present` hook + ImGui: a window on F8, game input blocked.
3. Spotify OAuth PKCE and token storage. The "Now playing" screen and control buttons.
4. Playlists, liked songs, search, queue, devices, covers.
5. The cab screen: find the built-in screen texture, render the interface into it, a cursor while a key is held (section 5b).
6. Spotify style: dark theme, green accent, own font, big layout for the cab screen. Settings: key, window position, 4K scaling.
7. (Optional) own tablet as a cab accessory.
8. (Optional) option B: sound through the game radio.

What the player needs:
- a Spotify Premium account;
- an app on developer.spotify.com with the redirect URI `http://127.0.0.1:8888/callback`;
- its Client ID.

## Sources

- [SCS Telemetry SDK](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry)
- [Spotify: February 2026 migration guide](https://developer.spotify.com/documentation/web-api/tutorials/february-2026-migration-guide)
- [Spotify: changelog February 2026](https://developer.spotify.com/documentation/web-api/references/changes/february-2026)
- [Spotify: Update on Developer Access](https://developer.spotify.com/blog/2026-02-06-update-on-developer-access-and-platform-security)
- [TechCrunch on the Development Mode limits](https://techcrunch.com/2026/02/06/spotify-changes-developer-mode-api-to-require-premium-accounts-limits-test-users/)
- [Spotify Web Playback SDK](https://developer.spotify.com/documentation/web-playback-sdk)
- [WestPlay](https://github.com/enes-less/WestPlay)
- [Massy Spotify App](https://massy007.gumroad.com/l/MassySpotifyapp)
