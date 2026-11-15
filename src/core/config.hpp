#pragma once
#include <filesystem>
#include <string>

// Settings file: Documents\Euro Truck Simulator 2\ets_spotify.ini (next to ets_spotify.log).
// A missing key is written back with its default, so the file documents every setting.
namespace config {
std::filesystem::path Path();
std::wstring GetString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);
int GetInt(const wchar_t* section, const wchar_t* key, int fallback);
float GetFloat(const wchar_t* section, const wchar_t* key, float fallback);

// Reads a key without writing a default: false when it is missing.
bool Find(const wchar_t* section, const wchar_t* key, std::wstring* value);
void Set(const wchar_t* section, const wchar_t* key, const std::wstring& value);
}  // namespace config
