// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Rendering\KnitRaster.h"
#include "..\..\src\Knitting\WicSurface.h"
#include "..\..\src\Designs\Catalogue.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <crtdbg.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

using namespace window_sweaters::knitting;
using namespace window_sweaters::rendering;
using window_sweaters::knitting::detail::Check;
using window_sweaters::knitting::detail::ComPtr;

namespace {
int checks = 0;
void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void Invalid(const std::function<void()>& operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, message);
}
bool Near(float a, float b, float tolerance = 0.00001f) { return std::abs(a - b) <= tolerance; }
std::uint32_t Pixel(const RasterImage& image, std::uint32_t x, std::uint32_t y) {
    return image.pixels.at(static_cast<std::size_t>(y) * image.width + x);
}
std::uint32_t Alpha(std::uint32_t p) { return p >> 24; }
void Premultiplied(const RasterImage& image) {
    bool okay = true, partial = false;
    for (auto p : image.pixels) {
        auto alpha = Alpha(p);
        if ((p & 255) > alpha || ((p >> 8) & 255) > alpha || ((p >> 16) & 255) > alpha) okay = false;
        if (alpha > 0 && alpha < 255) partial = true;
    }
    Require(okay, "BGRA channels must be premultiplied");
    Require(partial, "Rounded ring must have antialiased alpha boundaries");
}

RasterImage LoadPng(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)), "Create test PNG factory");
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder), "Decode reference PNG");
    Check(decoder->GetFrame(0, &frame), "Read reference PNG frame");
    Check(factory->CreateFormatConverter(&converter), "Create reference converter");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Convert reference PNG");
    RasterImage image;
    Check(converter->GetSize(&image.width, &image.height), "Read reference dimensions");
    image.stride = image.width * 4;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
    Check(converter->CopyPixels(nullptr, image.stride, static_cast<UINT>(image.ByteSize()),
        reinterpret_cast<BYTE*>(image.pixels.data())), "Read reference pixels");
    return image;
}

struct RecordedPath {
    std::vector<std::array<float, 4>> curves;
    std::vector<std::array<float, 2>> moves;
    static void Move(void* context, float x, float y) {
        static_cast<RecordedPath*>(context)->moves.push_back({x, y});
    }
    static void Quad(void* context, float cx, float cy, float x, float y) {
        static_cast<RecordedPath*>(context)->curves.push_back({cx, cy, x, y});
    }
    ws_knit_path_writer Writer() { return {this, Move, Quad}; }
};

