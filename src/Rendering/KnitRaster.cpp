// SPDX-License-Identifier: GPL-3.0-only
// Frame geometry adapted from upstream src/knit.c:505-731 at
// a49dff52483dafdbe27f972bd271cd28d37cef1c. No CoreGraphics compatibility layer.
#include "KnitRaster.h"
#include "..\Geometry\Geometry.h"
#include "..\Knitting\WicSurface.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace window_sweaters::rendering {
namespace {
using knitting::detail::Check;
using knitting::detail::ComPtr;
using knitting::detail::WicSurface;

KnitFrameSpec Normalize(KnitFrameSpec f) {
    if (!std::isfinite(f.widthDip) || !std::isfinite(f.heightDip)
        || !std::isfinite(f.radiusDip) || !std::isfinite(f.bandDip)
        || !std::isfinite(f.tuckDip) || !std::isfinite(f.deviceScale)
        || !std::isfinite(f.dim) || f.widthDip <= 0.f || f.heightDip <= 0.f
        || f.bandDip > 256.f || f.deviceScale < knitting::MinimumDeviceScale
        || f.deviceScale > knitting::MaximumDeviceScale
        || f.dim < 0.f || f.dim > 1.f
        || ((f.contentWidthPixels == 0) != (f.contentHeightPixels == 0))
        || (f.anchor != KnitAnchor::Corner && f.anchor != KnitAnchor::Centre))
        throw std::invalid_argument("Invalid knit frame specification");
    if (f.contentWidthPixels) {
        f.widthDip = static_cast<float>(f.contentWidthPixels) / f.deviceScale;
        f.heightDip = static_cast<float>(f.contentHeightPixels) / f.deviceScale;
    }
    if (!std::isfinite(f.widthDip) || !std::isfinite(f.heightDip))
        throw std::invalid_argument("Derived knit frame dimensions are nonfinite");
    f.bandDip = std::max(2.f, f.bandDip);
    f.radiusDip = std::clamp(f.radiusDip, 0.f, std::min(f.widthDip, f.heightDip) * 0.5f);
    f.tuckDip = std::max(1.f, f.tuckDip);
    float half = std::min(f.widthDip, f.heightDip) * 0.5f - 2.f;
    if (f.tuckDip > half) f.tuckDip = half > 1.f ? half : 1.f;
    return f;
}

ComPtr<ID2D1Geometry> Ring(ID2D1Factory* factory, const KnitFrameSpec& f) {
    ComPtr<ID2D1RoundedRectangleGeometry> outer, inner;
    float band = f.bandDip, tuck = f.tuckDip;
    float ow = f.widthDip + 2.f * band, oh = f.heightDip + 2.f * band;
    Check(factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(0, 0, ow, oh), f.radiusDip + band, f.radiusDip + band), &outer),
        "Create outer rounded ring");
    // Upstream tiny-window tuck can invert the inner rectangle. Its hole is
    // then empty, not a reflected negative-sized rectangle.
    if (f.widthDip <= 2.f * tuck || f.heightDip <= 2.f * tuck) return outer;
    float radius = std::max(0.f, f.radiusDip - tuck);
    Check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(
        D2D1::RectF(band + tuck, band + tuck, band + f.widthDip - tuck,
                   band + f.heightDip - tuck), radius, radius), &inner), "Create inner rounded ring");
    ComPtr<ID2D1GeometryGroup> ring;
    ID2D1Geometry* contours[] = {outer.Get(), inner.Get()};
    Check(factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, contours, 2, &ring),
          "Create even-odd rounded ring");
    return ring;
}

ComPtr<ID2D1PathGeometry> Mitre(ID2D1Factory* factory, float length, float depth) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    Check(factory->CreatePathGeometry(&path), "Create mitre path");
    Check(path->Open(&sink), "Open mitre path");
    sink->BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F points[] = {{length, 0}, {length - depth, depth}, {depth, depth}};
    sink->AddLines(points, 3);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    Check(sink->Close(), "Close mitre path");
    return path;
}

