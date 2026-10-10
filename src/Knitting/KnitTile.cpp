// SPDX-License-Identifier: GPL-3.0-only
// Raster adapter for upstream src/knit.c at a49dff52483dafdbe27f972bd271cd28d37cef1c.
#include "WicSurface.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace window_sweaters::knitting {
namespace detail {
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), operation);
}

WicSurface::WicSurface(std::uint32_t width, std::uint32_t height, float scale, ID2D1Factory* sharedFactory)
    : width_(width), height_(height) {
    if (!width || !height || width > 32768 || height > 32768
        || static_cast<std::uint64_t>(width) * height > 16u * 1024u * 1024u
        || !std::isfinite(scale) || scale < MinimumDeviceScale || scale > MaximumDeviceScale)
        throw std::invalid_argument("WIC surface dimensions/scale exceed limits");
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&wic)), "Create WIC factory");
    if (sharedFactory) factory = sharedFactory;
    else Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()), "Create D2D factory");
    Check(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                           WICBitmapCacheOnLoad, &bitmap), "Create WIC bitmap");
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * scale, 96.f * scale);
    Check(factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target), "Create software render target");
}

RasterImage WicSurface::Read() const {
    RasterImage image{width_, height_, width_ * 4, {}};
    image.pixels.resize(static_cast<std::size_t>(width_) * height_);
    Check(bitmap->CopyPixels(nullptr, image.stride, static_cast<UINT>(image.ByteSize()),
                             reinterpret_cast<BYTE*>(image.pixels.data())), "Read WIC pixels");
    return image;
}

D2D1_COLOR_F OpaqueColor(std::uint32_t p, float f) {
    return D2D1::ColorF(std::min(1.f, ((p >> 16) & 255) / 255.f * f),
                       std::min(1.f, ((p >> 8) & 255) / 255.f * f),
                       std::min(1.f, (p & 255) / 255.f * f), 1.f);
}
}

namespace {
using detail::Check;
using detail::ComPtr;

std::array<float, 14> GaugeFields(const ws_knit_gauge& g) {
    return {g.rows, g.aspect, g.row_overlap, g.yarn, g.bow, g.jitter, g.ground,
            g.shadow, g.light, g.ambient, g.depth, g.relief, g.sheen, g.tuck};
}

bool SameSpec(const TileSpec& a, const TileSpec& b) {
    return a.bandDip == b.bandDip && a.baseArgb == b.baseArgb && a.stitch == b.stitch
        && a.material == b.material && a.chartRevision == b.chartRevision
        && a.deviceScale == b.deviceScale && a.rasterScale == b.rasterScale
        && a.sampling == b.sampling && GaugeFields(a.gauge) == GaugeFields(b.gauge);
}

struct PathWriter {
    ID2D1GeometrySink* sink;
    bool open = false;
    static void Move(void* context, float x, float y) {
        auto& self = *static_cast<PathWriter*>(context);
        if (self.open) self.sink->EndFigure(D2D1_FIGURE_END_OPEN);
        self.sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
        self.open = true;
    }
    static void Quad(void* context, float cx, float cy, float x, float y) {
        static_cast<PathWriter*>(context)->sink->AddQuadraticBezier(
            D2D1::QuadraticBezierSegment(D2D1::Point2F(cx, cy), D2D1::Point2F(x, y)));
    }
};

}

