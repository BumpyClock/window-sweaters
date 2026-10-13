// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitRaster.h"
#include "ResourceLedger.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <list>

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
    explicit GpuShader(ID3D11Device* device, std::shared_ptr<ResourceLedger> resources = {})
        : device_(device), resources_(resources ? std::move(resources) : std::make_shared<ResourceLedger>()) {}
    HRESULT Upload(const knitting::RasterImage& image, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) noexcept;
    HRESULT Render(const KnitSources& sources, const KnitMask& mask, ID3D11RenderTargetView* target,
        POINT offset, ResourceLedger::Lease* upload = nullptr) noexcept;
    HRESULT Submit() noexcept;
    ID3D11Device* Device() const noexcept { return device_.Get(); }
    std::uint64_t DrawCount() const noexcept { return draws_; }
    void ClearCache() noexcept { coordinates_.clear(); cachedBytes_ = 0; }
    std::size_t CachedBytes() const noexcept { return cachedBytes_; }
    std::uint64_t CoordinateUploads() const noexcept { return coordinateUploads_; }
    std::uint64_t CoordinateCacheHits() const noexcept { return coordinateHits_; }
    static constexpr std::size_t CacheByteLimit = 1024u * 1024u;
private:
    struct Coordinates {
        std::uint32_t length{}, originBits{}, scaleBits{};
        std::uint64_t tick{};
        ResourceLedger::CacheLease lease;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };
    static constexpr std::size_t MaxCachedCoordinates = 64;
    HRESULT CoordinateTable(std::uint32_t length, float origin, float scale,
        std::shared_ptr<Coordinates>& table, ResourceLedger::Lease* upload) noexcept;
    HRESULT Initialize() noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::shared_ptr<ResourceLedger> resources_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> knit_, copy_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> parameters_, placement_;
    std::list<std::shared_ptr<Coordinates>> coordinates_;
    std::size_t cachedBytes_{};
    std::uint64_t draws_{}, tick_{}, coordinateUploads_{}, coordinateHits_{};
};
}
