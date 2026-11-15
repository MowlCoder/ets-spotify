#pragma once
#include <d3d11.h>

#include <string>

// Finds the texture of the in-cab screen.
// Every 2D colour render target the game binds as a pixel-shader input becomes a numbered
// "candidate". In ALL mode each candidate is replaced by a test pattern showing its number;
// in SINGLE mode only the selected one is. The player looks at the dashboard and reads the number.
namespace probe {
constexpr int kModeOff = 0;
constexpr int kModeAll = 1;
constexpr int kModeSingle = 2;

bool Active();
void CountCall();
// Returns a replacement view for `view` with a reference the caller must Release(),
// or nullptr to keep the game's own.
ID3D11ShaderResourceView* Substitute(ID3D11ShaderResourceView* view);

void SetBackbuffer(UINT width, UINT height);
void OnPresentTick();
// Drops all candidates and their patterns (device change, plugin shutdown).
void Reset(const char* reason);

// For the frame capture.
int Lookup(ID3D11ShaderResourceView* view);  // candidate index (classifies if needed) or -1
// Sets "#19 512x512" for candidates or "1920x1080 fmt=87" otherwise; returns the index or -1.
int DescribeResource(ID3D11Resource* resource, std::string* text);
bool IsInteresting(int idx);  // not a full-screen / scene-resolution buffer

// Hotkey actions.
void CycleMode();
void Step(int direction);
void Confirm();

// Test helpers.
void DebugSet(int mode, int selected);
int CandidateCount();
}  // namespace probe