void PortableMath() {
    auto gauge = ws_knit_default_gauge();
    ws_knit_layout layout{};
    Require(ws_knit_make_layout(&gauge, 10, 0, &layout) == 1, "Default layout valid");
    Require(layout.width_dip == 18 && layout.height_dip == 13 && layout.raster_scale == 4,
            "Pinned plain gauge derives 18x13 DIP at 4x");
    Require(Near(layout.stitch_width, 2.25f) && Near(layout.row_step, 1.625f)
            && Near(layout.yarn_width, 1.08f), "Pinned plain stitch dimensions");
    Require(ws_knit_mix(0) == 0 && ws_knit_mix(1) == 0x514e28b7u, "Murmur3 finalizer golden values");
    float noise = ws_knit_noise(3, 4);
    Require(noise >= 0.f && noise < 1.f && noise == ws_knit_noise(3, 4), "Deterministic sine noise");
    RecordedPath plain;
    Require(ws_knit_emit_stockinette(&gauge, &layout, 0, plain.Writer()) == 1, "Emit plain path");
    Require(plain.moves.size() == 12 && plain.curves.size() == 288, "Continuous stockinette rows and margins");
    const auto& first = plain.curves[52];
    const auto& nextRepeat = plain.curves[68];
    Require(Near(nextRepeat[0] - first[0], 18.f) && Near(nextRepeat[1], first[1])
        && Near(nextRepeat[2] - first[2], 18.f) && Near(nextRepeat[3], first[3]),
        "Sine-wobbled stitch path repeats exactly across the tile");
    auto raised = ws_knit_raised_material(gauge);
    Require(ws_knit_ridge_widths[0] == 1.f && ws_knit_ridge_widths[1] == .8f
        && ws_knit_ridge_widths[2] == .6f && ws_knit_ridge_widths[3] == .4f
        && ws_knit_ridge_widths[4] == .2f && ws_knit_ridge_heights[0] == .22f
        && ws_knit_ridge_heights[1] == .48f && ws_knit_ridge_heights[2] == .70f
        && ws_knit_ridge_heights[3] == .88f && ws_knit_ridge_heights[4] == 1.f,
        "Pinned five nested ridge strokes");
    Require(ws_knit_make_layout(&raised, 10, 4, &layout) == 1, "Raised layout valid");
    RecordedPath cell;
    Require(ws_knit_emit_cell(&raised, &layout, 0, 0, cell.Writer()) == 1, "Emit real chart cell primitive");
    Require(cell.moves.size() == 1 && cell.moves[0] == std::array<float, 2>{0, 0}, "Chart cell orientation top down");
    Require(Near(cell.curves[0][0], 0.315f) && Near(cell.curves[0][1], 0.94078947f)
            && Near(cell.curves[0][2], 1.125f) && Near(cell.curves[0][3], 1.71052632f),
            "Pinned upstream quadratic left leg");
    Require(Near(cell.curves[1][0], 1.935f) && Near(cell.curves[1][1], 0.94078947f)
            && Near(cell.curves[1][2], 2.25f) && Near(cell.curves[1][3], 0.f),
            "Pinned upstream quadratic right leg");
    auto chartLayout = layout; chartLayout.columns = 12; chartLayout.rows = 6;
    RecordedPath chartCell;
    Require(ws_knit_emit_cell(&raised, &chartLayout, 11, 5, chartCell.Writer()) == 1,
        "Raised cell primitive accepts authored chart repeat counts without fake themes");
    std::array<float, 9> height = {1,0,0,0,0,0,0,0,0}, scratch{};
    Require(ws_knit_blur(height.data(), 3, 3, scratch.data()) == 1, "Wrapped blur valid");
    const std::array<float, 9> expected = {.25f,.125f,.125f,.125f,.0625f,.0625f,.125f,.0625f,.0625f};
    Require(height == expected, "Wrapping separable [1,2,1]/4 blur golden impulse");
    Require(ws_knit_blur(height.data(), 3, 3, scratch.data()) == 1, "Second wrapping blur valid");
    Require(Near(std::accumulate(height.begin(), height.end(), 0.f), 1.f), "Blur preserves energy");
    std::array<std::uint32_t, 9> black{}, lit{};
    Require(ws_knit_shade(&raised, 1, 4, height.data(), black.data(), 3, 3, lit.data()) == 1, "Dark raised yarn valid");
    Require((lit[0] & 255) > 0 && Alpha(lit[0]) == 255, "Very dark yarn has upstream lift and opaque output");
    height.fill(.5f);
    black.fill(0xff646464u);
    Require(ws_knit_shade(&raised, 1, 4, height.data(), black.data(), 3, 3, lit.data()) == 1
        && lit[0] == 0xff5a5a5au, "Pinned uniform-normal lighting golden pixel");
    black.fill(0xff000000u);
    Require(ws_knit_shade(&raised, 1, 4, height.data(), black.data(), 3, 3, lit.data()) == 1
        && lit[0] == 0xff050505u, "Pinned dark-yarn lift golden pixel");
    height[0] = std::numeric_limits<float>::quiet_NaN();
    Require(ws_knit_shade(&raised, 1, 4, height.data(), black.data(), 3, 3, lit.data()) == 0,
        "NaN height cannot reach float-to-byte conversion");
    Require(ws_knit_blur(nullptr, 1, 1, scratch.data()) == 0, "Null blur rejected");
    Require(ws_knit_blur(height.data(), 1, 1, height.data()) == 0, "Aliased blur scratch rejected");
    Require(ws_knit_make_layout(&gauge, std::numeric_limits<float>::infinity(), 0, &layout) == 0,
            "Infinite layout rejected");
    gauge.row_overlap = 1;
    Require(ws_knit_validate_gauge(&gauge) == 0, "Degenerate row overlap rejected");
}

