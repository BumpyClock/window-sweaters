// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "..\Knitting\KnitTile.h"
#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace window_sweaters::rendering {
enum class KnitAnchor : std::uint8_t { Corner, Centre };
enum class Renderer : std::uint8_t { Cpu, Gpu };
struct KnitFrameSpec {
    float widthDip = 620.f, heightDip = 140.f;
    std::uint32_t contentWidthPixels = 0, contentHeightPixels = 0;
    std::uint32_t physicalPaddingPixels = 0;
    float radiusDip = 10.f, bandDip = 10.f, tuckDip = 1.f;
    float deviceScale = 1.f, dim = 0.f;
    KnitAnchor anchor = KnitAnchor::Corner;
    Renderer renderer = Renderer::Cpu;
};
struct KnitShaderParameters {
    float ringWidth = 0, ringHeight = 0, outerRadius = 0, inset = 0;
    float innerRadius = 0, originX = 0, originY = 0, scale = 0;
    float relief = 0, band = 0, pitch = 0, phase = 0;
    std::uint32_t side = 0;
    float inverseScale = 0;
    std::array<float, 2> padding{};
};
static_assert(sizeof(KnitShaderParameters) == 64);
struct KnitRasterPiece {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    knitting::RasterImage image;
};
struct KnitAssembly {
    KnitFrameSpec frame;
    float depth = 0, gutterDip = 0;
    std::shared_ptr<const knitting::Tile> patch, cuff;
    std::array<KnitShaderParameters, 4> parameters;
};
struct KnitPerimeter {
    std::uint32_t width = 0, height = 0;
    std::uint32_t contentInsetPixels = 0, shadowMarginPixels = 0;
    float bandDip = 0.f, deviceScale = 1.f;
    std::array<KnitRasterPiece, 4> pieces;
    std::shared_ptr<const knitting::Tile> tile;
    std::optional<KnitAssembly> assembly;
    std::array<const knitting::Tile*, 3> RetainedTiles() const noexcept {
        std::array<const knitting::Tile*, 3> tiles{tile.get(),
            assembly ? assembly->patch.get() : nullptr,
            assembly ? assembly->cuff.get() : nullptr};
        for (std::size_t i = 0; i < tiles.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (tiles[i] == tiles[j]) { tiles[i] = nullptr; break; }
        return tiles;
    }
    bool PieceValid(const KnitRasterPiece& piece) const noexcept {
        const auto& image = piece.image;
        if (!piece.width || !piece.height || piece.x > width || piece.y > height ||
            piece.width > width - piece.x || piece.height > height - piece.y) return false;
        if (assembly) return image.pixels.empty() && !image.width && !image.height;
        return image.width == piece.width && image.height == piece.height && image.stride == piece.width * 4u &&
            image.pixels.size() == static_cast<std::size_t>(piece.width) * piece.height;
    }
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
