// ets_spotify: a telemetry SDK plugin for Euro Truck Simulator 2 that puts Spotify on the cab's
// navigation screen.
//
// The game loads every DLL from bin\win_x64\plugins and calls scs_telemetry_init(). This file is
// the composition root: it turns SDK events into game state and starts the modules.
//
//   d3d/hooks      hooks Direct3D 11 and hands the game's draw calls to the screen module
//   screen/        finds the navigation display texture and swaps it for our picture
//   ui/            paints that picture (Direct2D, on its own thread)
//   spotify/       Spotify Web API client: login, player state, commands
//   cabin/         auto-pause and speed-dependent volume
//   input/         hotkeys

#include <windows.h>

#include <array>

#include "cabin/cabin.hpp"
#include "core/log.hpp"
#include "core/version.hpp"
#include "d3d/hooks.hpp"
#include "game/game_state.hpp"
#include "game/scs_sdk.hpp"
#include "game/vehicle.hpp"
#include "input/input.hpp"
#include "screen/nav_screen.hpp"
#include "screen/screen_profile.hpp"
#include "spotify/spotify.hpp"
#include "ui/ui_renderer.hpp"

namespace {
bool g_started = false;

enum Channel : uintptr_t { kSpeed = 1, kIgnition };

// "truck" configuration: sent when a game is loaded and when the player gets into another truck.
void OnTruckConfiguration(const scs_telemetry_configuration_t& config) {
    vehicle::Info truck;
    if (!vehicle::ReadTruckConfig(config, &truck) || !vehicle::Set(truck)) { return; }

    const char* const screen = screen_profile::HasOwn()
                                   ? "own screen settings"
                                   : "no screen settings yet, using [nav] (hold F8 to fit)";
    logx::Log("[truck] {} (id {}): {}", vehicle::Describe(truck), truck.id, screen);

    unsigned width = 0;
    unsigned height = 0;
    bool pinned = false;
    screen_profile::PreferredDisplay(&width, &height, &pinned);
    nav::OnVehicleChanged(width, height, pinned);
    ui::ReloadSettings();  // this truck's window
}

void SCSAPIFUNC OnEvent(const scs_event_t event, const void* const info, const scs_context_t) {
    switch (event) {
        case SCS_TELEMETRY_EVENT_paused: game::SetPaused(true); break;
        case SCS_TELEMETRY_EVENT_started: game::SetPaused(false); break;
        case SCS_TELEMETRY_EVENT_configuration:
            if (info) {
                OnTruckConfiguration(*static_cast<const scs_telemetry_configuration_t*>(info));
            }
            break;
        default: break;
    }
}

void SCSAPIFUNC OnChannel(const scs_string_t, const scs_u32_t, const scs_value_t* const value,
                          const scs_context_t context) {
    if (!value) { return; }
    switch (reinterpret_cast<uintptr_t>(context)) {
        case kSpeed: game::SetSpeed(value->value_float); break;
        case kIgnition: game::SetIgnition(value->value_bool != 0); break;
        default: break;
    }
}

void Subscribe(const scs_telemetry_init_params_v100_t& params) {
    if (params.register_for_event) {
        for (scs_event_t event : {SCS_TELEMETRY_EVENT_paused, SCS_TELEMETRY_EVENT_started,
                                  SCS_TELEMETRY_EVENT_configuration}) {
            params.register_for_event(event, OnEvent, nullptr);
        }
    }

    if (!params.register_for_channel) { return; }
    struct ChannelInfo {
        const char* name;
        scs_value_type_t type;
        Channel id;
    };
    constexpr std::array<ChannelInfo, 2> kChannels{{
        {SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, kSpeed},
        {SCS_TELEMETRY_TRUCK_CHANNEL_electric_enabled, SCS_VALUE_TYPE_bool, kIgnition},
    }};

    for (const ChannelInfo& c : kChannels) {
        const scs_result_t result = params.register_for_channel(
            c.name, SCS_U32_NIL, c.type, SCS_TELEMETRY_CHANNEL_FLAG_none, OnChannel,
            reinterpret_cast<scs_context_t>(c.id));
        if (result != SCS_RESULT_ok) {
            logx::Log("[telemetry] channel {} not available ({})", c.name, result);
        }
    }
}

// The D3D hooks point into this DLL, so it must stay loaded for the life of the game.
void PinSelf() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&PinSelf), &self);
}

void StartModules() {
    PinSelf();
    if (hooks::Install()) {
        logx::Game(SCS_LOG_TYPE_message,
                   "D3D11 hooks installed. F8 = Spotify on the navigation screen");
    } else {
        logx::Game(SCS_LOG_TYPE_warning, "D3D11 hooks NOT installed, see ets_spotify.log");
    }

    ui::Start();
    spotify::Start();
    cabin::Start();
    input::Start();
}

// Reverse order: no new commands, no more D3D work, then the threads (cabin restores the volume).
void StopModules() {
    input::Stop();
    hooks::Disable();
    cabin::Stop();
    spotify::Stop();
    ui::Stop();
}

const char* OrEmpty(const char* s) { return s ? s : ""; }
}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(instance); }
    return TRUE;
}

extern "C" __declspec(dllexport) scs_result_t SCSAPIFUNC
scs_telemetry_init(const scs_u32_t version, const void* const rawParams) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) {
        return SCS_RESULT_unsupported;
    }

    if (!rawParams) { return SCS_RESULT_generic_error; }
    const auto& params = *static_cast<const scs_telemetry_init_params_v100_t*>(rawParams);

    logx::Init(params.common.log);
    logx::Game(SCS_LOG_TYPE_message,
               "ets_spotify {} loaded: game='{}' id='{}' game_api={}.{} sdk_api={}.{}",
               core::kVersion, OrEmpty(params.common.game_name), OrEmpty(params.common.game_id),
               scs_major_version(params.common.game_version),
               scs_minor_version(params.common.game_version), scs_major_version(version),
               scs_minor_version(version));

    Subscribe(params);

    if (!g_started) {
        StartModules();
        g_started = true;
    }

    return SCS_RESULT_ok;
}

extern "C" __declspec(dllexport) void SCSAPIFUNC scs_telemetry_shutdown() {
    logx::Log("shutdown");
    StopModules();
    g_started = false;
    logx::Shutdown();
}