ComPtr<ID2D1Geometry> CuffRing(ID2D1Factory* factory, const KnitFrameSpec& f) {
    float ow = f.widthDip + 2.f * f.bandDip, oh = f.heightDip + 2.f * f.bandDip;
    float width = f.bandDip / 3.f, radius = f.radiusDip + f.bandDip;
    ComPtr<ID2D1RoundedRectangleGeometry> outer, inner;
    Check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(
        D2D1::RectF(0, 0, ow, oh), radius, radius), &outer), "Create outer cuff curve");
    Check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(
        D2D1::RectF(width, width, ow-width, oh-width), radius-width, radius-width), &inner),
        "Create inner cuff curve");
    ID2D1Geometry* contours[] = {outer.Get(), inner.Get()};
    ComPtr<ID2D1GeometryGroup> ring;
    Check(factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, contours, 2, &ring), "Create curved cuff band");
    return ring;
}

ComPtr<ID2D1BitmapBrush> TileBrush(ID2D1RenderTarget* target, const knitting::Tile& tile,
                                  float px, float py, float repeatWidth, bool stitchHeight = false) {
    ComPtr<ID2D1Bitmap> bitmap;
    const auto& image = stitchHeight ? tile.height : tile.image;
    auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * tile.layout.raster_scale, 96.f * tile.layout.raster_scale);
    Check(target->CreateBitmap(D2D1::SizeU(image.width, image.height),
        image.pixels.data(), image.stride, properties, &bitmap), "Upload software periodic tile");
    auto sampling = tile.spec.sampling == knitting::Sampling::Nearest
        ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
    ComPtr<ID2D1BitmapBrush> brush;
    Check(target->CreateBitmapBrush(bitmap.Get(), D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, sampling), &brush), "Create periodic yarn brush");
    brush->SetTransform(D2D1::Matrix3x2F::Scale(repeatWidth / tile.layout.width_dip, 1.f)
        * D2D1::Matrix3x2F::Translation(px, py));
    return brush;
}

