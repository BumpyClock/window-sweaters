// SPDX-License-Identifier: GPL-3.0-only
#include "BorderOracle.h"
#include "BorderMath.h"
#include "../../src/Rendering/KnitStrip.h"
#include "../../src/Knitting/WicSurface.h"
#include <algorithm>
#include <stdexcept>

namespace window_sweaters::testing {
using knitting::detail::Check;
using knitting::detail::ComPtr;

knitting::RasterImage SoftwareStrip(const rendering::KnitPerimeter& perimeter, std::size_t side, bool heights) {
    if (!perimeter.tile || side >= perimeter.pieces.size() || !perimeter.PieceValid(perimeter.pieces[side]))
        throw std::invalid_argument("Invalid reference perimeter strip");
    const auto& piece = perimeter.pieces[side];
    knitting::detail::WicSurface surface(piece.width, piece.height, perimeter.deviceScale);
    const rendering::KnitTileBrush brush = [&](int, const knitting::Tile& tile, bool stitchHeight,
        float px, float py, float repeatWidth) {
        const auto& image = stitchHeight ? tile.height : tile.image;
        ComPtr<ID2D1Bitmap> bitmap;
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f * tile.layout.raster_scale, 96.f * tile.layout.raster_scale);
        Check(surface.target->CreateBitmap(D2D1::SizeU(image.width, image.height), image.pixels.data(),
            image.stride, properties, &bitmap), "Upload reference periodic tile");
        const auto sampling = tile.spec.sampling == knitting::Sampling::Nearest
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        ComPtr<ID2D1BitmapBrush> result;
        Check(surface.target->CreateBitmapBrush(bitmap.Get(), D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, sampling), &result), "Create reference yarn brush");
        result->SetTransform(D2D1::Matrix3x2F::Scale(repeatWidth / tile.layout.width_dip, 1.f)
            * D2D1::Matrix3x2F::Translation(px, py));
        return result;
    };
    rendering::DrawKnitStrip(*surface.target.Get(), perimeter.assembly, *perimeter.tile, piece,
        static_cast<int>(side), heights, brush);
    return surface.Read();
}

std::array<knitting::RasterImage, 4> ReferenceStrips(const rendering::KnitPerimeter& perimeter) {
    std::array<knitting::RasterImage, 4> strips;
    for (std::size_t side = 0; side < strips.size(); ++side) {
        auto& strip = strips[side];
        strip = SoftwareStrip(perimeter, side, false);
        const auto heights = SoftwareStrip(perimeter, side, true);
        const auto& p = perimeter.assembly.parameters[side];
        const ws_knit_ring ring{p.ringWidth, p.ringHeight, p.outerRadius, p.inset, p.innerRadius};
        const auto width = static_cast<int>(strip.width), height = static_cast<int>(strip.height);
        if (!ws_knit_shade_ring_relief(&ring, p.originX, p.originY, p.scale, strip.pixels.data(),
                width, height, heights.pixels.data(), p.relief) ||
            !ws_knit_fray_ring(&ring, static_cast<int>(side), ws_knit_fray_depth(p.band), p.pitch, p.phase,
                p.originX, p.originY, p.scale, strip.pixels.data(), width, height) ||
            !ws_knit_ring_drop_shadow(&ring, static_cast<int>(side), p.band, p.originX, p.originY, p.scale,
                strip.pixels.data(), width, height))
            throw std::invalid_argument("Invalid reference border shading");
    }
    return strips;
}

knitting::RasterImage ComposeStrips(const rendering::KnitPerimeter& perimeter,
    const std::array<knitting::RasterImage, 4>& strips) {
    if (!perimeter.width || !perimeter.height || perimeter.width > 32768 || perimeter.height > 32768 ||
        std::uint64_t(perimeter.width) * perimeter.height > 16u * 1024u * 1024u)
        throw std::invalid_argument("Reference frame exceeds limits");
    knitting::RasterImage image{perimeter.width, perimeter.height, perimeter.width * 4u, {}};
    image.pixels.resize(std::size_t(image.width) * image.height);
    for (std::size_t side = 0; side < strips.size(); ++side) {
        const auto& piece = perimeter.pieces[side];
        const auto& strip = strips[side];
        if (!perimeter.PieceValid(piece) || strip.width != piece.width || strip.height != piece.height ||
            strip.stride != strip.width * 4u || strip.pixels.size() != std::size_t(strip.width) * strip.height)
            throw std::invalid_argument("Invalid reference strip image");
        for (std::uint32_t y = 0; y < strip.height; ++y)
            for (std::uint32_t x = 0; x < strip.width; ++x) {
                const auto source = strip.pixels[std::size_t(y) * strip.width + x];
                auto& destination = image.pixels[std::size_t(y + piece.y) * image.width + x + piece.x];
                const auto inverse = 255u - (source >> 24);
                std::uint32_t out = 0;
                for (int shift = 0; shift < 32; shift += 8) {
                    const auto channel = ((source >> shift) & 255u) +
                        (((destination >> shift) & 255u) * inverse + 127u) / 255u;
                    out |= std::min(255u, channel) << shift;
                }
                destination = out;
            }
    }
    return image;
}

knitting::RasterImage ReferenceFrame(const rendering::KnitPerimeter& perimeter) {
    return ComposeStrips(perimeter, ReferenceStrips(perimeter));
}
}
