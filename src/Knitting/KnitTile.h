// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "knit_math.h"
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

namespace window_sweaters::knitting {

inline constexpr float MinimumDeviceScale = 0.125f;
inline constexpr float MaximumDeviceScale = 8.f;

enum class Stitch : std::uint8_t { Stockinette, Rib, Garter };
enum class Material : std::uint8_t { Plain, Raised };
enum class Sampling : std::uint8_t { Nearest, Linear };

struct TileSpec {
    ws_knit_gauge gauge = ws_knit_default_gauge();
    float bandDip = 10.f;
    std::uint32_t baseArgb = 0xff4e8098u;
    Stitch stitch = Stitch::Stockinette;
    Material material = Material::Plain;
    // Invalidation token, NOT a chart selector: this slice has no chart input.
    std::uint64_t chartRevision = 0;
    float deviceScale = 1.f; // Supported logical-to-physical range: 1/8x..8x.
    int rasterScale = 0;
    Sampling sampling = Sampling::Linear;
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
    RasterImage image;
};

class TileCache {
public:
    explicit TileCache(std::size_t byteBudget = 8u * 1024u * 1024u);
    std::shared_ptr<const Tile> Get(const TileSpec& spec);
    void Clear();
    std::size_t ResidentBytes() const;
    std::uint64_t GenerationCount() const;
private:
    struct Entry { std::shared_ptr<const Tile> tile; };
    const std::size_t budget_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    std::size_t resident_ = 0;
    std::uint64_t generations_ = 0;
};

// Throws invalid_argument for invalid/unsupported inputs, system_error for
// D2D/WIC failures. Callers need a COM apartment; no target HWND/device is used.
TileSpec NormalizeTileSpec(TileSpec spec);
void SavePng(const RasterImage& image, const wchar_t* path);

}