KnitRasterPiece RenderSide(const KnitFrameSpec& f, const knitting::Tile& tile, int side,
                           std::uint32_t width, std::uint32_t height, float depth, float gutterDip,
                           const knitting::Tile* patch, const knitting::Tile* cuff) {
    float ow = f.widthDip + 2.f * f.bandDip, oh = f.heightDip + 2.f * f.bandDip;
    float right = std::floor((gutterDip + ow - depth) * f.deviceScale);
    float bottom = std::floor((gutterDip + oh - depth) * f.deviceScale);
    float narrow = std::ceil((gutterDip + depth) * f.deviceScale);
    if (!std::isfinite(right) || !std::isfinite(bottom) || !std::isfinite(narrow)
        || right < 0.f || right > width || bottom < 0.f || bottom > height
        || narrow < 0.f || narrow > std::max(width, height))
        throw std::invalid_argument("Derived knit piece bounds are invalid");
    auto dx = static_cast<std::uint32_t>(right);
    auto dy = static_cast<std::uint32_t>(bottom);
    auto dw = static_cast<std::uint32_t>(narrow);
    KnitRasterPiece piece;
    std::uint32_t pw = width, ph = height;
    if (side == 0) ph = std::min(dw, height);
    if (side == 1) { piece.x = dx; pw = width - dx; }
    if (side == 2) { piece.y = dy; ph = height - dy; }
    if (side == 3) pw = std::min(dw, width);
    WicSurface surface(pw, ph, f.deviceScale);
    auto ring = Ring(surface.factory.Get(), f);
    float length = side % 2 ? oh : ow;
    auto mitre = Mitre(surface.factory.Get(), length, depth);
    const D2D1::Matrix3x2F orientations[] = {
        D2D1::Matrix3x2F::Identity(),
        D2D1::Matrix3x2F(0, 1, -1, 0, ow, 0),
        D2D1::Matrix3x2F(-1, 0, 0, -1, ow, oh),
        D2D1::Matrix3x2F(0, -1, 1, 0, 0, oh)
    };
    auto placement = D2D1::Matrix3x2F::Translation(
        gutterDip - static_cast<float>(piece.x) / f.deviceScale,
        gutterDip - static_cast<float>(piece.y) / f.deviceScale);
    auto look = tile.spec.chart ? tile.spec.chart->appearance : knitting::ChartAppearance{};
    float px = f.anchor == KnitAnchor::Centre ? length * 0.5f - tile.layout.width_dip * 0.5f : 0.f;
    float py = look.outsideAnchored ? 0.f : f.bandDip * 0.5f - tile.layout.height_dip * 0.5f;
    bool fitted = (look.flags & designs::ChartFittedRepeat) != 0;
    float repeatWidth = fitted ? length / std::max(1.f, std::round(length / tile.layout.width_dip))
        : static_cast<float>(tile.layout.width_dip);
    float cap = std::min(f.radiusDip + f.bandDip, length * .5f);
    float available = std::max(0.f, length - 2.f * cap);
    if (patch && available > 0.f) {
        repeatWidth = available / std::max(1.f, std::round(available / tile.layout.width_dip));
        px = cap;
    } else if (fitted) px = 0.f;
    // Authored CoreGraphics plates tile a fitted image at an integral device
    // period. Keep the logical fit/caps, but match that sampler period instead
    // of accumulating D2D's fractional-period phase drift down each edge.
    if (fitted || patch)
        repeatWidth = std::max(1.f, std::round(repeatWidth * f.deviceScale)) / f.deviceScale;
    auto brush = TileBrush(surface.target.Get(), tile, px, py, repeatWidth);
    ComPtr<ID2D1BitmapBrush> patchBrush, cuffBrush;
    ComPtr<ID2D1Geometry> cuffRing;
    if (patch) patchBrush = TileBrush(surface.target.Get(), *patch, 0, py,
        static_cast<float>(patch->layout.width_dip));
    if (cuff) {
        cuffBrush = TileBrush(surface.target.Get(), *cuff, 0, 0, static_cast<float>(cuff->layout.width_dip));
        cuffRing = CuffRing(surface.factory.Get(), f);
    }
    ComPtr<ID2D1SolidColorBrush> backing, black;
    auto cornerColor = patch ? patch->spec.baseArgb : tile.spec.baseArgb;
    Check(surface.target->CreateSolidColorBrush(knitting::detail::OpaqueColor(cornerColor, 0.96f), &backing),
          "Create opaque yarn backing");
    Check(surface.target->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, f.dim), &black), "Create dim overlay");
    surface.target->BeginDraw();
    surface.target->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    surface.target->SetTransform(placement);
    surface.target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), ring.Get()), nullptr);
    surface.target->SetTransform(orientations[side] * placement);
    auto mask = D2D1::LayerParameters(D2D1::InfiniteRect(), mitre.Get(), D2D1_ANTIALIAS_MODE_ALIASED);
    surface.target->PushLayer(mask, nullptr);
    auto rectangle = D2D1::RectF(0.f, 0.f, length, depth);
    surface.target->FillRectangle(rectangle, backing.Get());
    if (patch) {
        surface.target->FillRectangle(D2D1::RectF(0, 0, cap, depth), patchBrush.Get());
        surface.target->FillRectangle(D2D1::RectF(length-cap, 0, length, depth), patchBrush.Get());
        if (available > 0.f)
            surface.target->FillRectangle(D2D1::RectF(cap, 0, length-cap, depth), brush.Get());
    } else surface.target->FillRectangle(rectangle, brush.Get());
    if (cuff) {
        surface.target->SetTransform(placement);
        surface.target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), cuffRing.Get()), nullptr);
        surface.target->SetTransform(orientations[side] * placement);
        surface.target->FillRectangle(rectangle, cuffBrush.Get());
        surface.target->PopLayer();
    }
    if (f.dim > 0.001f) surface.target->FillRectangle(D2D1::RectF(-depth, -depth, length + 2.f*depth, 3.f*depth), black.Get());
    surface.target->PopLayer();
    surface.target->PopLayer();
    Check(surface.target->EndDraw(), "Render knit perimeter side");
    piece.image = surface.Read();
    // Stitch height takes the same clips, orientation and brush transforms as the
    // colour, so its pixel grid is screen space and the lighting needs no rotation.
    WicSurface relief(pw, ph, f.deviceScale, surface.factory.Get());
    {
        auto& rt = *relief.target.Get();
        auto heightBrush = TileBrush(&rt, tile, px, py, repeatWidth, true);
        ComPtr<ID2D1BitmapBrush> heightPatch, heightCuff;
        if (patch) heightPatch = TileBrush(&rt, *patch, 0, py, static_cast<float>(patch->layout.width_dip), true);
        if (cuff) heightCuff = TileBrush(&rt, *cuff, 0, 0, static_cast<float>(cuff->layout.width_dip), true);
        ComPtr<ID2D1SolidColorBrush> floor;
        Check(rt.CreateSolidColorBrush(D2D1::ColorF(.3f, .3f, .3f, 1.f), &floor), "Create stitch height backing");
        rt.BeginDraw();
        rt.Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
        rt.SetTransform(placement);
        rt.PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), ring.Get()), nullptr);
        rt.SetTransform(orientations[side] * placement);
        rt.PushLayer(mask, nullptr);
        rt.FillRectangle(rectangle, floor.Get());
        if (patch) {
            rt.FillRectangle(D2D1::RectF(0, 0, cap, depth), heightPatch.Get());
            rt.FillRectangle(D2D1::RectF(length-cap, 0, length, depth), heightPatch.Get());
            if (available > 0.f) rt.FillRectangle(D2D1::RectF(cap, 0, length-cap, depth), heightBrush.Get());
        } else rt.FillRectangle(rectangle, heightBrush.Get());
        if (cuff) {
            rt.SetTransform(placement);
            rt.PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), cuffRing.Get()), nullptr);
            rt.SetTransform(orientations[side] * placement);
            rt.FillRectangle(rectangle, heightCuff.Get());
            rt.PopLayer();
        }
        rt.PopLayer();
        rt.PopLayer();
        Check(rt.EndDraw(), "Render knit stitch height");
    }
    auto stitchHeights = relief.Read();
    auto material = tile.spec.material == knitting::Material::Raised
        ? ws_knit_raised_material(tile.spec.gauge) : tile.spec.gauge;
    bool hole = f.widthDip > 2.f * f.tuckDip && f.heightDip > 2.f * f.tuckDip;
    const ws_knit_ring tube{ow, oh, f.radiusDip + f.bandDip, hole ? f.bandDip + f.tuckDip : 0.f,
        std::max(0.f, f.radiusDip - f.tuckDip)};
    const float ox = static_cast<float>(piece.x) / f.deviceScale - gutterDip;
    const float oy = static_cast<float>(piece.y) / f.deviceScale - gutterDip;
    const int iw = static_cast<int>(pw), ih = static_cast<int>(ph);
    if (!ws_knit_shade_ring_relief(&tube, ox, oy, f.deviceScale, piece.image.pixels.data(), iw, ih,
            stitchHeights.pixels.data(), material.relief))
        throw std::invalid_argument("Invalid knit ring shading");
    if (!ws_knit_fray_ring(&tube, side, ws_knit_fray_depth(f.bandDip),
            repeatWidth / static_cast<float>(tile.layout.columns), px, ox, oy, f.deviceScale,
            piece.image.pixels.data(), iw, ih))
        throw std::invalid_argument("Invalid knit ring fray");
    if (!ws_knit_ring_drop_shadow(&tube, side, f.bandDip, ox, oy, f.deviceScale,
            piece.image.pixels.data(), iw, ih))
        throw std::invalid_argument("Invalid knit drop shadow");
    return piece;
}
}

