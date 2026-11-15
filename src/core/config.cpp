#include "core/config.hpp"

#include <windows.h>

#include <array>
#include <cstdlib>
#include <format>

#include "core/log.hpp"

namespace config {
namespace {
constexpr const wchar_t* kMissing = L"\x01";  // a value no ini file contains

// The raw value of a key, or kMissing.
std::wstring Read(const wchar_t* section, const wchar_t* key) {
    std::array<wchar_t, 512> buf{};
    GetPrivateProfileStringW(section, key, kMissing, buf.data(), static_cast<DWORD>(buf.size()),
                             Path().c_str());
    return buf.data();
}
}  // namespace

std::filesystem::path Path() { return logx::Dir() / L"ets_spotify.ini"; }

std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    std::wstring value = Read(section, key);
    if (value == kMissing) {
        WritePrivateProfileStringW(section, key, fallback, Path().c_str());
        return fallback;
    }
    return value;
}

int GetInt(const wchar_t* section, const wchar_t* key, int fallback) {
    const std::wstring value = GetString(section, key, std::format(L"{}", fallback).c_str());
    return value.empty() ? fallback : _wtoi(value.c_str());
}

float GetFloat(const wchar_t* section, const wchar_t* key, float fallback) {
    const std::wstring value = GetString(section, key, std::format(L"{:.3f}", fallback).c_str());
    return value.empty() ? fallback : static_cast<float>(_wtof(value.c_str()));
}

bool Find(const wchar_t* section, const wchar_t* key, std::wstring* value) {
    std::wstring found = Read(section, key);
    if (found == kMissing) { return false; }
    *value = std::move(found);
    return true;
}

void Set(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), Path().c_str());
}

}  // namespace config
