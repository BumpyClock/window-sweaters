// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "..\Knitting\KnitTile.h"
#include <array>
#include <functional>
#include <memory>

namespace window_sweaters::rendering {
enum class KnitAnchor : std::uint8_t { Corner, Centre };
struct KnitFrameSpec {
    float widthDip = 620.f, heightDip = 140.f;
    std::uint32_t contentWidthPixels = 0, contentHeightPixels = 0;
    std::uint32_t physicalPaddingPixels = 0;
    float radiusDip = 10.f, bandDip = 10.f, tuckDip = 1.f;
    float deviceScale = 1.f, dim = 0.f;
    KnitAnchor anchor = KnitAnchor::Corner;
};
struct KnitRasterPiece {
    std::uint32_t x = 0, y = 0;
    knitting::RasterImage image;
};
struct KnitPerimeter {
    std::uint32_t width = 0, height = 0;
    std::uint32_t contentInsetPixels = 0, shadowMarginPixels = 0;
    float bandDip = 0.f, deviceScale = 1.f;
    std::array<KnitRasterPiece, 4> pieces;
    std::shared_ptr<const knitting::Tile> tile;
};

class KnitRasterScratch {
public:
    struct Stats { std::uint64_t factories = 0, targets = 0, uploads = 0, reused = 0; };
    explicit KnitRasterScratch(std::size_t byteBudget = 16u * 1024u * 1024u);
    ~KnitRasterScratch();
    KnitRasterScratch(const KnitRasterScratch&) = delete;
    KnitRasterScratch& operator=(const KnitRasterScratch&) = delete;
    std::size_t ResidentBytes() const noexcept;
    Stats GetStats() const noexcept;
    void Clear() noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct KnitRenderCancelled {};
struct KnitRenderControl {
    KnitRasterScratch* scratch = nullptr;
    std::function<bool()> stop;
};

class KnitRaster {
public:
    static KnitPerimeter RenderPerimeter(const KnitFrameSpec& frame,
        knitting::TileSpec tile, knitting::TileCache& cache,
        const KnitRenderControl& control = {});
    static knitting::RasterImage Compose(const KnitPerimeter& perimeter);
    static knitting::RasterImage RenderFrame(const KnitFrameSpec& frame,
        knitting::TileSpec tile, knitting::TileCache& cache);
};
}