KnitPerimeter KnitRaster::RenderPerimeter(const KnitFrameSpec& requested,
                                         knitting::TileSpec tileSpec, knitting::TileCache& cache) {
    auto f = Normalize(requested);
    float ow = f.widthDip + 2.f * f.bandDip, oh = f.heightDip + 2.f * f.bandDip;
    auto naturalInset = static_cast<std::uint32_t>(std::ceil(f.bandDip * f.deviceScale));
    auto inset = f.physicalPaddingPixels ? f.physicalPaddingPixels : naturalInset;
    if (inset + 1u < naturalInset || inset > naturalInset + 1u)
        throw std::invalid_argument("Authoritative physical padding differs by more than one pixel");
    // Desktop margin for the drop shadow, matching the native geometry envelope.
    const auto margin = geometry::ShadowMarginPixels(inset);
    double contentWidth = f.contentWidthPixels ? f.contentWidthPixels
        : std::ceil(static_cast<double>(f.widthDip) * f.deviceScale);
    double contentHeight = f.contentHeightPixels ? f.contentHeightPixels
        : std::ceil(static_cast<double>(f.heightDip) * f.deviceScale);
    double width = contentWidth + 2. * (inset + margin), height = contentHeight + 2. * (inset + margin);
    if (width > 32768 || height > 32768) throw std::invalid_argument("Frame dimensions exceed limits");
    float irad = std::max(0.f, f.radiusDip - f.tuckDip);
    float depth = f.bandDip + f.tuckDip + irad * (1.f - 0.7071067811865475244f) + 1.f;
    depth = std::min(depth, std::min(ow, oh) * 0.5f);
    // Bound the complete allocation before generating a tile or any surface.
    double narrow = std::ceil(depth * f.deviceScale) + 2. + margin;
    if (2. * narrow * (width + height) > 16. * 1024. * 1024.)
        throw std::invalid_argument("Perimeter byte limit exceeded");
    tileSpec.bandDip = f.bandDip;
    tileSpec.deviceScale = f.deviceScale;
    KnitPerimeter perimeter;
    perimeter.width = static_cast<std::uint32_t>(width);
    perimeter.height = static_cast<std::uint32_t>(height);
    perimeter.contentInsetPixels = inset + margin;
    perimeter.shadowMarginPixels = margin;
    perimeter.bandDip = f.bandDip;
    perimeter.deviceScale = f.deviceScale;
    perimeter.tile = cache.Get(tileSpec);
    std::shared_ptr<const knitting::Tile> patch, cuff;
    if (perimeter.tile->spec.chart) {
        const auto& look = perimeter.tile->spec.chart->appearance;
        auto solid = perimeter.tile->spec;
        solid.chart.reset(); solid.stitch = knitting::Stitch::Stockinette;
        solid.material = knitting::Material::Raised;
        if (look.flags & designs::ChartSolidCorners) {
            solid.baseArgb = look.cornerArgb ? look.cornerArgb : tileSpec.baseArgb;
            patch = cache.Get(solid);
        }
        if (look.flags & designs::ChartOuterCuff) {
            solid.baseArgb = look.cuffArgb;
            cuff = cache.Get(solid);
        }
    }
    float gutterDip = static_cast<float>(inset + margin) / f.deviceScale - f.bandDip;
    for (int side = 0; side < 4; ++side)
        perimeter.pieces[side] = RenderSide(f, *perimeter.tile, side, perimeter.width,
            perimeter.height, depth, gutterDip, patch.get(), cuff.get());
    return perimeter;
}

