#include "core/log.hpp"

#include <windows.h>
#include <share.h>
#include <shlobj.h>

#include <array>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/immortal.hpp"

namespace logx {
namespace {
std::mutex& g_mutex = core::Immortal<std::mutex>();  // guards everything below
// A C stream rather than std::ofstream: it can be opened shared for reading (_SH_DENYWR) while we
// write, so the log can be watched during a game.
FILE* g_file = nullptr;
GameLogFn g_gameLog = nullptr;
std::filesystem::path& g_path = core::Immortal<std::filesystem::path>();

std::filesystem::path ResolvePath() {
    std::array<wchar_t, MAX_PATH> buf{};
    const DWORD n =
        GetEnvironmentVariableW(L"ETS_SPOTIFY_LOG", buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) { return std::filesystem::path(buf.data()); }

    std::filesystem::path path;
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        path = std::filesystem::path(docs) / L"Euro Truck Simulator 2" / L"ets_spotify.log";
        CoTaskMemFree(docs);
    }
    return path;
}
}  // namespace

void Init(GameLogFn gameLog) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_gameLog = gameLog;
    if (g_file) { return; }
    g_path = ResolvePath();
    if (!g_path.empty()) { g_file = _wfsopen(g_path.c_str(), L"w", _SH_DENYWR); }
}

void Shutdown() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_gameLog = nullptr;
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

std::filesystem::path Dir() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_path.parent_path();
}

void WriteLine(std::string_view message) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{:5}] {}\n", t.wHour, t.wMinute,
                                         t.wSecond, t.wMilliseconds, GetCurrentThreadId(), message);

    const std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) { return; }
    fputs(line.c_str(), g_file);
    fflush(g_file);
}

void GameLine(int type, std::string_view message) {
    WriteLine(message);
    GameLogFn gameLog = nullptr;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        gameLog = g_gameLog;
    }
    if (gameLog) { gameLog(type, std::format("[ets_spotify] {}", message).c_str()); }
}
}  // namespace logx