void CacheAndInvalidInputs() {
    TileSpec spec;
    TileCache probe;
    auto tile = probe.Get(spec);
    Require(tile->image.width == 72 && tile->image.height == 52, "Physical tile dimensions reflect sampling scale");
    Require(std::all_of(tile->image.pixels.begin(), tile->image.pixels.end(),
                       [](auto p) { return Alpha(p) == 255; }), "Yarn tile always opaque");
    auto [minimum, maximum] = std::minmax_element(tile->image.pixels.begin(), tile->image.pixels.end());
    Require((*maximum & 255) - (*minimum & 255) > 8, "Tile contains real raised relief, not flat colour");
    Require(probe.Get(spec) == tile && probe.GenerationCount() == 1, "Same key shares immutable tile");
    auto half = spec; half.bandDip = 10.1f;
    Require(probe.Get(half) == tile, "Pinned half-DIP tile quantization");
    auto opaque = spec; opaque.baseArgb &= 0x00ffffffu;
    Require(probe.Get(opaque) == tile, "ARGB input alpha cannot make yarn translucent");
    auto originalPixels = tile->image.pixels;
    TileCache bounded(tile->image.ByteSize());
    auto inUse = bounded.Get(spec);
    auto another = spec; another.baseArgb = 0xffd1495bu;
    bounded.Get(another);
    Require(bounded.ResidentBytes() <= tile->image.ByteSize(), "Cache image bytes stay bounded");
    Require(inUse->image.pixels == originalPixels, "Tile survives eviction while in use");
    bounded.Clear();
    Require(bounded.ResidentBytes() == 0 && inUse->image.pixels == originalPixels, "Tile survives explicit cache clear");
    TileCache uncached(0);
    uncached.Get(spec);
    Require(uncached.ResidentBytes() == 0, "Oversize tile bypasses cache without violating bound");
    auto change = [&](TileSpec changed) {
        auto previous = probe.GenerationCount();
        probe.Get(changed);
        Require(probe.GenerationCount() == previous + 1, "Complete cache key invalidates");
    };
    auto changed = spec; changed.deviceScale = 1.25f; change(changed);
    changed = spec; changed.chartRevision = 42; change(changed);
    changed = spec; changed.rasterScale = 2; change(changed);
    changed = spec; changed.sampling = Sampling::Nearest; change(changed);
    changed = spec; changed.material = Material::Raised; change(changed);
    const std::array<float ws_knit_gauge::*, 14> fields = {
        &ws_knit_gauge::rows, &ws_knit_gauge::aspect, &ws_knit_gauge::row_overlap,
        &ws_knit_gauge::yarn, &ws_knit_gauge::bow, &ws_knit_gauge::jitter,
        &ws_knit_gauge::ground, &ws_knit_gauge::shadow, &ws_knit_gauge::light,
        &ws_knit_gauge::ambient, &ws_knit_gauge::depth, &ws_knit_gauge::relief,
        &ws_knit_gauge::sheen, &ws_knit_gauge::tuck};
    for (auto field : fields) { changed = spec; changed.gauge.*field += .001f; change(changed); }
    changed = spec; changed.stitch = Stitch::Rib;
    Invalid([&] { probe.Get(changed); }, "Unported stitch must not masquerade as stockinette");
    changed = spec; changed.gauge.jitter = std::numeric_limits<float>::quiet_NaN();
    Invalid([&] { probe.Get(changed); }, "NaN gauge rejected");
    changed = spec; changed.deviceScale = 0;
    Invalid([&] { probe.Get(changed); }, "Zero scale rejected");
    changed = spec; changed.deviceScale = 1e-38f;
    Invalid([&] { probe.Get(changed); }, "Positive subnormal-scale tile specification rejected");
    changed = spec; changed.rasterScale = 9;
    Invalid([&] { probe.Get(changed); }, "Excess sampling rejected");
    changed = spec; changed.bandDip = 257;
    Invalid([&] { probe.Get(changed); }, "Excess band rejected");
    changed = spec; changed.gauge.aspect = 8; changed.gauge.rows = .1f; changed.bandDip = 256;
    Invalid([&] { probe.Get(changed); }, "Excess tile allocation rejected");
}

bool InsideRounded(float x, float y, float left, float top, float width, float height, float radius) {
    if (x < left || y < top || x >= left + width || y >= top + height) return false;
    radius = std::clamp(radius, 0.f, std::min(width, height) * .5f);
    float mx = left + width * .5f, my = top + height * .5f;
    float hx = std::max(0.f, width * .5f - radius), hy = std::max(0.f, height * .5f - radius);
    // At a capsule end hx/hy can be zero. Centre +/- a nonnegative extent
    // cannot reverse due to cancellation of large origins and tiny radii.
    float cx = std::clamp(x, mx - hx, mx + hx);
    float cy = std::clamp(y, my - hy, my + hy);
    return (x-cx)*(x-cx) + (y-cy)*(y-cy) <= radius*radius;
}

void ReferenceGeometry(const RasterImage& image, const std::filesystem::path& root) {
    auto reference = LoadPng(root / L"references" / L"app-05.png");
    Require(reference.width == 1440 && reference.height == 1060, "Pinned renderer plate dimensions");
    std::size_t compared = 0, mismatch = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x) {
            float dipX = (x + .5f) / 2.f, dipY = (y + .5f) / 2.f;
            // Remove the white content (including its antialias boundary).
            // Crop contains no CoreText captions.
            if (InsideRounded(dipX, dipY, 9.5f, 9.5f, 621.f, 141.f, 10.5f)) continue;
            auto baseline = Pixel(reference, x + 76, y + 204);
            int backgroundDistance = std::abs(static_cast<int>((baseline >> 16) & 255) - 249)
                + std::abs(static_cast<int>((baseline >> 8) & 255) - 247)
                + std::abs(static_cast<int>(baseline & 255) - 243);
            bool upstreamYarn = backgroundDistance > 12;
            bool nativeYarn = Alpha(Pixel(image, x, y)) > 128;
            ++compared;
            if (upstreamYarn != nativeYarn) ++mismatch;
        }
    double fraction = static_cast<double>(mismatch) / static_cast<double>(compared);
    std::cout << "reference exterior geometry: " << mismatch << "/" << compared
              << " disagreements (" << fraction * 100. << "%), tolerance 1%\n";
    Require(fraction <= .01, "Pinned rounded exterior geometry within 1% raster tolerance");
}