knitting::RasterImage KnitRaster::Compose(const KnitPerimeter& p) {
    if (!p.width || !p.height || p.width > 32768 || p.height > 32768
        || static_cast<std::uint64_t>(p.width) * p.height > 16u * 1024u * 1024u)
        throw std::invalid_argument("Fixture image exceeds limits");
    knitting::RasterImage image{p.width, p.height, p.width * 4, {}};
    image.pixels.resize(static_cast<std::size_t>(p.width) * p.height);
    for (const auto& piece : p.pieces) {
        const auto& src = piece.image;
        if (piece.x > p.width || piece.y > p.height || src.width > p.width - piece.x
            || src.height > p.height - piece.y || src.stride != src.width * 4u
            || src.pixels.size() != static_cast<std::size_t>(src.width) * src.height)
            throw std::invalid_argument("Invalid perimeter piece");
        for (std::uint32_t y = 0; y < src.height; ++y)
            for (std::uint32_t x = 0; x < src.width; ++x) {
                auto s = src.pixels[static_cast<std::size_t>(y) * src.width + x];
                auto& d = image.pixels[static_cast<std::size_t>(y + piece.y) * p.width + x + piece.x];
                std::uint32_t inverse = 255 - (s >> 24), out = 0;
                for (int shift = 0; shift < 32; shift += 8) {
                    auto channel = ((s >> shift) & 255) + (((d >> shift) & 255) * inverse + 127) / 255;
                    out |= std::min(255u, channel) << shift;
                }
                d = out;
            }
    }
    return image;
}

knitting::RasterImage KnitRaster::RenderFrame(const KnitFrameSpec& frame,
                                             knitting::TileSpec tile, knitting::TileCache& cache) {
    return Compose(RenderPerimeter(frame, tile, cache));
}
}
