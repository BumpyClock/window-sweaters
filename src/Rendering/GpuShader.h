// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitRaster.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace window_sweaters::rendering {
struct KnitMask {
    float left = 0, top = 0, right = 0, bottom = 0, radius = 0;
    bool hole = false;
};
struct KnitSources {
    std::uint32_t width = 0, height = 0;
    ID3D11ShaderResourceView* colour = nullptr;
    ID3D11ShaderResourceView* heights = nullptr;
    const KnitShaderParameters* parameters = nullptr;
};
class GpuShader final {
public:
    explicit GpuShader(ID3D11Device* device) noexcept : device_(device) {}
    HRESULT Upload(const knitting::RasterImage& image, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) noexcept;
    HRESULT Render(const KnitSources& sources, const KnitMask& mask, ID3D11RenderTargetView* target,
        POINT offset) noexcept;
    ID3D11Device* Device() const noexcept { return device_.Get(); }
    std::uint64_t DrawCount() const noexcept { return draws_; }
private:
    HRESULT Initialize() noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> knit_, copy_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> parameters_, placement_;
    std::uint64_t draws_{};
};
}