struct StitchPitch { int pixels = 0; double correlation = -1; };
StitchPitch MeasureStitchPitch(const RasterImage& image, std::uint32_t ox,
                              std::uint32_t oy, std::uint32_t width, std::uint32_t height) {
    StitchPitch result;
    // Remove the authored green/butter palette, not its stitch detail.
    // Mixed-yarn colour boundaries are excluded from this texture-only metric.
    auto sample = [](std::uint32_t p) {
        double r = (p >> 16) & 255, g = (p >> 8) & 255;
        if (r < g * .75) return g / 118.;
        if (r > g * 1.02) return r / 229.;
        return -1.;
    };
    for (int lag = 12; lag <= 24; ++lag) {
        double covariance = 0, energyA = 0, energyB = 0;
        for (std::uint32_t y = 4; y < height - 4; ++y) {
            double mean = 0;
            std::size_t count = 0;
            for (std::uint32_t x = 0; x < width; ++x) {
                double v = sample(Pixel(image, ox+x, oy+y));
                if (v >= 0) { mean += v; ++count; }
            }
            if (!count) continue;
            mean /= static_cast<double>(count);
            for (std::uint32_t x = 0; x + lag < width; ++x) {
                double a = sample(Pixel(image, ox+x, oy+y));
                double b = sample(Pixel(image, ox+x+lag, oy+y));
                if (a < 0 || b < 0) continue;
                a -= mean; b -= mean;
                covariance += a*b; energyA += a*a; energyB += b*b;
            }
        }
        double correlation = energyA > 0 && energyB > 0
            ? covariance / std::sqrt(energyA*energyB) : -1.;
        if (correlation > result.correlation) result = {lag, correlation};
    }
    return result;
}

void ReferenceStitches(const RasterImage& nativeDetail, const std::filesystem::path& root) {
    auto reference = LoadPng(root / L"references" / L"app-05.png");
    auto baseline = MeasureStitchPitch(reference, 76, 602, 1280, 80);
    auto candidate = MeasureStitchPitch(nativeDetail, 0, 0, nativeDetail.width, nativeDetail.height);
    std::cout << "raised stitch pitch at 8 physical px/DIP: upstream=" << baseline.pixels
        << "px (correlation " << baseline.correlation << "), native=" << candidate.pixels
        << "px (correlation " << candidate.correlation << "), expected=18px, tolerance=1px\n";
    Require(std::abs(baseline.pixels - 18) <= 1 && std::abs(candidate.pixels - 18) <= 1,
            "Renderer plate and native yarn retain the pinned 2.25-DIP stitch pitch");
    Require(baseline.correlation > .5 && candidate.correlation > .5,
            "Reference and native stitch texture have real periodic detail, not flat bands");
}

RasterImage TileDetail(const RasterImage& tile, const ws_knit_layout& layout, float startX) {
    window_sweaters::knitting::detail::WicSurface surface(640, 80, 8);
    ComPtr<ID2D1Bitmap> bitmap;
    auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f * layout.raster_scale, 96.f * layout.raster_scale);
    Check(surface.target->CreateBitmap(D2D1::SizeU(tile.width, tile.height), tile.pixels.data(),
        tile.stride, properties, &bitmap), "Create diagnostic tile bitmap");
    ComPtr<ID2D1BitmapBrush> brush;
    Check(surface.target->CreateBitmapBrush(bitmap.Get(),
        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &brush), "Create diagnostic periodic brush");
    brush->SetTransform(D2D1::Matrix3x2F::Translation(-startX, 0));
    surface.target->BeginDraw();
    surface.target->Clear(D2D1::ColorF(0, 0, 0, 1));
    surface.target->FillRectangle(D2D1::RectF(0, 0, 80, 10), brush.Get());
    Check(surface.target->EndDraw(), "Render diagnostic detail");
    return surface.Read();
}

