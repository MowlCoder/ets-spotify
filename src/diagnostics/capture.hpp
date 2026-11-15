#pragma once
#include <d3d11.h>

// Frame capture for reverse-engineering how the game draws its in-cab displays.
// Home key -> the next few frames are recorded: render-target switches, viewports while rendering
// into a display-sized candidate, pixel-shader binds of candidates and ExecuteCommandList.
// Draw calls are not recorded (their vtable slots cannot be hooked, see d3d_hooks.cpp).
// The record is written to ets_spotify.log with a "[cap]" prefix.
namespace capture {
void Request(int frames);
bool Active();

// Called from the D3D hooks.
void OnPresent();
void OnSetRenderTargets(ID3D11DeviceContext* ctx, UINT num, ID3D11RenderTargetView* const* views);
void OnSetViewports(ID3D11DeviceContext* ctx, UINT num, const D3D11_VIEWPORT* viewports);
void OnBindShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT num,
                           ID3D11ShaderResourceView* const* views);
void OnExecuteCommandList(ID3D11DeviceContext* ctx);
}  // namespace capture
