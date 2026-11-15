#pragma once
#include <d3d11.h>

// Puts our picture on the cab's navigation screen.
//
// Every frame the hooks report render-target switches and texture binds; the display passes are
// recorded into a FrameGraph and screen/display_graph.hpp decides which display texture is the
// navigation screen. In the scene pass the game binds that texture through a few shader resource
// views; those views are learned in short windows and swapped for the view of our UI texture
// (painted by ui/, uploaded here on Present).
namespace nav {
void SetEnabled(bool enabled);
void Toggle();  // F8
bool Enabled();

// From d3d/hooks.cpp, render thread.
void OnPresent(ID3D11Device* device, ID3D11DeviceContext* immediate);
void OnSetRenderTargets(UINT num, ID3D11RenderTargetView* const* views);  // call while Enabled()
bool WantsBinds();  // cheap: true while a display pass runs or views are being learned
void OnBindShaderResources(UINT num, ID3D11ShaderResourceView* const* views);
// Replacement for `view` with a reference the caller must Release(), or nullptr.
ID3D11ShaderResourceView* Substitute(ID3D11ShaderResourceView* view);
void Reset(const char* reason);  // the game recreated its device

// Truck change (any thread): forget the current screen and prefer this display size (0x0: none);
// pinned: the display was picked by hand for this truck and is used whether it shows the map or
// not. The displays of the new truck are written to the log once the screen is on.
void OnVehicleChanged(unsigned preferredWidth, unsigned preferredHeight, bool pinned);
// Calibration (any thread): switch to the next (+1) or previous (-1) display at the end of a chain,
// map displays first. The display picked this way is pinned.
void CycleCandidate(int direction);
// Size of the display texture in use, 0x0 when none, and whether it is pinned (any thread).
void TargetSize(unsigned* width, unsigned* height);
bool TargetPinned();
}  // namespace nav