void SourceTextureDiagnosis(const std::filesystem::path& root) {
    using namespace window_sweaters::designs;
    auto chart = ws_design_chart_by_name("atelier-spotify");
    Require(chart && chart->width == 12 && chart->height == 6, "Diagnostic uses actual authored Spotify repeat");
    TileSpec spec;
    spec.material = Material::Raised; spec.baseArgb = 0xff497641u;
    auto material = ws_knit_raised_material(spec.gauge);
    ws_knit_layout layout{12, 6, 27, 10, 4, 2.25f, (10.f/6) / (1.f-material.row_overlap),
        10.f/6, .9f};
    std::vector<window_sweaters::knitting::detail::YarnCell> cells;
    for (int y = -2; y <= 7; ++y)
        for (int x = -2; x <= 13; ++x) {
            auto contrast = ws_design_cell_argb(chart, static_cast<std::uint32_t>((x+12)%12),
                static_cast<std::uint32_t>((y+6)%6));
            cells.push_back({x, y, contrast ? contrast : spec.baseArgb});
        }
    auto output = root / L"outputs";
    RasterImage height;
    auto direct = window_sweaters::knitting::detail::RasterizeTile(spec, layout, cells, &height);
    SavePng(direct, (output / L"spotify-diagnostic-tile.png").c_str());
    SavePng(TileDetail(height, layout, 62), (output / L"spotify-diagnostic-height.png").c_str());
    auto directDetail = TileDetail(direct, layout, 62);
    SavePng(directDetail, (output / L"spotify-corrected-detail.png").c_str());
    auto upstream = LoadPng(root / L"references" / L"app-05.png");
    RasterImage crop{640, 80, 2560, {}};
    crop.pixels.resize(640*80);
    for (std::uint32_t y = 0; y < 80; ++y)
        for (std::uint32_t x = 0; x < 640; ++x) crop.pixels[y*640+x] = Pixel(upstream, 76+x, 602+y);
    SavePng(crop, (output / L"spotify-source-detail.png").c_str());
    auto metric = [&](const RasterImage& candidate) {
        double absolute = 0, square = 0, baselineEnergy = 0, candidateEnergy = 0;
        auto gray = [](std::uint32_t p) {
            return .2126 * ((p >> 16) & 255) + .7152 * ((p >> 8) & 255) + .0722 * (p & 255);
        };
        for (std::uint32_t y = 0; y < crop.height; ++y)
            for (std::uint32_t x = 0; x < crop.width; ++x) {
                auto a = gray(Pixel(crop, x, y)), b = gray(Pixel(candidate, x, y));
                absolute += std::abs(a-b); square += (a-b)*(a-b);
                if (x && y) {
                    baselineEnergy += std::abs(a-gray(Pixel(crop, x-1, y))) + std::abs(a-gray(Pixel(crop, x, y-1)));
                    candidateEnergy += std::abs(b-gray(Pixel(candidate, x-1, y))) + std::abs(b-gray(Pixel(candidate, x, y-1)));
                }
            }
        auto count = static_cast<double>(crop.pixels.size());
        std::cout << "authored Spotify same-palette/repeat texture: MAE=" << absolute/count
            << ", RMSE=" << std::sqrt(square/count) << ", edgeEnergyRatio=" << candidateEnergy/baselineEnergy << "\n";
        return std::array<double, 3>{absolute/count, std::sqrt(square/count), candidateEnergy/baselineEnergy};
    };
    auto texture = metric(directDetail);
    Require(texture[0] < 6. && texture[1] < 8.,
        "Source-comparable raised stitches preserve bottom-up shader lighting within AA tolerance");
    Require(texture[2] > .85 && texture[2] < 1.10,
        "Native source-comparable yarn detail retains edge energy, not blurred diagonal grain");
    auto badLayout = layout; badLayout.stitch_width = std::numeric_limits<float>::quiet_NaN();
    Invalid([&] { window_sweaters::knitting::detail::RasterizeTile(spec, badLayout, cells); },
        "Raster adapter rejects nonfinite cell geometry before creating paths");
    std::vector<window_sweaters::knitting::detail::YarnCell> excessive;
    for (int color = 0; color < 17; ++color) excessive.push_back({0, 0, 0xff000000u | static_cast<std::uint32_t>(color)});
    Invalid([&] { window_sweaters::knitting::detail::RasterizeTile(spec, layout, excessive); },
        "Raised adapter rejects seventeenth yarn rather than skipping or recolouring source cells");
    auto solidSpec = spec;
    ws_knit_layout solidLayout{};
    ws_knit_make_layout(&material, 10, 4, &solidLayout);
    solidLayout.yarn_width = .9f;
    auto solid = window_sweaters::knitting::detail::RasterizeTile(solidSpec, solidLayout);
    SavePng(TileDetail(solid, solidLayout, 62), (output / L"raised-solid-matched-phase.png").c_str());
}

