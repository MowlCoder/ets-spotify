#pragma once
#include <string_view>

// ETS_SPOTIFY_VERSION comes from project(... VERSION x.y.z) in CMakeLists.txt.
#ifndef ETS_SPOTIFY_VERSION
#define ETS_SPOTIFY_VERSION "dev"
#endif

namespace core {
inline constexpr std::string_view kVersion = ETS_SPOTIFY_VERSION;
inline constexpr std::wstring_view kUserAgent =
    L"ets-spotify/" ETS_SPOTIFY_VERSION;  // null-terminated
}  // namespace core
