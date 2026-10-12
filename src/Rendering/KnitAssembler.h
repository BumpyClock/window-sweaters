// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitRaster.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <vector>

namespace window_sweaters::rendering {
class KnitAssembler final {
public:
    explicit KnitAssembler(ID3D11Device* device) noexcept : device_(device) {}
    HRESULT Assemble(const KnitPerimeter& perimeter, std::size_t side,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& colour,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& heights) noexcept;
private:
    struct CachedTile {
        std::uint64_t serial = 0;
        bool heights = false;
        std::uint64_t tick = 0;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    };
    static constexpr std::size_t MaxCachedTiles = 12;
    HRESULT Initialize() noexcept;
    HRESULT Target(std::uint32_t width, std::uint32_t height, float scale,
        Microsoft::WRL::ComPtr<ID2D1Bitmap1>& target, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) noexcept;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> Brush(const knitting::Tile& tile, bool heights, float px, float py,
        float repeatWidth);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Device> drawing_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    std::vector<CachedTile> tiles_;
    std::uint64_t tick_{};
};
}
