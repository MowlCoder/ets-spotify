#pragma once
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

// File log at Documents\Euro Truck Simulator 2\ets_spotify.log (the ETS_SPOTIFY_LOG environment
// variable overrides the path; the tests use it). Lines carry a time stamp and the thread id.
//
//     logx::Log("[nav] nav display: {}x{}", width, height);
//
// Format strings are std::format strings, checked at compile time.
namespace logx {

// The game's own log function (scs_log_t of the SCS SDK has the same signature).
using GameLogFn = void(__stdcall*)(int type, const char* message);

void Init(GameLogFn gameLog);
void Shutdown();
std::filesystem::path Dir();  // folder of the log file

// File only; safe from any thread.
void WriteLine(std::string_view message);
// File and the game's log/console. Only from the game thread (SDK init, shutdown and callbacks).
void GameLine(int type, std::string_view message);

template <typename... Args>
void Log(std::format_string<Args...> format, Args&&... args) {
    WriteLine(std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void Game(int type, std::format_string<Args...> format, Args&&... args) {
    GameLine(type, std::format(format, std::forward<Args>(args)...));
}

}  // namespace logx
