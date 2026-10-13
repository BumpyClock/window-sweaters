// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitStrip.h"
#include "ResourceLedger.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <list>

namespace window_sweaters::rendering {
class KnitAssembler final {
public:
    explicit KnitAssembler(ID3D11Device* device, std::shared_ptr<ResourceLedger> resources = {})
        : device_(device), resources_(resources ? std::move(resources) : std::make_shared<ResourceLedger>()) {}
    HRESULT Assemble(const KnitPerimeter& perimeter, std::size_t side,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& colour,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& heights, ResourceLedger::Lease* upload = nullptr) noexcept;
    void ClearCache() noexcept { tiles_.clear(); cachedBytes_ = 0; }
    std::size_t CachedBytes() const noexcept { return cachedBytes_; }
    std::uint64_t TileUploads() const noexcept { return uploads_; }
    std::uint64_t TileCacheHits() const noexcept { return hits_; }
    static constexpr std::size_t CacheByteLimit = 32u * 1024u * 1024u;
private:
    struct CachedTile {
        std::uint64_t serial = 0;
        bool heights = false;
        std::uint64_t tick = 0;
        std::size_t bytes = 0;
        ResourceLedger::CacheLease lease;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        std::array<Microsoft::WRL::ComPtr<ID2D1BitmapBrush>, 3> brushes;
    };
    static constexpr std::size_t MaxCachedTiles = 12;
    HRESULT Initialize() noexcept;
    HRESULT Target(std::uint32_t width, std::uint32_t height, float scale,
        Microsoft::WRL::ComPtr<ID2D1Bitmap1>& target, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) noexcept;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> Brush(int role, const knitting::Tile& tile, bool heights, float px, float py,
        float repeatWidth, std::array<std::shared_ptr<CachedTile>, 3>& pinned, ResourceLedger::Lease* upload);
    void EvictTile() noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::shared_ptr<ResourceLedger> resources_;
    Microsoft::WRL::ComPtr<ID2D1Device> drawing_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    std::list<std::shared_ptr<CachedTile>> tiles_;
    std::size_t cachedBytes_{};
    std::uint64_t tick_{}, uploads_{}, hits_{};
};
}
