// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "..\Knitting\KnitTile.h"
#include <array>

namespace window_sweaters::rendering {
enum class KnitAnchor : std::uint8_t { Corner, Centre };
struct KnitFrameSpec {
    float widthDip = 620.f, heightDip = 140.f;
    // Native adapters pass BOTH physical content dimensions, avoiding a
    // lossy physical -> DIP -> ceil(physical) roundtrip. Zero selects DIP input.
    std::uint32_t contentWidthPixels = 0, contentHeightPixels = 0;
    // Native geometry can supply its double-precision ceil(band*scale).
    // Zero selects the natural float padding; overrides may differ by one pixel.
    std::uint32_t physicalPaddingPixels = 0;
    float radiusDip = 10.f, bandDip = 10.f, tuckDip = 1.f;
    float deviceScale = 1.f, dim = 0.f; // Scale accepts 1/8x..8x.
    KnitAnchor anchor = KnitAnchor::Corner;
};
struct KnitRasterPiece {
    // Integer physical offsets relative to the outer rectangle's top left.
    std::uint32_t x = 0, y = 0;
    knitting::RasterImage image;
};
struct KnitPerimeter {
    std::uint32_t width = 0, height = 0;
    // Desktop placement is target physical top-left minus contentInsetPixels.
    // Fractional band*scale uses a transparent subpixel gutter, not a target shift.
    std::uint32_t contentInsetPixels = 0;
    float bandDip = 0.f, deviceScale = 1.f;
    std::array<KnitRasterPiece, 4> pieces; // top, right, bottom, left
    std::shared_ptr<const knitting::Tile> tile;
};

class KnitRaster {
public:
    // Independent of target position/lifecycle. Caller initializes COM.
    // Only narrow perimeter WIC surfaces are allocated.
    static KnitPerimeter RenderPerimeter(const KnitFrameSpec& frame,
        knitting::TileSpec tile, knitting::TileCache& cache);
    // Composes the exact production pieces; intended for offscreen fixtures.
    static knitting::RasterImage Compose(const KnitPerimeter& perimeter);
    static knitting::RasterImage RenderFrame(const KnitFrameSpec& frame,
        knitting::TileSpec tile, knitting::TileCache& cache);
};
}
