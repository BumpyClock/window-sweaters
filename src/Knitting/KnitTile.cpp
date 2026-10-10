// SPDX-License-Identifier: GPL-3.0-only
// Raster adapter for upstream src/knit.c at a49dff52483dafdbe27f972bd271cd28d37cef1c.
#include "WicSurface.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace window_sweaters::knitting {
namespace detail {
void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), operation);
}

WicSurface::WicSurface(std::uint32_t width, std::uint32_t height, float scale, ID2D1Factory* sharedFactory,
                       IWICImagingFactory* sharedWic)
    : width_(width), height_(height) {
    if (!width || !height || width > 32768 || height > 32768
        || static_cast<std::uint64_t>(width) * height > 16u * 1024u * 1024u
        || !std::isfinite(scale) || scale < MinimumDeviceScale || scale > MaximumDeviceScale)
        throw std::invalid_argument("WIC surface dimensions/scale exceed limits");
    if (sharedWic) wic = sharedWic;
    else Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
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
    bool sameChart = a.chart == b.chart;
    if (!sameChart && a.chart && b.chart) {
        const auto& x = *a.chart; const auto& y = *b.chart;
        sameChart = x.semanticId == y.semanticId && x.contentRevision == y.contentRevision
            && x.width == y.width && x.height == y.height && x.appearance == y.appearance
            && x.contentHash == y.contentHash && x.argbCells == y.argbCells;
    }
    return a.bandDip == b.bandDip && a.baseArgb == b.baseArgb && a.stitch == b.stitch
        && a.material == b.material && a.chartRevision == b.chartRevision
        && a.deviceScale == b.deviceScale && a.rasterScale == b.rasterScale
        && a.sampling == b.sampling && GaugeFields(a.gauge) == GaugeFields(b.gauge) && sameChart;
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
                          RasterImage* diagnosticHeight, RasterImage* stitchHeight) {
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
    std::array<std::uint32_t, 16> colors{};
    std::size_t colorCount = 0;
    if (chartCells.empty()) colors[colorCount++] = spec.baseArgb;
    for (const auto& cell : chartCells) {
        auto end = colors.begin() + colorCount;
        if (std::find(colors.begin(), end, cell.argb) != end) continue;
        if (colorCount == colors.size())
            throw std::invalid_argument("Raised raster supports at most sixteen yarn colours");
        colors[colorCount++] = cell.argb;
    }
    detail::WicSurface colour(width, height, static_cast<float>(l.raster_scale));
    detail::WicSurface ridge(width, height, static_cast<float>(l.raster_scale), colour.factory.Get());
    struct YarnPath { std::uint32_t argb; ComPtr<ID2D1PathGeometry> path; };
    std::vector<YarnPath> paths;
    for (std::size_t index = 0; index < colorCount; ++index) {
        auto color = colors[index];
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        Check(colour.factory->CreatePathGeometry(&path), "Create stockinette path");
        Check(path->Open(&sink), "Open stockinette path");
        PathWriter writer{sink.Get()};
        if (chartCells.empty()) {
            auto output = ws_knit_path_writer{&writer, PathWriter::Move, PathWriter::Quad};
            if (!(raised ? ws_knit_emit_stockinette(&material, &l, 1, output)
                : ws_knit_emit_plain(&material, &l, static_cast<int>(spec.stitch), output)))
                throw std::invalid_argument("Invalid plain knit path");
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
    flipRows(flat);
    flipRows(heights);
    std::vector<float> hf(flat.pixels.size()), scratch(hf.size());
    for (std::size_t i = 0; i < hf.size(); ++i) hf[i] = (heights.pixels[i] & 255) / 255.f;
    ws_knit_blur(hf.data(), static_cast<int>(width), static_cast<int>(height), scratch.data());
    ws_knit_blur(hf.data(), static_cast<int>(width), static_cast<int>(height), scratch.data());
    (stitchHeight ? ws_knit_albedo : ws_knit_shade)(&material, raised, l.raster_scale, hf.data(),
        flat.pixels.data(), static_cast<int>(width), static_cast<int>(height), flat.pixels.data());
    flipRows(flat);
    if (stitchHeight) {
        *stitchHeight = {width, height, width * 4, std::vector<std::uint32_t>(hf.size())};
        for (std::size_t i = 0; i < hf.size(); ++i) {
            auto v = static_cast<std::uint32_t>(std::lround(std::clamp(hf[i], 0.f, 1.f) * 255.f));
            stitchHeight->pixels[i] = 0xff000000u | v << 16 | v << 8 | v;
        }
        flipRows(*stitchHeight);
    }
    return flat;
}
}

namespace {
std::size_t YarnCellCount(int columns, int rows) {
    if (columns <= 0 || rows <= 0 || columns > 4096 || rows > 4096)
        throw std::invalid_argument("Invalid chart yarn dimensions");
    auto width = static_cast<std::size_t>(columns) + 4;
    auto height = static_cast<std::size_t>(rows) + 4;
    if (height > std::numeric_limits<std::size_t>::max() / width)
        throw std::invalid_argument("Chart yarn path count overflows");
    auto count = width * height;
    if (count > 256u * 1024u)
        throw std::invalid_argument("Chart yarn path count exceeds CPU limit");
    return count;
}

ws_knit_layout ChartLayout(const TileSpec& spec, const ws_knit_gauge& material) {
    const auto& ch = *spec.chart;
    bool raised = spec.material == Material::Raised;
    int density = raised ? 1 : 2;
    float step = spec.bandDip / std::max(1.5f, material.rows) / density;
    float sw = step * material.aspect;
    int columns = static_cast<int>(ch.width) * density;
    int rows = static_cast<int>(ch.height) * density;
    float columnRepeats = std::max(1.f, std::ceil(8.f / (sw * columns)));
    float rowRepeats = std::max(1.f, std::ceil(8.f / (step * rows)));
    if (!std::isfinite(columnRepeats) || !std::isfinite(rowRepeats)
        || columnRepeats > 4096.f / columns || rowRepeats > 4096.f / rows)
        throw std::invalid_argument("Chart gauge exceeds repeat limits");
    columns *= static_cast<int>(columnRepeats);
    rows *= static_cast<int>(rowRepeats);
    int width = std::max(8, static_cast<int>(std::lround(sw * columns)));
    int height = std::max(8, static_cast<int>(std::lround(step * rows)));
    sw = static_cast<float>(width) / columns;
    step = static_cast<float>(height) / rows;
    int scale = spec.rasterScale ? spec.rasterScale : step < 4.f ? 4 : 2;
    if (columns > 4096 || rows > 4096 || width > 4096 / scale || height > 4096 / scale)
        throw std::invalid_argument("Chart tile exceeds raster limits");
    if (raised) YarnCellCount(columns, rows);
    return {columns, rows, width, height, scale, sw, step / (1.f - material.row_overlap),
        step, std::max(sw * material.yarn, raised ? .8f : .35f)};
}

RasterImage FabricTile(const TileSpec& spec, const ws_knit_layout& l, RasterImage& stitchHeight) {
    const auto& ch = *spec.chart;
    const auto& g = spec.gauge;
    bool defined = (ch.appearance.flags & designs::ChartDefinedYarn) != 0;
    bool dots = (ch.appearance.flags & designs::ChartRoundDots) != 0;
    std::uint32_t width = static_cast<std::uint32_t>(l.width_dip * l.raster_scale);
    std::uint32_t height = static_cast<std::uint32_t>(l.height_dip * l.raster_scale);
    RasterImage image{width, height, width * 4, {}};
    image.pixels.resize(static_cast<std::size_t>(width) * height);
    stitchHeight = image;
    float stitchScale = defined ? 2.f : 1.f;
    float relief = std::min(defined ? .30f : .25f,
        std::max(0.f, g.relief * (defined ? .32f : .20f)));
    for (std::uint32_t y = 0; y < height; ++y) {
        float sy = (y + .5f) / (l.raster_scale * l.row_step);
        float stitchY = sy / stitchScale;
        int row = static_cast<int>(std::floor(stitchY));
        float v = stitchY - row;
        for (std::uint32_t x = 0; x < width; ++x) {
            float sx = (x + .5f) / (l.raster_scale * l.stitch_width);
            int column = static_cast<int>(std::floor(sx));
            float stitchX = sx / stitchScale;
            int yarnColumn = static_cast<int>(std::floor(stitchX));
            float u = stitchX - yarnColumn;
            float warp = dots ? 0.f : .16f * (1.f - 2.f * std::abs(2.f * u - 1.f));
            int colorRow = static_cast<int>(std::floor(sy + warp));
            int chartRows = static_cast<int>(ch.height) * 2;
            colorRow = (colorRow % chartRows + chartRows) % chartRows;
            auto cx = static_cast<std::uint32_t>(column / 2) % ch.width;
            auto cy = static_cast<std::uint32_t>(colorRow / 2);
            auto yarn = ch.argbCells[static_cast<std::size_t>(cy) * ch.width + cx];
            if (dots && (yarn >> 24) >= 128) {
                bool left = ch.argbCells[static_cast<std::size_t>(cy) * ch.width
                    + (cx + ch.width - 1) % ch.width] == yarn;
                bool above = ch.argbCells[static_cast<std::size_t>((cy + ch.height - 1)
                    % ch.height) * ch.width + cx] == yarn;
                float dx = sx * .5f - std::floor(sx * .5f) + (left ? 1.f : 0.f) - 1.f;
                float dy = sy * .5f - std::floor(sy * .5f) + (above ? 1.f : 0.f) - 1.f;
                if (dx * dx + dy * dy > .94f) yarn = spec.baseArgb;
            }
            if ((yarn >> 24) < 128) yarn = spec.baseArgb;
            auto seed = ws_knit_mix(static_cast<std::uint32_t>(yarnColumn) * 73856093u
                ^ static_cast<std::uint32_t>(row) * 19349663u);
            float wobble = ((seed & 255u) / 255.f - .5f) * g.jitter;
            float leg = .42f * (1.f - v) + .08f * v * (1.f - v);
            float distance = std::abs(std::abs(u - .5f - wobble) - leg);
            float radius = std::max(.10f, g.yarn * .5f);
            float ridge = std::max(0.f, 1.f - distance * distance / (radius * radius));
            auto grain = ws_knit_mix(x * 73856093u ^ y * 19349663u);
            float fibre = ((grain & 255u) / 255.f - .5f) * .014f;
            float shade = std::max(0.f, g.ground - relief * (1.f - ridge)
                + (g.ambient - .94f) + g.sheen * .08f * (.5f - u) + fibre);
            auto r = static_cast<std::uint32_t>(std::min(255.f, ((yarn >> 16) & 255u) * shade));
            auto green = static_cast<std::uint32_t>(std::min(255.f, ((yarn >> 8) & 255u) * shade));
            auto b = static_cast<std::uint32_t>(std::min(255.f, (yarn & 255u) * shade));
            image.pixels[static_cast<std::size_t>(y) * width + x] = 0xff000000u | r << 16 | green << 8 | b;
            auto hv = static_cast<std::uint32_t>(std::lround(std::min(1.f, ridge) * 255.f));
            stitchHeight.pixels[static_cast<std::size_t>(y) * width + x] = 0xff000000u | hv << 16 | hv << 8 | hv;
        }
    }
    return image;
}

std::shared_ptr<const Tile> MakeTile(const TileSpec& spec) {
    static std::atomic<std::uint64_t> nextSerial{0};
    auto tile = std::make_shared<Tile>();
    tile->serial = ++nextSerial;
    tile->spec = spec;
    auto material = spec.material == Material::Raised ? ws_knit_raised_material(spec.gauge) : spec.gauge;
    if (spec.chart) tile->layout = ChartLayout(spec, material);
    else if (!ws_knit_make_layout(&material, spec.bandDip, spec.rasterScale, &tile->layout))
        throw std::invalid_argument("Invalid knit tile layout");
    if (spec.material == Material::Raised)
        tile->layout.yarn_width = std::max(tile->layout.stitch_width * material.yarn, 0.8f);
    if (spec.chart && spec.material == Material::Plain) {
        tile->image = FabricTile(spec, tile->layout, tile->height);
        return tile;
    }
    if (!spec.chart && spec.material == Material::Plain && spec.stitch == Stitch::Rib)
        tile->layout.yarn_width = tile->layout.stitch_width * .55f;
    std::vector<detail::YarnCell> cells;
    if (spec.chart) {
        auto count = YarnCellCount(tile->layout.columns, tile->layout.rows);
        if (count > cells.max_size())
            throw std::invalid_argument("Chart yarn cell allocation exceeds vector limit");
        cells.reserve(count);
        const auto& ch = *spec.chart;
        for (int j = -2; j <= tile->layout.rows + 1; ++j)
            for (int i = -2; i <= tile->layout.columns + 1; ++i) {
                int cx = (i % static_cast<int>(ch.width) + ch.width) % ch.width;
                int cy = (j % static_cast<int>(ch.height) + ch.height) % ch.height;
                auto argb = ch.argbCells[static_cast<std::size_t>(cy) * ch.width + cx];
                cells.push_back({i, j, (argb >> 24) < 128 ? spec.baseArgb : argb | 0xff000000u});
            }
    }
    tile->image = detail::RasterizeTile(spec, tile->layout, cells, nullptr, &tile->height);
    return tile;
}
}

TileSpec NormalizeTileSpec(TileSpec spec) {
    if (!ws_knit_validate_gauge(&spec.gauge) || !std::isfinite(spec.bandDip)
        || spec.bandDip > 256.f || !std::isfinite(spec.deviceScale)
        || spec.deviceScale < MinimumDeviceScale || spec.deviceScale > MaximumDeviceScale
        || spec.rasterScale < 0 || spec.rasterScale > 8
        || (spec.stitch != Stitch::Stockinette && spec.stitch != Stitch::Rib && spec.stitch != Stitch::Garter)
        || (spec.material != Material::Plain && spec.material != Material::Raised)
        || (spec.sampling != Sampling::Nearest && spec.sampling != Sampling::Linear))
        throw std::invalid_argument("Invalid or unsupported tile specification");
    spec.bandDip = std::round(std::max(2.f, spec.bandDip) * 2.f) / 2.f;
    spec.baseArgb |= 0xff000000u;
    if (spec.chart) {
        spec.stitch = Stitch::Stockinette;
        spec.material = (spec.chart->appearance.flags & designs::ChartSculptedYarn)
            ? Material::Raised : Material::Plain;
    }
    return spec;
}

TileCache::TileCache(std::size_t byteBudget) : budget_(byteBudget) {}

std::shared_ptr<const Tile> TileCache::Get(const TileSpec& requested) {
    auto spec = NormalizeTileSpec(requested);
    std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (SameSpec(it->tile->spec, spec)) {
            ++hits_;
            auto tile = it->tile;
            entries_.splice(entries_.begin(), entries_, it);
            return tile;
        }
    }
    auto tile = MakeTile(spec);
    ++generations_;
    std::size_t bytes = tile->ByteSize();
    if (bytes > budget_) return tile;
    while (resident_ > budget_ - bytes) {
        resident_ -= entries_.back().tile->ByteSize();
        entries_.pop_back();
    }
    entries_.push_front({tile});
    resident_ += bytes;
    return tile;
}

void TileCache::Clear() { std::lock_guard lock(mutex_); entries_.clear(); resident_ = 0; }
std::size_t TileCache::ResidentBytes() const { std::lock_guard lock(mutex_); return resident_; }
std::uint64_t TileCache::GenerationCount() const { std::lock_guard lock(mutex_); return generations_; }
std::uint64_t TileCache::HitCount() const { std::lock_guard lock(mutex_); return hits_; }

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
