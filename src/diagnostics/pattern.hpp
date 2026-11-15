#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// Test pattern: magenta stripes, yellow border and a big "#<number>" in the middle.
namespace pattern {
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateSrv(ID3D11Device* device, int number);
}