namespace detail {
RasterImage RasterizeTile(const TileSpec& spec, const ws_knit_layout& l,
                          const std::vector<YarnCell>& chartCells,
                          RasterImage* diagnosticHeight) {
    if (!ws_knit_validate_gauge(&spec.gauge) || l.columns <= 0 || l.rows <= 0
        || l.columns > 4096 || l.rows > 4096 || l.raster_scale <= 0 || l.raster_scale > 8
        || l.width_dip <= 0 || l.height_dip <= 0 || l.width_dip > 4096 / l.raster_scale
        || l.height_dip > 4096 / l.raster_scale || !std::isfinite(l.stitch_width)
        || !std::isfinite(l.stitch_height) || !std::isfinite(l.row_step)
        || !std::isfinite(l.yarn_width) || l.stitch_width <= 0 || l.stitch_height <= 0
        || l.row_step <= 0 || l.yarn_width <= 0)
        throw std::invalid_argument("Invalid native raster layout");
    bool raised = spec.material == Material::Raised;
    auto material = raised ? ws_knit_raised_material(spec.gauge) : spec.gauge;
    auto width = static_cast<std::uint32_t>(l.width_dip * l.raster_scale);
    auto height = static_cast<std::uint32_t>(l.height_dip * l.raster_scale);
    detail::WicSurface colour(width, height, static_cast<float>(l.raster_scale));
    detail::WicSurface ridge(width, height, static_cast<float>(l.raster_scale), colour.factory.Get());
    struct YarnPath { std::uint32_t argb; ComPtr<ID2D1PathGeometry> path; };
    std::vector<YarnPath> paths;
    std::vector<std::uint32_t> colors;
    if (chartCells.empty()) colors.push_back(spec.baseArgb);
    for (const auto& cell : chartCells)
        if (std::find(colors.begin(), colors.end(), cell.argb) == colors.end()) colors.push_back(cell.argb);
    if (colors.size() > 16) throw std::invalid_argument("Raised raster supports at most sixteen yarn colours");
    for (auto color : colors) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        Check(colour.factory->CreatePathGeometry(&path), "Create stockinette path");
        Check(path->Open(&sink), "Open stockinette path");
        PathWriter writer{sink.Get()};
        if (chartCells.empty()) {
            if (!ws_knit_emit_stockinette(&material, &l, raised, {&writer, PathWriter::Move, PathWriter::Quad}))
                throw std::invalid_argument("Invalid stockinette path");
        } else {
            for (const auto& cell : chartCells)
                if (cell.argb == color && !ws_knit_emit_cell(&material, &l, cell.column, cell.row,
                        {&writer, PathWriter::Move, PathWriter::Quad}))
                    throw std::invalid_argument("Invalid chart cell path");
        }
        if (writer.open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        Check(sink->Close(), "Close stockinette path");
        paths.push_back({color, path});
    }
    ComPtr<ID2D1StrokeStyle> stroke;
    auto strokeProperties = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    Check(colour.factory->CreateStrokeStyle(strokeProperties, nullptr, 0, &stroke), "Create round yarn stroke");

    ComPtr<ID2D1SolidColorBrush> yarn;
    Check(colour.target->CreateSolidColorBrush(detail::OpaqueColor(spec.baseArgb), &yarn), "Create yarn colour");
    colour.target->BeginDraw();
    colour.target->Clear(detail::OpaqueColor(spec.baseArgb, material.ground));
    for (const auto& path : paths) {
        yarn->SetColor(detail::OpaqueColor(path.argb));
        colour.target->DrawGeometry(path.path.Get(), yarn.Get(), l.yarn_width, stroke.Get());
    }
    Check(colour.target->EndDraw(), "Draw flat yarn");

    ComPtr<ID2D1SolidColorBrush> gray;
    Check(ridge.target->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &gray), "Create height brush");
    ridge.target->BeginDraw();
    ridge.target->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 1.f));
    for (const auto& path : paths)
        for (int i = 0; i < 5; ++i) {
            float v = ws_knit_ridge_heights[i];
            gray->SetColor(D2D1::ColorF(v, v, v, 1.f));
            ridge.target->DrawGeometry(path.path.Get(), gray.Get(), l.yarn_width * ws_knit_ridge_widths[i], stroke.Get());
        }
    Check(ridge.target->EndDraw(), "Draw five yarn ridges");
    auto flat = colour.Read(), heights = ridge.Read();
    if (diagnosticHeight) *diagnosticHeight = heights;
    auto flipRows = [](RasterImage& image) {
        for (std::uint32_t y = 0; y < image.height / 2; ++y)
            for (std::uint32_t x = 0; x < image.width; ++x)
                std::swap(image.pixels[static_cast<std::size_t>(y)*image.width+x],
                    image.pixels[static_cast<std::size_t>(image.height-1-y)*image.width+x]);
    };
    // CoreGraphics's shader buffers are bottom-up, unlike the top-down WIC
    // surface. Preserve its dy/light and grain coordinates, then return BGRA
    // top-down output. Flipping only the displayed path changes the stitches.
    flipRows(flat);
    flipRows(heights);
    std::vector<float> hf(flat.pixels.size()), scratch(hf.size());
    for (std::size_t i = 0; i < hf.size(); ++i) hf[i] = (heights.pixels[i] & 255) / 255.f;
    ws_knit_blur(hf.data(), static_cast<int>(width), static_cast<int>(height), scratch.data());
    ws_knit_blur(hf.data(), static_cast<int>(width), static_cast<int>(height), scratch.data());
    ws_knit_shade(&material, raised, l.raster_scale, hf.data(), flat.pixels.data(),
                  static_cast<int>(width), static_cast<int>(height), flat.pixels.data());
    flipRows(flat);
    return flat;
}
}

