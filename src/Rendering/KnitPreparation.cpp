// SPDX-License-Identifier: GPL-3.0-only
// Frame geometry adapted from upstream src/knit.c:505-731 at
// a49dff52483dafdbe27f972bd271cd28d37cef1c. No CoreGraphics compatibility layer.
#include "KnitStrip.h"
#include "..\Geometry\Geometry.h"
#include "..\Knitting\WicSurface.h"
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace window_sweaters::rendering {
using knitting::detail::Check;
using knitting::detail::ComPtr;

namespace {

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

ComPtr<ID2D1PathGeometry> Mitre(ID2D1Factory* factory, float length, float depth, float overlap) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    Check(factory->CreatePathGeometry(&path), "Create mitre path");
    Check(path->Open(&sink), "Open mitre path");
    sink->BeginFigure(D2D1::Point2F(-overlap, 0), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F points[] = {{length + overlap, 0}, {length - depth + overlap, depth}, {depth - overlap, depth}};
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

void Poll(const KnitRenderControl& control) {
    if (control.stop && control.stop()) throw KnitRenderCancelled{};
}

struct SideLayout {
    float length = 0, px = 0, py = 0, repeatWidth = 0, cap = 0, available = 0;
    D2D1::Matrix3x2F orientation, placement;
};

SideLayout Layout(const KnitAssembly& a, const knitting::Tile& tile, const KnitPiece& piece, int side) {
    const auto& f = a.frame;
    const float ow = f.widthDip + 2.f * f.bandDip, oh = f.heightDip + 2.f * f.bandDip;
    const D2D1::Matrix3x2F orientations[] = {
        D2D1::Matrix3x2F::Identity(),
        D2D1::Matrix3x2F(0, 1, -1, 0, ow, 0),
        D2D1::Matrix3x2F(-1, 0, 0, -1, ow, oh),
        D2D1::Matrix3x2F(0, -1, 1, 0, 0, oh)
    };
    SideLayout l;
    l.orientation = orientations[side];
    l.placement = D2D1::Matrix3x2F::Translation(
        a.gutterDip - static_cast<float>(piece.x) / f.deviceScale,
        a.gutterDip - static_cast<float>(piece.y) / f.deviceScale);
    l.length = side % 2 ? oh : ow;
    auto look = tile.spec.chart ? tile.spec.chart->appearance : knitting::ChartAppearance{};
    l.px = f.anchor == KnitAnchor::Centre ? l.length * 0.5f - tile.layout.width_dip * 0.5f : 0.f;
    l.py = look.outsideAnchored ? 0.f : f.bandDip * 0.5f - tile.layout.height_dip * 0.5f;
    bool fitted = (look.flags & designs::ChartFittedRepeat) != 0;
    l.repeatWidth = fitted ? l.length / std::max(1.f, std::round(l.length / tile.layout.width_dip))
        : static_cast<float>(tile.layout.width_dip);
    l.cap = std::min(f.radiusDip + f.bandDip, l.length * .5f);
    l.available = std::max(0.f, l.length - 2.f * l.cap);
    if (a.patch && l.available > 0.f) {
        l.repeatWidth = l.available / std::max(1.f, std::round(l.available / tile.layout.width_dip));
        l.px = l.cap;
    } else if (fitted) l.px = 0.f;
    if (fitted || a.patch)
        l.repeatWidth = std::max(1.f, std::round(l.repeatWidth * f.deviceScale)) / f.deviceScale;
    return l;
}

KnitShaderParameters ShaderParameters(const KnitAssembly& a, const knitting::Tile& tile,
                                      const KnitPiece& piece, int side) {
    const auto& f = a.frame;
    const auto l = Layout(a, tile, piece, side);
    auto material = tile.spec.material == knitting::Material::Raised
        ? ws_knit_raised_material(tile.spec.gauge) : tile.spec.gauge;
    bool hole = f.widthDip > 2.f * f.tuckDip && f.heightDip > 2.f * f.tuckDip;
    return {
        .ringWidth = f.widthDip + 2.f * f.bandDip, .ringHeight = f.heightDip + 2.f * f.bandDip,
        .outerRadius = f.radiusDip + f.bandDip, .inset = hole ? f.bandDip + f.tuckDip : 0.f,
        .innerRadius = std::max(0.f, f.radiusDip - f.tuckDip),
        .originX = static_cast<float>(piece.x) / f.deviceScale - a.gutterDip,
        .originY = static_cast<float>(piece.y) / f.deviceScale - a.gutterDip,
        .scale = f.deviceScale, .relief = material.relief, .band = f.bandDip,
        .pitch = l.repeatWidth / static_cast<float>(tile.layout.columns), .phase = l.px,
        .side = static_cast<std::uint32_t>(side), .inverseScale = 1.f / f.deviceScale};
}

KnitPiece SideBounds(const KnitFrameSpec& f, int side, std::uint32_t width, std::uint32_t height,
                           float depth, float gutterDip) {
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
    KnitPiece piece;
    piece.width = width; piece.height = height;
    if (side == 0) piece.height = std::min(dw, height);
    if (side == 1) { piece.x = dx; piece.width = width - dx; }
    if (side == 2) { piece.y = dy; piece.height = height - dy; }
    if (side == 3) piece.width = std::min(dw, width);
    return piece;
}


}

void DrawKnitStrip(ID2D1RenderTarget& target, const KnitAssembly& a, const knitting::Tile& tile,
                   const KnitPiece& piece, int side, bool heights, const KnitTileBrush& tileBrush) {
    const auto& f = a.frame;
    const auto l = Layout(a, tile, piece, side);
    const auto* patch = a.patch.get();
    const auto* cuff = a.cuff.get();
    const float depth = a.depth;
    ComPtr<ID2D1Factory> factory;
    target.GetFactory(&factory);
    auto ring = Ring(factory.Get(), f);
    // Aliased 45-degree edges through pixel centres rasterize differently in software and hardware D2D.
    auto mitre = Mitre(factory.Get(), l.length, depth, 1.f / (8.f * f.deviceScale));
    ComPtr<ID2D1Geometry> cuffRing;
    if (cuff) cuffRing = CuffRing(factory.Get(), f);
    auto brush = tileBrush(0, tile, heights, l.px, l.py, l.repeatWidth);
    ComPtr<ID2D1BitmapBrush> patchBrush, cuffBrush;
    if (patch) patchBrush = tileBrush(1, *patch, heights, 0, l.py, static_cast<float>(patch->layout.width_dip));
    if (cuff) cuffBrush = tileBrush(2, *cuff, heights, 0, 0, static_cast<float>(cuff->layout.width_dip));
    ComPtr<ID2D1SolidColorBrush> backing, black;
    if (heights) Check(target.CreateSolidColorBrush(D2D1::ColorF(.3f, .3f, .3f, 1.f), &backing),
        "Create stitch height backing");
    else {
        auto cornerColor = patch ? patch->spec.baseArgb : tile.spec.baseArgb;
        Check(target.CreateSolidColorBrush(knitting::detail::OpaqueColor(cornerColor, 0.96f), &backing),
              "Create opaque yarn backing");
        Check(target.CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, f.dim), &black), "Create dim overlay");
    }
    target.BeginDraw();
    target.Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    target.SetTransform(l.placement);
    target.PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), ring.Get()), nullptr);
    target.SetTransform(l.orientation * l.placement);
    target.PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mitre.Get(), D2D1_ANTIALIAS_MODE_ALIASED), nullptr);
    auto rectangle = D2D1::RectF(0.f, 0.f, l.length, depth);
    target.FillRectangle(rectangle, backing.Get());
    if (patch) {
        target.FillRectangle(D2D1::RectF(0, 0, l.cap, depth), patchBrush.Get());
        target.FillRectangle(D2D1::RectF(l.length - l.cap, 0, l.length, depth), patchBrush.Get());
        if (l.available > 0.f)
            target.FillRectangle(D2D1::RectF(l.cap, 0, l.length - l.cap, depth), brush.Get());
    } else target.FillRectangle(rectangle, brush.Get());
    if (cuff) {
        target.SetTransform(l.placement);
        target.PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), cuffRing.Get()), nullptr);
        target.SetTransform(l.orientation * l.placement);
        target.FillRectangle(rectangle, cuffBrush.Get());
        target.PopLayer();
    }
    if (!heights && f.dim > 0.001f)
        target.FillRectangle(D2D1::RectF(-depth, -depth, l.length + 2.f * depth, 3.f * depth), black.Get());
    target.PopLayer();
    target.PopLayer();
    Check(target.EndDraw(), heights ? "Render knit stitch height" : "Render knit perimeter side");
}