void RenderBehavior(const std::filesystem::path& root) {
    TileCache cache;
    TileSpec tile;
    KnitFrameSpec frame;
    frame.deviceScale = 2;
    auto perimeter = KnitRaster::RenderPerimeter(frame, tile, cache);
    auto image = KnitRaster::Compose(perimeter);
    Require(image.width == 1280 && image.height == 320, "620x140 DIP content plus 10 DIP band at 2x");
    std::size_t perimeterBytes = 0;
    for (const auto& piece : perimeter.pieces) perimeterBytes += piece.image.ByteSize();
    Require(perimeterBytes < image.ByteSize() / 3, "Production allocates narrow perimeter surfaces only");
    Require(Alpha(Pixel(image, 640, 160)) == 0, "Content center transparent");
    Require(Alpha(Pixel(image, 0, 0)) == 0, "Rounded exterior corner transparent");
    Require(Alpha(Pixel(image, 100, 10)) == 255, "Top band opaque");
    Require(Alpha(Pixel(image, 10, 100)) == 255, "Left band opaque");
    Require(Alpha(Pixel(image, 100, 20)) == 255, "Tuck below content edge opaque");
    Require(Alpha(Pixel(image, 100, 24)) == 0, "Tuck stops at clamped inner edge");
    Premultiplied(image);
    Require(KnitRaster::RenderFrame(frame, tile, cache).pixels == image.pixels, "Fixture uses exact production output");
    Require(cache.GenerationCount() == 1, "Repeated rendering/reposition needs no yarn generation");
    auto repeat = static_cast<std::uint32_t>(perimeter.tile->layout.width_dip * 2);
    bool periodic = true;
    std::array<std::uint32_t, 3> orientationErrors{};
    std::array<std::uint32_t, 3> orientationMaxChannel{};
    for (std::uint32_t y = 4; y < 18; ++y)
        for (std::uint32_t x = 80; x < 160; ++x) {
            if (Pixel(image, x, y) != Pixel(image, x + repeat, y)) periodic = false;
            const std::array<std::uint32_t, 3> rotated = {
                Pixel(image, image.width-1-y, x),
                Pixel(image, image.width-1-x, image.height-1-y),
                Pixel(image, y, image.height-1-x)};
            for (std::size_t side = 0; side < 3; ++side) {
                auto reference = Pixel(image, x, y);
                if (reference != rotated[side]) ++orientationErrors[side];
                for (int shift = 0; shift < 32; shift += 8) {
                    auto delta = static_cast<std::uint32_t>(std::abs(static_cast<int>((reference >> shift) & 255)
                        - static_cast<int>((rotated[side] >> shift) & 255)));
                    orientationMaxChannel[side] = std::max(orientationMaxChannel[side], delta);
                }
            }
        }
    Require(periodic, "Periodic bitmap brush preserves native repeat");
    std::cout << "rotation mismatches/max channel deltas: ";
    for (std::size_t side = 0; side < 3; ++side)
        std::cout << orientationErrors[side] << "/" << orientationMaxChannel[side] << " ";
    std::cout << "\n";
    Require(std::all_of(orientationMaxChannel.begin(), orientationMaxChannel.end(),
        [](auto delta) { return delta <= 1; }), "Separately rotated edge sampling differs by at most 1/255 channel");
    auto nearest = tile; nearest.sampling = Sampling::Nearest; nearest.rasterScale = 2;
    auto nearestImage = KnitRaster::RenderFrame(frame, nearest, cache);
    bool exactNearest = true;
    for (std::uint32_t y = 4; y < 18; ++y)
        for (std::uint32_t x = 80; x < 160; ++x)
            if (Pixel(nearestImage, x, y) != Pixel(nearestImage, nearestImage.width-1-y, x)
                || Pixel(nearestImage, x, y) != Pixel(nearestImage, nearestImage.width-1-x, nearestImage.height-1-y)
                || Pixel(nearestImage, x, y) != Pixel(nearestImage, y, nearestImage.height-1-x)) exactNearest = false;
    Require(exactNearest, "Nearest sampling proves exact four-edge orientation");
    // Analytic ring interior excludes a 1-physical-pixel AA boundary tolerance.
    bool cornerHoles = false;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x) {
            float dx = (x + .5f) / 2, dy = (y + .5f) / 2;
            bool outerInterior = InsideRounded(dx, dy, .5f, .5f, 639.f, 159.f, 19.5f);
            bool innerExpanded = InsideRounded(dx, dy, 10.5f, 10.5f, 619.f, 139.f, 9.5f);
            if (outerInterior && !innerExpanded && Alpha(Pixel(image, x, y)) != 255) cornerHoles = true;
        }
    Require(!cornerHoles, "Mitres and rounded inner arcs have no uncovered yarn holes");
    auto dimmed = frame; dimmed.dim = .5f;
    auto dark = KnitRaster::RenderFrame(dimmed, tile, cache);
    bool alphaEqual = true;
    for (std::size_t i = 0; i < image.pixels.size(); ++i)
        if (Alpha(image.pixels[i]) != Alpha(dark.pixels[i])) alphaEqual = false;
    Require(alphaEqual, "Black dim overlay darkens without fading");
    Require((Pixel(dark, 100, 10) & 255) < (Pixel(image, 100, 10) & 255), "Dimming darkens yarn");
    auto fractional = frame; fractional.deviceScale = 1.25f;
    auto fractionalImage = KnitRaster::RenderFrame(fractional, tile, cache);
    Require(fractionalImage.width == 801 && fractionalImage.height == 201,
        "Fractional DPI uses ceil physical band padding without shifting the target");
    Premultiplied(fractionalImage);
    auto physical = frame;
    physical.deviceScale = 1.5f;
    physical.contentWidthPixels = 1000; physical.contentHeightPixels = 241;
    auto physicalPerimeter = KnitRaster::RenderPerimeter(physical, tile, cache);
    Require(physicalPerimeter.width == 1030 && physicalPerimeter.height == 271
        && physicalPerimeter.contentInsetPixels == 15,
        "Native physical target extents do not undergo lossy DIP ceil roundtrips");
    physical.deviceScale = 1.25f;
    physical.contentWidthPixels = 775; physical.contentHeightPixels = 175;
    physicalPerimeter = KnitRaster::RenderPerimeter(physical, tile, cache);
    Require(physicalPerimeter.width == 801 && physicalPerimeter.height == 201
        && physicalPerimeter.contentInsetPixels == 13,
        "Integer outer placement matches physical padding and fractional gutter");
    physical.deviceScale = 1.f;
    physical.bandDip = static_cast<float>(10.00000001);
    physical.contentWidthPixels = 1000; physical.contentHeightPixels = 241;
    physical.physicalPaddingPixels = 11;
    physicalPerimeter = KnitRaster::RenderPerimeter(physical, tile, cache);
    Require(physicalPerimeter.width == 1022 && physicalPerimeter.height == 263
        && physicalPerimeter.contentInsetPixels == 11,
        "Authoritative double-precision geometry padding survives a float integer crossing");
    auto precisionImage = KnitRaster::Compose(physicalPerimeter);
    Require(Alpha(Pixel(precisionImage, 100, 0)) == 0 && Alpha(Pixel(precisionImage, 100, 1)) == 255
        && physicalPerimeter.tile->spec.bandDip == 10.f,
        "Extra physical padding is transparent gutter, not stretched yarn or target shift");
    auto resize = frame; resize.widthDip = 800;
    auto beforeResize = cache.GenerationCount();
    auto resized = KnitRaster::RenderFrame(resize, tile, cache);
    Require(Pixel(resized, 100, 10) == Pixel(image, 100, 10), "Corner anchoring stable under resize");
    Require(cache.GenerationCount() == beforeResize, "Window size is not part of the immutable yarn tile key");
    for (float size : {.25f, 1.f, 2.f, 3.f, 6.f, 10.f}) {
        auto tiny = frame; tiny.widthDip = size; tiny.heightDip = size; tiny.radiusDip = 500; tiny.tuckDip = 1000;
        auto tinyImage = KnitRaster::RenderFrame(tiny, tile, cache);
        Require(!tinyImage.pixels.empty(), "Tiny geometry safely renders");
        Require(std::any_of(tinyImage.pixels.begin(), tinyImage.pixels.end(),
            [](auto p) { return Alpha(p) > 0; }), "Tiny geometry retains a visible sweater");
    }
    for (float scale : {1.f, 1.25f, 2.25f})
        for (float radius : {0.f, 10.f, 1000.f}) {
            auto corners = frame;
            corners.widthDip = 105.25f; corners.heightDip = 31.75f;
            corners.deviceScale = scale; corners.radiusDip = radius; corners.tuckDip = 14.f;
            auto cornersImage = KnitRaster::RenderFrame(corners, tile, cache);
            float actualRadius = std::min(radius, 15.875f);
            float actualTuck = 13.875f;
            bool covered = true;
            for (std::uint32_t y = 0; y < cornersImage.height; ++y)
                for (std::uint32_t x = 0; x < cornersImage.width; ++x) {
                    float dx = (x + .5f) / scale, dy = (y + .5f) / scale;
                    float gutter = std::ceil(10.f * scale) / scale - 10.f;
                    dx -= gutter; dy -= gutter;
                    float tolerance = 1.f / scale;
                    bool outerInside = InsideRounded(dx, dy, tolerance, tolerance,
                        125.25f - 2*tolerance, 51.75f - 2*tolerance,
                        actualRadius + 10.f - tolerance);
                    bool holeExpanded = InsideRounded(dx, dy, 10.f + actualTuck - tolerance,
                        10.f + actualTuck - tolerance, 105.25f - 2*actualTuck + 2*tolerance,
                        31.75f - 2*actualTuck + 2*tolerance,
                        std::max(0.f, actualRadius - actualTuck) + tolerance);
                    if (outerInside && !holeExpanded && Alpha(Pixel(cornersImage, x, y)) != 255) covered = false;
                }
            Require(covered, "Fractional-size/DPI capsule and square corners have no mitre holes");
        }
    auto invalid = frame; invalid.widthDip = 0;
    Invalid([&] { KnitRaster::RenderFrame(invalid, tile, cache); }, "Zero content width rejected");
    invalid = frame; invalid.radiusDip = std::numeric_limits<float>::quiet_NaN();
    Invalid([&] { KnitRaster::RenderFrame(invalid, tile, cache); }, "NaN radius rejected");
    invalid = frame; invalid.dim = 1.1f;
    Invalid([&] { KnitRaster::RenderFrame(invalid, tile, cache); }, "Invalid dim rejected");
    invalid = frame; invalid.widthDip = 100000;
    Invalid([&] { KnitRaster::RenderFrame(invalid, tile, cache); }, "Unbounded allocation rejected");
    invalid = frame; invalid.contentWidthPixels = 1000;
    Invalid([&] { KnitRaster::RenderFrame(invalid, tile, cache); }, "Partial physical-dimension override rejected");
    invalid = frame; invalid.physicalPaddingPixels = 100;
    Invalid([&] { KnitRaster::RenderFrame(invalid, tile, cache); }, "Unrelated authoritative padding rejected");
    invalid = frame; invalid.deviceScale = 1e-38f;
    invalid.contentWidthPixels = 1000; invalid.contentHeightPixels = 241;
    Invalid([&] { KnitRaster::RenderPerimeter(invalid, tile, cache); },
        "Finite positive near-zero scale cannot overflow derived physical-to-DIP geometry");
    invalid = frame; invalid.deviceScale = std::nextafter(MinimumDeviceScale, 0.f);
    Invalid([&] { KnitRaster::RenderPerimeter(invalid, tile, cache); },
        "Scale below explicit supported minimum rejected");
    auto minimum = frame; minimum.deviceScale = MinimumDeviceScale;
    minimum.contentWidthPixels = 100; minimum.contentHeightPixels = 31;
    auto minimumPerimeter = KnitRaster::RenderPerimeter(minimum, tile, cache);
    Require(minimumPerimeter.width == 104 && minimumPerimeter.height == 35
        && minimumPerimeter.contentInsetPixels == 2,
        "Supported minimum scale safely derives physical perimeter bounds");
    auto output = root / L"outputs";
    std::filesystem::create_directories(output);
    SavePng(image, (output / L"stockinette-620x140-2x.png").c_str());
    auto roundTrip = LoadPng(output / L"stockinette-620x140-2x.png");
    std::uint32_t pngMaxChannel = 0;
    bool pngAlphaEqual = true;
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        if (Alpha(roundTrip.pixels[i]) != Alpha(image.pixels[i])) pngAlphaEqual = false;
        for (int shift = 0; shift < 24; shift += 8) {
            auto delta = static_cast<std::uint32_t>(std::abs(static_cast<int>((roundTrip.pixels[i] >> shift) & 255)
                - static_cast<int>((image.pixels[i] >> shift) & 255)));
            pngMaxChannel = std::max(pngMaxChannel, delta);
        }
    }
    std::cout << "PNG straight/premultiplied roundtrip max channel delta: " << pngMaxChannel << "\n";
    Require(pngAlphaEqual && pngMaxChannel <= 1,
            "PNG preserves alpha and premultiplied colours within 1/255 conversion rounding");
    SavePng(dark, (output / L"stockinette-dim-2x.png").c_str());
    SavePng(fractionalImage, (output / L"stockinette-1.25x.png").c_str());
    tile.material = Material::Raised;
    auto raisedImage = KnitRaster::RenderFrame(frame, tile, cache);
    SavePng(raisedImage, (output / L"raised-stockinette-620x140-2x.png").c_str());
    SavePng(perimeter.tile->image, (output / L"plain-periodic-tile.png").c_str());
    tile.baseArgb = 0xff497641u;
    auto magnified = frame; magnified.deviceScale = 8;
    auto detailImage = KnitRaster::RenderFrame(magnified, tile, cache);
    RasterImage detail{640, 80, 2560, {}};
    detail.pixels.resize(static_cast<std::size_t>(detail.width) * detail.height);
    for (std::uint32_t y = 0; y < detail.height; ++y)
        for (std::uint32_t x = 0; x < detail.width; ++x)
            detail.pixels[static_cast<std::size_t>(y) * detail.width + x] = Pixel(detailImage, 800+x, y);
    SavePng(detail, (output / L"raised-stitch-detail-4x.png").c_str());
    ReferenceGeometry(image, root);
    ReferenceStitches(detail, root);
    SourceTextureDiagnosis(root);
    std::cout << "production perimeter bytes=" << perimeterBytes << ", full fixture bytes="
              << image.ByteSize() << ", tile bytes=" << perimeter.tile->image.ByteSize() << "\n";
}
}

int wmain(int argc, wchar_t** argv) {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::cout << std::unitbuf;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { std::cerr << "COM initialization failed\n"; return 1; }
    int exitCode = 0;
    try {
        std::filesystem::path root = argc > 1 ? argv[1] : L"tests\\Knitting";
        PortableMath();
        CacheAndInvalidInputs();
        RenderBehavior(root);
        std::cout << "PASS " << checks << " native knitting checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
        exitCode = 1;
    }
    CoUninitialize();
    return exitCode;
}