namespace {
std::shared_ptr<const Tile> MakeTile(const TileSpec& spec) {
    auto tile = std::make_shared<Tile>();
    tile->spec = spec;
    auto material = spec.material == Material::Raised ? ws_knit_raised_material(spec.gauge) : spec.gauge;
    if (!ws_knit_make_layout(&material, spec.bandDip, spec.rasterScale, &tile->layout))
        throw std::invalid_argument("Invalid knit tile layout");
    if (spec.material == Material::Raised)
        tile->layout.yarn_width = std::max(tile->layout.stitch_width * material.yarn, 0.8f);
    tile->image = detail::RasterizeTile(spec, tile->layout);
    return tile;
}
}

TileSpec NormalizeTileSpec(TileSpec spec) {
    if (!ws_knit_validate_gauge(&spec.gauge) || !std::isfinite(spec.bandDip)
        || spec.bandDip > 256.f || !std::isfinite(spec.deviceScale)
        || spec.deviceScale < MinimumDeviceScale || spec.deviceScale > MaximumDeviceScale
        || spec.rasterScale < 0 || spec.rasterScale > 8
        || spec.stitch != Stitch::Stockinette
        || (spec.material != Material::Plain && spec.material != Material::Raised)
        || (spec.sampling != Sampling::Nearest && spec.sampling != Sampling::Linear))
        throw std::invalid_argument("Invalid or unsupported tile specification");
    spec.bandDip = std::round(std::max(2.f, spec.bandDip) * 2.f) / 2.f;
    spec.baseArgb |= 0xff000000u;
    return spec;
}

TileCache::TileCache(std::size_t byteBudget) : budget_(byteBudget) {}

std::shared_ptr<const Tile> TileCache::Get(const TileSpec& requested) {
    auto spec = NormalizeTileSpec(requested);
    std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (SameSpec(it->tile->spec, spec)) {
            auto tile = it->tile;
            entries_.splice(entries_.begin(), entries_, it);
            return tile;
        }
    }
    auto tile = MakeTile(spec);
    ++generations_;
    std::size_t bytes = tile->image.ByteSize();
    if (bytes > budget_) return tile;
    while (resident_ > budget_ - bytes) {
        resident_ -= entries_.back().tile->image.ByteSize();
        entries_.pop_back();
    }
    entries_.push_front({tile});
    resident_ += bytes;
    return tile;
}

void TileCache::Clear() { std::lock_guard lock(mutex_); entries_.clear(); resident_ = 0; }
std::size_t TileCache::ResidentBytes() const { std::lock_guard lock(mutex_); return resident_; }
std::uint64_t TileCache::GenerationCount() const { std::lock_guard lock(mutex_); return generations_; }

void SavePng(const RasterImage& image, const wchar_t* path) {
    if (!path || !*path || !image.width || !image.height || image.stride != image.width * 4u
        || image.pixels.size() != static_cast<std::size_t>(image.width) * image.height
        || image.ByteSize() > std::numeric_limits<UINT>::max())
        throw std::invalid_argument("Invalid PNG image/path");
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "Create PNG WIC factory");
    Check(wic->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppPBGRA,
        image.stride, static_cast<UINT>(image.ByteSize()),
        reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(image.pixels.data())), &bitmap), "Create PNG source");
    Check(wic->CreateFormatConverter(&converter), "Create PNG converter");
    Check(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0, WICBitmapPaletteTypeCustom), "Unpremultiply PNG source");
    Check(wic->CreateStream(&stream), "Create PNG stream");
    Check(stream->InitializeFromFilename(path, GENERIC_WRITE), "Open PNG output");
    Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
    Check(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
    Check(frame->Initialize(nullptr), "Initialize PNG frame");
    Check(frame->SetSize(image.width, image.height), "Set PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&format), "Set PNG pixel format");
    Check(frame->WriteSource(converter.Get(), nullptr), "Write PNG pixels");
    Check(frame->Commit(), "Commit PNG frame");
    Check(encoder->Commit(), "Commit PNG");
}
}
