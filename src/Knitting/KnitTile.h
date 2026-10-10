// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "knit_math.h"
#include "..\Designs\Catalogue.h"
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace window_sweaters::knitting {

inline constexpr float MinimumDeviceScale = 0.125f;
inline constexpr float MaximumDeviceScale = 8.f;

enum class Stitch : std::uint8_t { Stockinette, Rib, Garter };
enum class Material : std::uint8_t { Plain, Raised };
enum class Sampling : std::uint8_t { Nearest, Linear };

struct ChartAppearance {
    std::uint32_t flags = 0, cornerArgb = 0, cuffArgb = 0;
    bool outsideAnchored = false;
    bool operator==(const ChartAppearance&) const = default;
};

// Owns the cells independently of catalogue/import-cache lifetime. Alpha below
// 128 is the base-yarn sentinel; every other cell retains its original RGB.
class Chart {
public:
    Chart(std::string semanticId, std::uint64_t contentRevision,
          std::uint32_t width, std::uint32_t height,
          std::vector<std::uint32_t> argbCells, ChartAppearance appearance = {});
    const std::string semanticId;
    const std::uint64_t contentRevision;
    const std::uint32_t width, height;
    const std::vector<std::uint32_t> argbCells;
    const ChartAppearance appearance;
    const std::uint64_t contentHash;
    std::size_t ByteSize() const noexcept;
};

std::shared_ptr<const Chart> MakeDesignChart(const designs::ChartView& source,
    std::string semanticId, std::uint64_t contentRevision = 0);

struct TileSpec {
    ws_knit_gauge gauge = ws_knit_default_gauge();
    float bandDip = 10.f;
    std::uint32_t baseArgb = 0xff4e8098u;
    Stitch stitch = Stitch::Stockinette;
    Material material = Material::Plain;
    // Legacy invalidation token, never a catalogue index.
    std::uint64_t chartRevision = 0;
    float deviceScale = 1.f; // Supported logical-to-physical range: 1/8x..8x.
    int rasterScale = 0;
    Sampling sampling = Sampling::Linear;
    std::shared_ptr<const Chart> chart;
};

// Physical pixels, top-down rows, little-endian premultiplied BGRA.
// Tile pixels are opaque; logical +y is the inward direction of an edge.
struct RasterImage {
    std::uint32_t width = 0, height = 0, stride = 0;
    std::vector<std::uint32_t> pixels;
    std::size_t ByteSize() const noexcept { return pixels.size() * sizeof(std::uint32_t); }
};

struct Tile {
    TileSpec spec;
    ws_knit_layout layout{};
    // image is unlit albedo (occlusion only); height is a same-size periodic
    // greyscale stitch height (blue channel) for screen-space lighting.
    RasterImage image, height;
    // Process-unique immutable identity (renderer resource caches key on it).
    std::uint64_t serial = 0;
    std::size_t ByteSize() const noexcept {
        return image.ByteSize() + height.ByteSize() + (spec.chart ? spec.chart->ByteSize() : 0);
    }
};

class TileCache {
public:
    explicit TileCache(std::size_t byteBudget = 8u * 1024u * 1024u);
    std::shared_ptr<const Tile> Get(const TileSpec& spec);
    void Clear();
    std::size_t ResidentBytes() const;
    std::uint64_t GenerationCount() const;
    std::uint64_t HitCount() const;
private:
    struct Entry { std::shared_ptr<const Tile> tile; };
    const std::size_t budget_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    std::size_t resident_ = 0;
    std::uint64_t generations_ = 0;
    std::uint64_t hits_ = 0;
};

// Throws invalid_argument for invalid/unsupported inputs, system_error for
// D2D/WIC failures. Callers need a COM apartment; no target HWND/device is used.
TileSpec NormalizeTileSpec(TileSpec spec);
void SavePng(const RasterImage& image, const wchar_t* path);

}
