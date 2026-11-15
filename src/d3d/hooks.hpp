#pragma once

// Direct3D 11 / DXGI vtable hooks:
//   IDXGISwapChain::Present / Present1   end of frame: upload our picture, decide the nav texture
//   ID3D11Device::CreateDeferredContext  so deferred contexts get hooked too
//   ID3D11DeviceContext::PSSetShaderResources, OMSetRenderTargets, RSSetViewports,
//   ExecuteCommandList                   observe the frame, swap the nav texture's view for ours
namespace hooks {
// Idempotent. The hooks stay in place for the life of the process (the DLL is pinned);
// Disable() turns them into pass-throughs.
bool Install();
void Disable();
// Bit s set = context method slot s currently points to our hook (diagnostics and tests).
unsigned HookedSlotMask(void* context);
}  // namespace hooks
