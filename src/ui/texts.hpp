#pragma once
#include <string>

#include "screen/calibration.hpp"
#include "spotify/spotify.hpp"

// Words on the cab screen. [ui] language=auto|ru|en in ets_spotify.ini (auto: the Windows display
// language). Another language is one more column in the table in texts.cpp.
namespace texts {
void Reload();  // re-reads [ui] language

// Title and second line shown instead of a track. `screenKey` is the key name for "Press F8".
void Status(const spotify::State& state, const std::wstring& screenKey, std::wstring* title,
            std::wstring* line);
std::wstring Notice(const spotify::State& state);  // empty if there is none

std::wstring CalibrationLabel(calibration::Item item);
std::wstring DisplayNotFound();
}  // namespace texts