KnitPerimeter KnitPreparation::PreparePerimeter(const KnitFrameSpec& requested,
                                         knitting::TileSpec tileSpec, knitting::TileCache& cache,
                                         const KnitRenderControl& control) {
    auto f = Normalize(requested);
    float ow = f.widthDip + 2.f * f.bandDip, oh = f.heightDip + 2.f * f.bandDip;
    auto naturalInset = static_cast<std::uint32_t>(std::ceil(f.bandDip * f.deviceScale));
    auto inset = f.physicalPaddingPixels ? f.physicalPaddingPixels : naturalInset;
    if (inset + 1u < naturalInset || inset > naturalInset + 1u)
        throw std::invalid_argument("Authoritative physical padding differs by more than one pixel");
    const auto margin = geometry::ShadowMarginPixels(inset);
    double contentWidth = f.contentWidthPixels ? f.contentWidthPixels
        : std::ceil(static_cast<double>(f.widthDip) * f.deviceScale);
    double contentHeight = f.contentHeightPixels ? f.contentHeightPixels
        : std::ceil(static_cast<double>(f.heightDip) * f.deviceScale);
    double width = contentWidth + 2. * (inset + margin), height = contentHeight + 2. * (inset + margin);
    const auto dimensionLimit = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (width > dimensionLimit || height > dimensionLimit) throw std::invalid_argument("Frame dimensions exceed limits");
    float irad = std::max(0.f, f.radiusDip - f.tuckDip);
    float depth = f.bandDip + f.tuckDip + irad * (1.f - 0.7071067811865475244f) + 1.f;
    depth = std::min(depth, std::min(ow, oh) * 0.5f);
    double narrow = std::ceil(depth * f.deviceScale) + 2. + margin;
    if (2. * narrow * (width + height) > 16. * 1024. * 1024.)
        throw std::invalid_argument("Perimeter byte limit exceeded");
    tileSpec.bandDip = f.bandDip;
    KnitPerimeter perimeter;
    perimeter.width = static_cast<std::uint32_t>(width);
    perimeter.height = static_cast<std::uint32_t>(height);
    perimeter.contentInsetPixels = inset + margin;
    perimeter.shadowMarginPixels = margin;
    perimeter.bandDip = f.bandDip;
    perimeter.deviceScale = f.deviceScale;
    Poll(control);
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
    Poll(control);
    float gutterDip = static_cast<float>(inset + margin) / f.deviceScale - f.bandDip;
    KnitAssembly assembly{f, depth, gutterDip, std::move(patch), std::move(cuff), {}};
    for (int side = 0; side < 4; ++side) {
        Poll(control);
        auto& piece = perimeter.pieces[side];
        piece = SideBounds(f, side, perimeter.width, perimeter.height, depth, gutterDip);
        assembly.parameters[side] = ShaderParameters(assembly, *perimeter.tile, piece, side);
    }
    perimeter.assembly = std::move(assembly);
    Poll(control);
    return perimeter;
}

}
