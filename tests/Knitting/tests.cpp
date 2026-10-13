// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Rendering\KnitRaster.h"
#include "..\..\src\Knitting\WicSurface.h"
#include "..\..\src\Designs\Catalogue.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <crtdbg.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>

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
    Require(probe.HitCount() == 1, "Cache hit metric counts actual reused tile");
    auto half = spec; half.bandDip = 10.1f;
    Require(probe.Get(half) == tile, "Pinned half-DIP tile quantization");
    auto opaque = spec; opaque.baseArgb &= 0x00ffffffu;
    Require(probe.Get(opaque) == tile, "ARGB input alpha cannot make yarn translucent");
    auto originalPixels = tile->image.pixels;
    TileCache bounded(tile->ByteSize());
    auto inUse = bounded.Get(spec);
    auto another = spec; another.baseArgb = 0xffd1495bu;
    bounded.Get(another);
    Require(bounded.ResidentBytes() <= tile->ByteSize(), "Cache tile bytes (albedo + stitch height) stay bounded");
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
    auto changed = spec;
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
    changed = spec; changed.stitch = static_cast<Stitch>(255);
    Invalid([&] { probe.Get(changed); }, "Unknown stitch rejected");
    changed = spec; changed.gauge.jitter = std::numeric_limits<float>::quiet_NaN();
    Invalid([&] { probe.Get(changed); }, "NaN gauge rejected");
    changed = spec; changed.rasterScale = 9;
    Invalid([&] { probe.Get(changed); }, "Excess sampling rejected");
    changed = spec; changed.bandDip = 257;
    Invalid([&] { probe.Get(changed); }, "Excess band rejected");
    changed = spec; changed.gauge.aspect = 8; changed.gauge.rows = .1f; changed.bandDip = 256;
    Invalid([&] { probe.Get(changed); }, "Excess tile allocation rejected");
}

RasterImage BandImage(const KnitPerimeter& perimeter) {
    auto image = KnitRaster::Compose(perimeter);
    const auto m = perimeter.shadowMarginPixels;
    Require(image.width > 2*m && image.height > 2*m, "Shadow margin fits the composed perimeter");
    RasterImage band{image.width - 2*m, image.height - 2*m, (image.width - 2*m) * 4, {}};
    band.pixels.reserve(static_cast<std::size_t>(band.width) * band.height);
    for (std::uint32_t y = m; y < image.height - m; ++y)
        for (std::uint32_t x = m; x < image.width - m; ++x)
            band.pixels.push_back(image.pixels[static_cast<std::size_t>(y) * image.width + x]);
    return band;
}

void DpiTileReuse() {
    for (const auto sampling : {Sampling::Nearest, Sampling::Linear}) {
        TileCache cache;
        TileSpec tile;
        tile.sampling = sampling;
        KnitFrameSpec frame;
        frame.widthDip = 73; frame.heightDip = 61;
        frame.deviceScale = 1.5f;
        const auto original = KnitRaster::RenderPerimeter(frame, tile, cache);
        const auto generations = cache.GenerationCount();
        const auto resident = cache.ResidentBytes();
        for (const float scale : {MinimumDeviceScale, 1.f, 1.25f, 1.5f, 2.f, 3.f, MaximumDeviceScale}) {
            frame.deviceScale = scale;
            const auto reused = KnitRaster::RenderPerimeter(frame, tile, cache);
            TileCache fresh;
            const auto independent = KnitRaster::RenderPerimeter(frame, tile, fresh);
            Require(reused.tile == original.tile && cache.GenerationCount() == generations &&
                cache.ResidentBytes() == resident, "DPI presentation changes reuse tile pixels and cache bytes");
            Require(reused.deviceScale == scale && reused.width == independent.width && reused.height == independent.height
                && (scale == 1.5f || (reused.width != original.width && reused.height != original.height)),
                "DPI presentation retains current physical size and scale");
            Require(reused.tile->image.pixels == independent.tile->image.pixels
                && reused.tile->height.pixels == independent.tile->height.pixels,
                "DPI frame requests retain independently generated tile colour and stitch height pixels");
            Require(reused.tile->spec.sampling == sampling &&
                KnitRaster::Compose(reused).pixels == KnitRaster::Compose(independent).pixels,
                "DPI cache reuse retains sampling and independently generated rendered pixels");
        }
    }
}

RasterImage BandFrame(const KnitFrameSpec& frame, TileSpec tile, TileCache& cache) {
    return BandImage(KnitRaster::RenderPerimeter(frame, std::move(tile), cache));
}

bool InsideRounded(float x, float y, float left, float top, float width, float height, float radius);

void DropShadowContracts(const RasterImage& desktop, std::uint32_t m, float scale) {
    const float w = static_cast<float>(desktop.width - 2*m), h = static_cast<float>(desktop.height - 2*m);
    std::uint32_t peak = 0;
    bool black = true, border = true;
    for (std::uint32_t y = 0; y < desktop.height; ++y)
        for (std::uint32_t x = 0; x < desktop.width; ++x) {
            auto p = Pixel(desktop, x, y);
            if (!x || !y || x == desktop.width-1 || y == desktop.height-1) border = border && Alpha(p) == 0;
            float px = x + .5f - static_cast<float>(m), py = y + .5f - static_cast<float>(m);
            if (InsideRounded(px, py, -1.f, -1.f, w + 2.f, h + 2.f, 20.f*scale + 1.f)) continue;
            peak = std::max(peak, Alpha(p));
            black = black && (p & 0x00ffffffu) == 0;
        }
    std::cout << "drop shadow peak alpha outside the band: " << peak << "/255\n";
    Require(border, "Drop-shadow tail fits the margin; the overlay edge stays transparent");
    Require(peak > 40 && peak <= 78, "Soft shadow peaks near 0.3 alpha; overlapping pieces never double it");
    Require(black, "Drop shadow is premultiplied black on the desktop");
    auto a = [&](std::uint32_t x, std::uint32_t y) { return Alpha(Pixel(desktop, x, y)); };
    const auto cx = desktop.width / 2, cy = desktop.height / 2;
    const auto bottom = a(cx, desktop.height - m + 2), top = a(cx, m - 3);
    const auto right = a(desktop.width - m + 2, cy), left = a(m - 3, cy);
    std::cout << "drop shadow 2.5px outside top/right/bottom/left: " << top << "/" << right
              << "/" << bottom << "/" << left << "\n";
    Require(bottom > top && right > left && bottom > 0 && right > 0,
        "Shadow falls down/right, away from the upper-left ring light");

    const ws_knit_ring ring{120.f, 80.f, 15.f, 11.f, 4.f};
    constexpr int bw = 132, bh = 92;
    std::array<std::vector<std::uint32_t>, 4> sides;
    for (int side = 0; side < 4; ++side) {
        sides[side].assign(static_cast<std::size_t>(bw) * bh, 0u);
        Require(ws_knit_ring_drop_shadow(&ring, side, 10.f, -6.f, -6.f, 1.f, sides[side].data(), bw, bh) == 1,
            "Drop shadow accepts each side");
    }
    bool partition = true, hole = true, any = false;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            auto i = static_cast<std::size_t>(y) * bw + x;
            int owners = 0;
            for (const auto& side : sides) owners += side[i] ? 1 : 0;
            partition = partition && owners <= 1;
            any = any || owners > 0;
            if (InsideRounded(x - 6.f + .5f, y - 6.f + .5f, 5.f, 5.f, 110.f, 70.f, 10.f)) hole = hole && owners == 0;
        }
    Require(partition && any, "Mitre partition: each shadow pixel has exactly one owning side");
    Require(hole, "Drop shadow never enters the inner band or window hole");
    std::vector<std::uint32_t> opaque(static_cast<std::size_t>(bw) * bh, 0xff112233u);
    Require(ws_knit_ring_drop_shadow(&ring, 2, 10.f, -6.f, -6.f, 1.f, opaque.data(), bw, bh) == 1
        && std::all_of(opaque.begin(), opaque.end(), [](auto p) { return p == 0xff112233u; }),
        "Shadow composites under opaque yarn without changing it");
    Require(ws_knit_ring_drop_shadow(&ring, 4, 10.f, 0.f, 0.f, 1.f, opaque.data(), bw, bh) == 0
        && ws_knit_ring_drop_shadow(&ring, 0, 0.f, 0.f, 0.f, 1.f, opaque.data(), bw, bh) == 0,
        "Drop shadow rejects invalid side and band");
}

bool InsideRounded(float x, float y, float left, float top, float width, float height, float radius) {
    if (x < left || y < top || x >= left + width || y >= top + height) return false;
    radius = std::clamp(radius, 0.f, std::min(width, height) * .5f);
    float mx = left + width * .5f, my = top + height * .5f;
    float hx = std::max(0.f, width * .5f - radius), hy = std::max(0.f, height * .5f - radius);
    float cx = std::clamp(x, mx - hx, mx + hx);
    float cy = std::clamp(y, my - hy, my + hy);
    return (x-cx)*(x-cx) + (y-cy)*(y-cy) <= radius*radius;
}

void ReferenceGeometry(const RasterImage& image, const std::filesystem::path& root) {
    auto reference = LoadPng(root / L"references" / L"app-05.png");
    Require(reference.width == 1440 && reference.height == 1060, "Pinned renderer plate dimensions");
    std::size_t compared = 0, mismatch = 0, nativeOnly = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x) {
            float dipX = (x + .5f) / 2.f, dipY = (y + .5f) / 2.f;
            if (InsideRounded(dipX, dipY, 9.5f, 9.5f, 621.f, 141.f, 10.5f)) continue;
            const float fray = ws_knit_fray_depth(10.f);
            if (InsideRounded(dipX, dipY, 0.f, 0.f, 640.f, 160.f, 20.f)
                && !InsideRounded(dipX, dipY, fray, fray, 640.f - 2*fray, 160.f - 2*fray, 20.f - fray)) continue;
            auto baseline = Pixel(reference, x + 76, y + 204);
            int backgroundDistance = std::abs(static_cast<int>((baseline >> 16) & 255) - 249)
                + std::abs(static_cast<int>((baseline >> 8) & 255) - 247)
                + std::abs(static_cast<int>(baseline & 255) - 243);
            bool upstreamYarn = backgroundDistance > 12;
            bool nativeYarn = Alpha(Pixel(image, x, y)) > 128;
            ++compared;
            if (upstreamYarn != nativeYarn) ++mismatch;
            if (nativeYarn && !upstreamYarn) ++nativeOnly;
        }
    double fraction = static_cast<double>(mismatch) / static_cast<double>(compared);
    std::cout << "reference exterior geometry: " << mismatch << "/" << compared
              << " disagreements (" << fraction * 100. << "%, " << nativeOnly << " native-only), tolerance 1%\n";
    Require(fraction <= .01, "Pinned rounded exterior geometry within 1% raster tolerance");
}

struct StitchPitch { int pixels = 0; double correlation = -1; };
StitchPitch MeasureStitchPitch(const RasterImage& image, std::uint32_t ox,
                              std::uint32_t oy, std::uint32_t width, std::uint32_t height) {
    StitchPitch result;
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
    auto desktop = KnitRaster::Compose(perimeter);
    Require(desktop.width == 1300 && desktop.height == 340 && perimeter.shadowMarginPixels == 10
        && perimeter.contentInsetPixels == 30, "Overlay adds a half-band (10px at 2x) drop-shadow margin per side");
    DropShadowContracts(desktop, perimeter.shadowMarginPixels, 2.f);
    auto image = BandImage(perimeter);
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
    Require(KnitRaster::RenderFrame(frame, tile, cache).pixels == desktop.pixels, "Fixture uses exact production output");
    Require(cache.GenerationCount() == 1, "Repeated rendering/reposition needs no yarn generation");
    auto repeat = static_cast<std::uint32_t>(perimeter.tile->layout.width_dip * 2);
    bool periodic = true;
    bool rotatedCoverage = true;
    for (std::uint32_t y = 4; y < 18; ++y)
        for (std::uint32_t x = 80; x < 160; ++x) {
            if (Pixel(image, x, y) != Pixel(image, x + repeat, y)) periodic = false;
            auto alpha = Alpha(Pixel(image, x, y));
            if (alpha != Alpha(Pixel(image, image.width-1-y, x))
                || alpha != Alpha(Pixel(image, image.width-1-x, image.height-1-y))
                || alpha != Alpha(Pixel(image, y, image.height-1-x))) rotatedCoverage = false;
        }
    Require(periodic, "Periodic bitmap brush preserves native repeat");
    Require(rotatedCoverage, "Separately rotated edges share coverage; only screen-space light differs");
    auto rowLight = [&](std::uint32_t y) {
        double sum = 0;
        for (std::uint32_t x = 80; x < 80 + 4 * repeat; ++x) {
            auto p = Pixel(image, x, y);
            sum += .2126 * ((p >> 16) & 255) + .7152 * ((p >> 8) & 255) + .0722 * (p & 255);
        }
        return sum / (4 * repeat);
    };
    double crown = (rowLight(9) + rowLight(10) + rowLight(11)) / 3;
    double outerTop = rowLight(1), innerTop = rowLight(20), outerBottom = rowLight(image.height - 2);
    std::cout << "tube luminance outer/crown/inner/bottom-outer: " << outerTop << "/" << crown
              << "/" << innerTop << "/" << outerBottom << "\n";
    Require(crown > 1.05 * outerTop && crown > 1.15 * innerTop,
        "Band crown is brighter than both flanks, reading as a padded tube");
    Require(outerTop > 1.1 * outerBottom, "Upper-left key light brightens top outer flank over bottom outer flank");
    auto nearest = tile; nearest.sampling = Sampling::Nearest; nearest.rasterScale = 2;
    auto nearestImage = BandFrame(frame, nearest, cache);
    auto sameYarn = [](std::uint32_t a, std::uint32_t b) {
        float pa = static_cast<float>(std::max({(a >> 16) & 255, (a >> 8) & 255, a & 255}));
        float pb = static_cast<float>(std::max({(b >> 16) & 255, (b >> 8) & 255, b & 255}));
        if (pa < 48.f || pb < 48.f) return true;
        for (int shift = 0; shift < 24; shift += 8)
            if (std::abs(((a >> shift) & 255) / pa - ((b >> shift) & 255) / pb) > 4.f / 48.f) return false;
        return true;
    };
    bool exactNearest = true;
    for (std::uint32_t y = 4; y < 18; ++y)
        for (std::uint32_t x = 80; x < 160; ++x) {
            auto p = Pixel(nearestImage, x, y);
            if (!sameYarn(p, Pixel(nearestImage, nearestImage.width-1-y, x))
                || !sameYarn(p, Pixel(nearestImage, nearestImage.width-1-x, nearestImage.height-1-y))
                || !sameYarn(p, Pixel(nearestImage, y, nearestImage.height-1-x))) exactNearest = false;
        }
    Require(exactNearest, "Nearest sampling proves four-edge orientation under ring light");
    bool cornerHoles = false;
    const float fray = ws_knit_fray_depth(10.f);
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x) {
            float dx = (x + .5f) / 2, dy = (y + .5f) / 2;
            bool outerInterior = InsideRounded(dx, dy, .5f + fray, .5f + fray, 639.f - 2*fray, 159.f - 2*fray, 19.5f - fray);
            bool innerExpanded = InsideRounded(dx, dy, 10.5f, 10.5f, 619.f, 139.f, 9.5f);
            if (outerInterior && !innerExpanded && Alpha(Pixel(image, x, y)) != 255) cornerHoles = true;
        }
    Require(!cornerHoles, "Mitres and rounded inner arcs have no uncovered yarn holes");
    auto dimmed = frame; dimmed.dim = .5f;
    auto dark = BandFrame(dimmed, tile, cache);
    bool alphaEqual = true;
    for (std::size_t i = 0; i < image.pixels.size(); ++i)
        if (Alpha(image.pixels[i]) != Alpha(dark.pixels[i])) alphaEqual = false;
    Require(alphaEqual, "Black dim overlay darkens without fading");
    Require((Pixel(dark, 100, 10) & 255) < (Pixel(image, 100, 10) & 255), "Dimming darkens yarn");
    auto fractional = frame; fractional.deviceScale = 1.25f;
    auto fractionalImage = BandFrame(fractional, tile, cache);
    Require(fractionalImage.width == 801 && fractionalImage.height == 201,
        "Fractional DPI uses ceil physical band padding without shifting the target");
    Premultiplied(fractionalImage);
    auto physical = frame;
    physical.deviceScale = 1.5f;
    physical.contentWidthPixels = 1000; physical.contentHeightPixels = 241;
    auto physicalPerimeter = KnitRaster::RenderPerimeter(physical, tile, cache);
    Require(physicalPerimeter.width == 1046 && physicalPerimeter.height == 287
        && physicalPerimeter.contentInsetPixels == 23 && physicalPerimeter.shadowMarginPixels == 8,
        "Native physical target extents do not undergo lossy DIP ceil roundtrips");
    physical.deviceScale = 1.25f;
    physical.contentWidthPixels = 775; physical.contentHeightPixels = 175;
    physicalPerimeter = KnitRaster::RenderPerimeter(physical, tile, cache);
    Require(physicalPerimeter.width == 815 && physicalPerimeter.height == 215
        && physicalPerimeter.contentInsetPixels == 20 && physicalPerimeter.shadowMarginPixels == 7,
        "Integer outer placement matches physical padding, shadow margin and fractional gutter");
    physical.deviceScale = 1.f;
    physical.bandDip = static_cast<float>(10.00000001);
    physical.contentWidthPixels = 1000; physical.contentHeightPixels = 241;
    physical.physicalPaddingPixels = 11;
    physicalPerimeter = KnitRaster::RenderPerimeter(physical, tile, cache);
    Require(physicalPerimeter.width == 1034 && physicalPerimeter.height == 275
        && physicalPerimeter.contentInsetPixels == 17 && physicalPerimeter.shadowMarginPixels == 6,
        "Authoritative double-precision geometry padding survives a float integer crossing");
    auto precisionImage = BandImage(physicalPerimeter);
    Require(Alpha(Pixel(precisionImage, 100, 0)) < 128 && Alpha(Pixel(precisionImage, 100, 1)) == 255
        && physicalPerimeter.tile->spec.bandDip == 10.f,
        "Extra physical padding is shadowed desktop gutter, not stretched yarn or target shift");
    auto resize = frame; resize.widthDip = 800;
    auto beforeResize = cache.GenerationCount();
    auto resized = BandFrame(resize, tile, cache);
    Require(Pixel(resized, 100, 10) == Pixel(image, 100, 10), "Corner anchoring stable under resize");
    Require(cache.GenerationCount() == beforeResize, "Window size is not part of the immutable yarn tile key");
    for (float size : {.25f, 1.f, 2.f, 3.f, 6.f, 10.f}) {
        auto tiny = frame; tiny.widthDip = size; tiny.heightDip = size; tiny.radiusDip = 500; tiny.tuckDip = 1000;
        auto tinyImage = KnitRaster::RenderFrame(tiny, tile, cache);
        Require(!tinyImage.pixels.empty(), "Tiny geometry safely renders");
        Require(std::any_of(tinyImage.pixels.begin(), tinyImage.pixels.end(),
            [](auto p) { return Alpha(p) > 128; }), "Tiny geometry retains a visible sweater, not only shadow");
    }
    for (float scale : {1.f, 1.25f, 2.25f})
        for (float radius : {0.f, 10.f, 1000.f}) {
            auto corners = frame;
            corners.widthDip = 105.25f; corners.heightDip = 31.75f;
            corners.deviceScale = scale; corners.radiusDip = radius; corners.tuckDip = 14.f;
            auto cornersImage = BandFrame(corners, tile, cache);
            float actualRadius = std::min(radius, 15.875f);
            float actualTuck = 13.875f;
            bool covered = true;
            for (std::uint32_t y = 0; y < cornersImage.height; ++y)
                for (std::uint32_t x = 0; x < cornersImage.width; ++x) {
                    float dx = (x + .5f) / scale, dy = (y + .5f) / scale;
                    float gutter = std::ceil(10.f * scale) / scale - 10.f;
                    dx -= gutter; dy -= gutter;
                    float tolerance = 1.f / scale, outerTolerance = tolerance + ws_knit_fray_depth(10.f);
                    bool outerInside = InsideRounded(dx, dy, outerTolerance, outerTolerance,
                        125.25f - 2*outerTolerance, 51.75f - 2*outerTolerance,
                        actualRadius + 10.f - outerTolerance);
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
    invalid = frame; invalid.deviceScale = 0;
    Invalid([&] { KnitRaster::RenderPerimeter(invalid, tile, cache); }, "Zero frame scale rejected");
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
    Require(minimumPerimeter.width == 106 && minimumPerimeter.height == 37
        && minimumPerimeter.contentInsetPixels == 3,
        "Supported minimum scale safely derives physical perimeter bounds");
    auto output = root / L"outputs";
    std::filesystem::create_directories(output);
    SavePng(image, (output / L"stockinette-620x140-2x.png").c_str());
    SavePng(desktop, (output / L"stockinette-620x140-2x-shadow.png").c_str());
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
    auto detailImage = BandFrame(magnified, tile, cache);
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

TileSpec ThemeSpec(const char* id) {
    using namespace window_sweaters::designs;
    auto theme = ws_design_theme_by_id(id);
    if (!theme) throw std::runtime_error("Unknown fixture theme");
    auto source = ws_design_chart_by_name(theme->chart_name);
    if (!source) throw std::runtime_error("Unknown fixture chart");
    TileSpec spec;
    spec.baseArgb = theme->base_argb;
    spec.chart = MakeDesignChart(*source, theme->id);
    return spec;
}

std::string FrameDigest(const KnitPerimeter& p) {
    std::string out;
    for (const auto& piece : p.pieces) {
        out.append(reinterpret_cast<const char*>(&piece.x), sizeof piece.x);
        out.append(reinterpret_cast<const char*>(&piece.y), sizeof piece.y);
        out.append(reinterpret_cast<const char*>(piece.image.pixels.data()), piece.image.ByteSize());
    }
    return out;
}

void ScratchContracts() {
    TileCache cache;
    KnitRasterScratch scratch;
    struct Case { const char* name; KnitFrameSpec frame; TileSpec tile; };
    auto frame = [](float w, float h, float scale, float dim = 0.f, KnitAnchor anchor = KnitAnchor::Corner) {
        KnitFrameSpec f; f.widthDip = w; f.heightDip = h; f.deviceScale = scale; f.dim = dim; f.anchor = anchor;
        return f;
    };
    const std::vector<Case> cases{
        {"base", frame(620, 140, 2), ThemeSpec("spotify")},
        {"resized", frame(300, 90, 2), ThemeSpec("spotify")},
        {"base again", frame(620, 140, 2), ThemeSpec("spotify")},
        {"scale", frame(620, 140, 1.5f), ThemeSpec("spotify")},
        {"theme", frame(620, 140, 2), ThemeSpec("messages")},
        {"anchor", frame(620, 140, 2, 0.f, KnitAnchor::Centre), ThemeSpec("messages")},
        {"dim", frame(620, 140, 2, .5f), ThemeSpec("messages")},
        {"plain", frame(620, 140, 2), TileSpec{}},
        {"base after plain", frame(620, 140, 2), ThemeSpec("spotify")},
        {"chrome", frame(620, 140, 2), ThemeSpec("google-chrome")},
        {"notes", frame(320, 120, 1), ThemeSpec("notes")},
        {"base last", frame(620, 140, 2), ThemeSpec("spotify")},
    };
    for (const auto& c : cases) {
        const auto fresh = FrameDigest(KnitRaster::RenderPerimeter(c.frame, c.tile, cache));
        const auto reused = FrameDigest(KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&scratch}));
        Require(fresh == reused, c.name);
    }
    Require(scratch.GetStats().factories == 2, "One D2D and one WIC factory for the scratch lifetime");
    const auto& c = cases[0];
    auto first = FrameDigest(KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&scratch}));
    auto warm = scratch.GetStats();
    auto second = FrameDigest(KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&scratch}));
    auto after = scratch.GetStats();
    Require(first == second, "Repeated reused render is deterministic");
    Require(after.targets == warm.targets && after.uploads == warm.uploads && after.factories == warm.factories
        && after.reused == warm.reused + 4, "Repeated identical render creates no targets or uploads");
    Require(scratch.ResidentBytes() > 0 && scratch.ResidentBytes() <= 16u * 1024u * 1024u, "Scratch bytes bounded");

    KnitRasterScratch tiny(1);
    Require(FrameDigest(KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&tiny})) == first
        && tiny.ResidentBytes() == 0, "Over-budget scratch renders identically and retains nothing");
    KnitRasterScratch capped(512 * 1024);
    for (const auto& k : cases)
        KnitRaster::RenderPerimeter(k.frame, k.tile, cache, {&capped});
    Require(capped.ResidentBytes() <= 512 * 1024, "Mid-sized budget evicts to its bound");

    std::uint32_t total = 0;
    KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&scratch, [&] { ++total; return false; }});
    Require(total == 2 + 4 * 6, "Every stage polls for cancellation");
    for (std::uint32_t stopAt = 1; stopAt <= total; ++stopAt) {
        std::uint32_t polls = 0;
        bool cancelled = false;
        try {
            KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&scratch, [&] { return ++polls == stopAt; }});
        } catch (const KnitRenderCancelled&) { cancelled = true; }
        Require(cancelled && polls == stopAt, "Stop predicate aborts at the polled stage");
        Require(FrameDigest(KnitRaster::RenderPerimeter(c.frame, c.tile, cache, {&scratch})) == first,
                "Render after an abort matches fresh output");
    }
}

void ChartContracts() {
    using namespace window_sweaters::designs;
    TileCache cache;
    TileSpec plain;
    auto stockinette = cache.Get(plain);
    plain.stitch = Stitch::Rib;
    auto rib = cache.Get(plain);
    plain.stitch = Stitch::Garter;
    auto garter = cache.Get(plain);
    Require(rib->layout.columns == 8 && garter->layout.rows == 8
        && rib->layout.width_dip == 18 && garter->layout.height_dip == 13,
        "Legacy plain 8x8 repeat dimensions preserved for rib and garter");
    Require(Near(rib->layout.yarn_width, 1.2375f), "Source rib thickness is stitch width times .55");
    Require(rib->image.pixels != stockinette->image.pixels
        && garter->image.pixels != stockinette->image.pixels
        && rib->image.pixels != garter->image.pixels, "Three plain styles produce distinct real stitch geometry");
    RecordedPath ribPath, garterPath;
    auto gauge = ws_knit_default_gauge();
    Require(ws_knit_emit_plain(&gauge, &rib->layout, 1, ribPath.Writer())
        && ribPath.moves.size() == 12 && ribPath.curves.size() == 12
        && Near(ribPath.moves[2][0], 1.125f) && Near(ribPath.moves[3][0], 3.735f)
        && Near(ribPath.curves[2][3], 19.5f), "Pinned k1p1 rib column positions and full margins");
    Require(ws_knit_emit_plain(&gauge, &garter->layout, 2, garterPath.Writer())
        && garterPath.moves.size() == 12 && garterPath.curves.size() == 288
        && Near(garterPath.curves[0][0], -3.9375f)
        && Near(garterPath.curves[0][1], -3.80397725f)
        && Near(garterPath.curves[24][0], -2.8125f),
        "Pinned alternating-phase garter waves use exact source quadratic controls");
    auto spotify = ThemeSpec("spotify");
    auto tile = cache.Get(spotify);
    Require(tile->layout.columns == 12 && tile->layout.rows == 6
        && tile->layout.width_dip == 27 && tile->layout.height_dip == 10
        && Near(tile->layout.row_step, 10.f/6) && Near(tile->layout.yarn_width, .9f),
        "Real Spotify chart retains source 12x6, 27x10 repeat and band row rounding");
    auto priorPlain = spotify; priorPlain.stitch = Stitch::Rib; priorPlain.material = Material::Plain;
    Require(cache.Get(priorPlain) == tile, "Chart material and stockinette independent of prior plain selection");
    priorPlain.stitch = Stitch::Garter;
    Require(cache.Get(priorPlain) == tile, "Chart does not inherit previous garter selection");
    auto messages = cache.Get(ThemeSpec("messages"));
    Require(messages->spec.material == Material::Plain && messages->layout.columns == 32
        && messages->layout.rows == 12 && messages->layout.width_dip == 36
        && messages->layout.height_dip == 10, "Messages uses actual 16x6 chart at source fabric 2x density");
    auto messagesSpec = ThemeSpec("messages");
    auto fabricLook = messagesSpec.chart->appearance;
    fabricLook.flags &= ~ChartRoundDots;
    auto squareDots = messagesSpec;
    squareDots.chart = std::make_shared<const Chart>("messages-square", 0, messagesSpec.chart->width,
        messagesSpec.chart->height, messagesSpec.chart->argbCells, fabricLook);
    Require(cache.Get(squareDots)->image.pixels != messages->image.pixels,
        "Round-dot mask changes actual fabric boundaries, not just catalogue metadata");
    fabricLook.flags = messagesSpec.chart->appearance.flags & ~ChartDefinedYarn;
    auto fineYarn = messagesSpec;
    fineYarn.chart = std::make_shared<const Chart>("messages-fine", 0, messagesSpec.chart->width,
        messagesSpec.chart->height, messagesSpec.chart->argbCells, fabricLook);
    Require(cache.Get(fineYarn)->image.pixels != messages->image.pixels,
        "Defined-yarn flag changes per-pixel stitch detail without replacing chart dimensions");
    auto chrome = cache.Get(ThemeSpec("google-chrome"));
    Require(chrome->layout.columns == 112 && chrome->layout.width_dip == 252,
        "Literal 112-column Chrome repeat is not replaced by an 8x8 stand-in");
    for (std::size_t i = 0; i < ws_design_chart_count(); ++i) {
        auto source = ws_design_chart_at(i);
        auto copied = MakeDesignChart(*source, source->name);
        bool exact = true;
        for (std::uint32_t y = 0; y < source->height; ++y)
            for (std::uint32_t x = 0; x < source->width; ++x)
                exact = exact && copied->argbCells[static_cast<std::size_t>(y)*source->width+x]
                    == ws_design_cell_argb(source, x, y);
        Require(exact && copied->width == source->width && copied->height == source->height
            && copied->appearance.flags == source->flags && copied->appearance.cornerArgb == source->corner_argb
            && copied->appearance.cuffArgb == source->cuff_argb, "All catalogue charts preserve cells, palette and flags when copied");
        auto custom = TileSpec{}; custom.chart = copied;
        Require(!cache.Get(custom)->image.pixels.empty(), "All catalogue chart repeats render through production");
    }
    auto notes = ThemeSpec("notes");
    Require(std::none_of(notes.chart->argbCells.begin(), notes.chart->argbCells.end(),
        [](auto cell) { return cell == 0xffefc852u; })
        && notes.chart->argbCells.back() == 0xfffffcf3u && notes.chart->appearance.cuffArgb == 0xffefc852u,
        "Notes cuff cells are source-suppressed; curved golden material remains separate");
    ChartAppearance raised{ChartSculptedYarn | ChartDefinedYarn};
    TileSpec custom;
    custom.chart = std::make_shared<const Chart>("imported:literal", 3, 2, 2,
        std::vector<std::uint32_t>{0xff102030u, 0xffe0b070u, 0u, 0x7fffff00u}, raised);
    auto owned = cache.Get(custom);
    auto recolored = custom; recolored.baseArgb = 0xffd1495bu;
    auto recoloredTile = cache.Get(recolored);
    Require(recoloredTile->spec.chart->argbCells == custom.chart->argbCells,
        "Changing base yarn never recolors literal custom PNG cells");
    auto fullColor = custom;
    fullColor.chart = std::make_shared<const Chart>("imported:opaque", 0, 2, 2,
        std::vector<std::uint32_t>{0xff102030u, 0xffe0b070u, 0xff123456u, 0xff789abcu}, raised);
    auto fixed = cache.Get(fullColor);
    fullColor.baseArgb = 0xffee1199u;
    Require(cache.Get(fullColor)->spec.chart->argbCells == fixed->spec.chart->argbCells,
        "Opaque imported palette retained when source ground color changes");
    auto change = [&](std::shared_ptr<const Chart> chart) {
        auto changed = custom; changed.chart = std::move(chart);
        Require(cache.Get(changed) != owned, "Chart identity, revision, content, dimensions and appearance invalidate key");
    };
    change(std::make_shared<const Chart>("imported:other", 3, 2, 2, custom.chart->argbCells, raised));
    change(std::make_shared<const Chart>("imported:literal", 4, 2, 2, custom.chart->argbCells, raised));
    auto cells = custom.chart->argbCells; cells[0] = 0xff0a141eu;
    change(std::make_shared<const Chart>("imported:literal", 3, 2, 2, cells, raised));
    change(std::make_shared<const Chart>("imported:literal", 3, 1, 4, custom.chart->argbCells, raised));
    auto look = raised; look.outsideAnchored = true;
    change(std::make_shared<const Chart>("imported:literal", 3, 2, 2, custom.chart->argbCells, look));
    for (auto flag : {ChartRoundDots, ChartFittedRepeat, ChartSolidCorners, ChartOuterCuff}) {
        look = raised; look.flags |= flag;
        if (flag == ChartOuterCuff) look.cuffArgb = 0xffefc852u;
        change(std::make_shared<const Chart>("imported:literal", 3, 2, 2, custom.chart->argbCells, look));
    }
    look = raised; look.cornerArgb = 0xff112233u;
    change(std::make_shared<const Chart>("imported:literal", 3, 2, 2, custom.chart->argbCells, look));
    auto changedCuff = notes;
    auto cuffLook = notes.chart->appearance; cuffLook.cuffArgb = 0xffe55c52u;
    changedCuff.chart = std::make_shared<const Chart>(notes.chart->semanticId,
        notes.chart->contentRevision, notes.chart->width, notes.chart->height, notes.chart->argbCells, cuffLook);
    Require(cache.Get(changedCuff) != cache.Get(notes), "Changing only cuff color invalidates complete chart appearance key");
    TileCache bounded(owned->ByteSize());
    auto held = bounded.Get(custom); auto heldPixels = held->image.pixels;
    custom.chart.reset(); bounded.Get(recolored); bounded.Clear();
    Require(held->spec.chart && held->spec.chart->contentRevision == 3
        && held->image.pixels == heldPixels && bounded.ResidentBytes() == 0,
        "Eviction and producer release preserve immutable chart/tile ownership");
    TileCache chartBudget(owned->image.ByteSize() + owned->height.ByteSize());
    chartBudget.Get(recolored);
    Require(chartBudget.ResidentBytes() == 0, "Byte cache bounds include owned chart data, not only raster");
    for (float scale : {MinimumDeviceScale, 1.f, 1.25f, 1.5f, 2.f, MaximumDeviceScale}) {
        KnitFrameSpec frame;
        frame.widthDip = 73; frame.heightDip = 61; frame.deviceScale = scale;
        const auto reused = KnitRaster::RenderPerimeter(frame, spotify, cache);
        TileCache fresh;
        const auto independent = KnitRaster::RenderPerimeter(frame, spotify, fresh);
        Require(reused.tile == tile && reused.deviceScale == scale
            && reused.tile->layout.width_dip == 27 && reused.tile->image.width == 108,
            "DPI frame requests share chart tiles and preserve source stitch gauge and raster sampling");
        Require(reused.tile->image.pixels == independent.tile->image.pixels
            && reused.tile->height.pixels == independent.tile->height.pixels
            && KnitRaster::Compose(reused).pixels == KnitRaster::Compose(independent).pixels,
            "DPI chart cache reuse matches independently generated tile and perimeter pixels");
    }
    Invalid([&] { Chart("", 0, 1, 1, {0}); }, "Empty chart identity rejected");
    Invalid([&] { Chart("bad", 0, 0, 1, {}); }, "Empty chart dimensions rejected");
    Invalid([&] { Chart("bad", 0, 257, 1, {}); }, "Unbounded chart dimensions rejected");
    Invalid([&] { Chart("bad", 0, 2, 2, {0}); }, "Truncated chart cells rejected");
    look = {}; look.flags = 1u << 31;
    Invalid([&] { Chart("bad", 0, 1, 1, {0}, look); }, "Unknown appearance flags rejected");
    look = {}; look.flags = ChartOuterCuff;
    Invalid([&] { Chart("bad", 0, 1, 1, {0}, look); }, "Missing outer cuff color rejected");
    auto enormous = spotify; enormous.bandDip = 256; enormous.gauge.rows = 1.5f; enormous.gauge.aspect = 8;
    auto count = cache.GenerationCount();
    Invalid([&] { cache.Get(enormous); }, "Oversized chart raster rejected before allocation");
    Require(cache.GenerationCount() == count, "Invalid chart raster cannot commit a cache generation");
    enormous = spotify; enormous.gauge.aspect = std::numeric_limits<float>::denorm_min();
    Invalid([&] { cache.Get(enormous); }, "Subnormal chart pitch cannot overflow repeat float-to-int conversion");
    enormous = spotify; enormous.gauge.aspect = .000001f;
    Invalid([&] { cache.Get(enormous); }, "Microscopic chart pitch cannot allocate unbounded yarn paths");
    std::vector<std::uint32_t> palette;
    for (std::uint32_t i = 0; i < 17; ++i) palette.push_back(0xff123400u | i);
    auto tooMany = TileSpec{};
    tooMany.chart = std::make_shared<const Chart>("17-yarns", 0, 17, 1, palette, raised);
    Invalid([&] { cache.Get(tooMany); }, "Raised chart rejects seventeenth yarn instead of silently recoloring");
    tooMany.chart = std::make_shared<const Chart>("17-fabric-yarns", 0, 17, 1, palette);
    Require(!cache.Get(tooMany)->image.pixels.empty(), "Per-pixel fabric accepts full literal palette without source path cap");
    TileSpec sentinel;
    sentinel.chart = std::make_shared<const Chart>("sentinel", 0, 1, 1,
        std::vector<std::uint32_t>{0}, raised);
    auto sentinelImage = cache.Get(sentinel)->image;
    sentinel.chart = std::make_shared<const Chart>("alpha127", 0, 1, 1,
        std::vector<std::uint32_t>{0x7fffff00u}, raised);
    Require(cache.Get(sentinel)->image.pixels == sentinelImage.pixels,
        "Alpha127 imported cell renders base yarn, not a recolored contrast");
    sentinel.chart = std::make_shared<const Chart>("alpha128", 0, 1, 1,
        std::vector<std::uint32_t>{0x80ffff00u}, raised);
    auto opaque128 = cache.Get(sentinel);
    Require(opaque128->image.pixels != sentinelImage.pixels
        && std::all_of(opaque128->image.pixels.begin(), opaque128->image.pixels.end(),
            [](auto p) { return Alpha(p) == 255; }),
        "Alpha128 is literal opaque yarn rather than transparency or base sentinel");
    TileSpec paletteFixture; paletteFixture.baseArgb = 0xff010101u;
    paletteFixture.chart = std::make_shared<const Chart>("literal-red-blue", 0, 2, 1,
        std::vector<std::uint32_t>{0xffff0000u, 0xff0000ffu}, raised);
    auto pixels = cache.Get(paletteFixture)->image.pixels;
    Require(std::any_of(pixels.begin(), pixels.end(), [](auto p) {
        return ((p>>16)&255)>100 && (p&255)<10;
    }) && std::any_of(pixels.begin(), pixels.end(), [](auto p) {
        return (p&255)>100 && ((p>>16)&255)<10;
    }), "Actual custom PNG raster contains both literal contrast hues, not a substituted base palette");
    auto pathLimit = ThemeSpec("spotify");
    pathLimit.chart = std::make_shared<const Chart>("path-pressure", 0, 256, 256,
        std::vector<std::uint32_t>(256*256), raised);
    pathLimit.bandDip = 2; pathLimit.gauge.rows = 128; pathLimit.gauge.aspect = 1;
    Invalid([&] { cache.Get(pathLimit); }, "Expanded small stitches obey bounded CPU yarn-path budget");
    auto dense = pathLimit;
    dense.chart = std::make_shared<const Chart>("dense-1x1", 0, 1, 1,
        std::vector<std::uint32_t>{0}, raised);
    dense.gauge.aspect = 1.28f;
    auto denseTile = cache.Get(dense);
    Require(denseTile->layout.columns == 400 && denseTile->layout.rows == 512
        && denseTile->image.width == 32 && denseTile->image.height == 32,
        "Dense valid source chart renders exact400x512 repeat within262144 margin-cell limit");
    Require(std::all_of(denseTile->image.pixels.begin(), denseTile->image.pixels.end(),
        [](auto p) { return Alpha(p) == 255; }),
        "Dense208464-cell source raster remains opaque after one bounded YarnCell reservation");
    std::vector<std::uint32_t> adversarial(256*256);
    std::iota(adversarial.begin(), adversarial.end(), 0xff000000u);
    TileSpec rejection;
    rejection.chart = std::make_shared<const Chart>("adversarial-256x256", 0, 256, 256,
        std::move(adversarial), raised);
    bool beforeWic = false;
    auto started = std::chrono::steady_clock::now();
    std::thread withoutCom([&] {
        // No COM here on purpose: a WIC allocation would throw system_error, not invalid_argument.
        try { TileCache isolated; isolated.Get(rejection); }
        catch (const std::invalid_argument&) { beforeWic = true; }
        catch (const std::system_error&) {}
    });
    withoutCom.join();
    double elapsed = std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-started).count();
    Require(beforeWic, "Adversarial65536-color raised chart rejects seventeenth distinct yarn before COM/WIC");
    Require(elapsed < 500., "Palette admission is bounded at16 comparisons per cell, never quadratic in65536 colors");
    std::cout << "adversarial65536-color chart rejection before WIC: " << elapsed << "ms\n";
    std::vector<std::uint32_t> sixteen(16);
    std::iota(sixteen.begin(), sixteen.end(), 0xff102030u);
    auto boundary = rejection;
    boundary.chart = std::make_shared<const Chart>("16-opaque-yarns",0,16,1,sixteen,raised);
    Require(!cache.Get(boundary)->image.pixels.empty(),
        "Sixteen opaque authored yarns pass; unused base-colored ground is not a seventeenth path");
    sixteen.back() = 0;
    boundary.chart = std::make_shared<const Chart>("15-plus-base-yarns",0,16,1,sixteen,raised);
    Require(!cache.Get(boundary)->image.pixels.empty(), "Fifteen authored colors plus used base sentinel fit16-yarn limit");
    sixteen.back() = 0xff10203fu; sixteen.push_back(0);
    boundary.chart = std::make_shared<const Chart>("16-plus-base-yarns",0,17,1,sixteen,raised);
    Invalid([&] { cache.Get(boundary); }, "Sixteen authored colors plus used base sentinel reject17th effective yarn");
}

void CatalogueFixtures(const std::filesystem::path& root) {
    using namespace window_sweaters::designs;
    TileCache cache;
    auto output = root / L"outputs";
    for (std::size_t i = 0; i < ws_design_theme_count(); ++i) {
        auto theme = ws_design_theme_at(i);
        auto spec = ThemeSpec(theme->id);
        for (float scale : {1.f, 2.f}) {
            KnitFrameSpec frame; frame.deviceScale = scale;
            auto perimeter = KnitRaster::RenderPerimeter(frame, spec, cache);
            auto name = std::string(theme->id) + (scale == 1.f ? "-1x.png" : "-2x.png");
            SavePng(KnitRaster::Compose(perimeter), (output / name).c_str());
            auto image = BandImage(perimeter);
            Require(image.width == static_cast<std::uint32_t>(640*scale)
                && image.height == static_cast<std::uint32_t>(160*scale)
                && Alpha(Pixel(image, image.width/2, image.height/2)) == 0
                && Alpha(Pixel(image, image.width/2, 2)) == 255,
                "All catalogue themes render opaque bands and transparent content at 1x/2x");
            Premultiplied(image);
        }
        for (float scale : {1.f, 2.f})
            for (float radius : {0.f, 10.f, 28.f, 200.f}) {
                KnitFrameSpec f; f.widthDip = 180; f.heightDip = 110;
                f.deviceScale = scale; f.radiusDip = radius;
                auto image = BandFrame(f, spec, cache);
                float r = std::min(radius, 55.f), tolerance = 1.f / scale;
                float outerTolerance = tolerance + ws_knit_fray_depth(10.f);
                bool valid = true;
                for (std::uint32_t y = 0; y < image.height; ++y)
                    for (std::uint32_t x = 0; x < image.width; ++x) {
                        float dx = (x+.5f)/scale, dy = (y+.5f)/scale;
                        bool outer = InsideRounded(dx, dy, outerTolerance, outerTolerance,
                            200-2*outerTolerance, 130-2*outerTolerance, r+10-outerTolerance);
                        bool hole = InsideRounded(dx, dy, 11-tolerance, 11-tolerance,
                            178+2*tolerance, 108+2*tolerance, std::max(0.f, r-1)+tolerance);
                        if (outer && !hole && Alpha(Pixel(image, x, y)) != 255) valid = false;
                    }
                Require(valid, "All catalogue designs preserve opaque mitres at 1x/2x and square/rounded/clamped radii");
            }
    }

    auto whatsapp = ThemeSpec("whatsapp");
    KnitFrameSpec frame; frame.deviceScale = 2;
    auto actual = BandFrame(frame, whatsapp, cache);
    auto patch = TileSpec{}; patch.material = Material::Raised; patch.baseArgb = whatsapp.baseArgb;
    ChartAppearance capLook{ChartSculptedYarn}; capLook.outsideAnchored = true;
    patch.chart = std::make_shared<const Chart>("solid-cap-fixture", 0, 8, 8,
        std::vector<std::uint32_t>(64), capLook);
    auto patchOnly = BandFrame(frame, patch, cache);
    Require(Pixel(actual, 30, 8) == Pixel(patchOnly, 30, 8)
        && Pixel(actual, 100, 8) != Pixel(patchOnly, 100, 8),
        "WhatsApp curved tangent caps use solid raised base yarn while straight edge retains stripes");
    auto square = frame; square.widthDip = 20; square.heightDip = 20; square.radiusDip = 10;
    auto allCaps = KnitRaster::RenderFrame(square, whatsapp, cache);
    Require(allCaps.pixels == KnitRaster::RenderFrame(square, patch, cache).pixels,
        "A short WhatsApp edge with no tangent interval is wholly solid, without partial motif");
    for (auto id : {"notes", "calendar"}) {
        auto spec = ThemeSpec(id);
        auto image = BandFrame(frame, spec, cache);
        auto isCuff = [&](auto p) {
            float r = static_cast<float>((p >> 16) & 255);
            float green = static_cast<float>((p >> 8) & 255);
            float blue = static_cast<float>(p & 255);
            return r > 2.3f * blue && (std::string(id) == "calendar" ? r > 2.3f * green : green > 2.f * blue);
        };
        Require(isCuff(Pixel(image, 100, 2)) && !isCuff(Pixel(image, 100, 10)),
            "Notes/Calendar outer cuff is one third band, not a full-edge recolor");
        Require(isCuff(Pixel(image, 14, 14)),
            "Curved outer cuff reaches the arc, not disconnected rectangular strips");
    }
    auto fitted = ThemeSpec("finder");
    auto fittedImage = KnitRaster::RenderFrame(frame, fitted, cache);
    auto noFit = fitted.chart->appearance; noFit.flags &= ~ChartFittedRepeat;
    auto free = fitted; free.chart = std::make_shared<const Chart>("finder-free", 0,
        fitted.chart->width, fitted.chart->height, fitted.chart->argbCells, noFit);
    Require(fittedImage.pixels != KnitRaster::RenderFrame(frame, free, cache).pixels,
        "Fitted flag changes real along-edge motif placement");
    for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        auto fitFrame = frame; fitFrame.deviceScale = scale;
        fitFrame.widthDip = 107.25f; fitFrame.heightDip = 31.75f; fitFrame.radiusDip = 1000;
        auto image = KnitRaster::RenderFrame(fitFrame, whatsapp, cache);
        Require(std::any_of(image.pixels.begin(), image.pixels.end(), [](auto p) { return Alpha(p) == 255; }),
            "Fractional short-edge tangent fits retain covered opaque yarn");
    }
    auto physical = frame; physical.deviceScale = 1.5f;
    physical.contentWidthPixels = 1000; physical.contentHeightPixels = 241;
    physical.physicalPaddingPixels = 16;
    auto authority = KnitRaster::RenderPerimeter(physical, ThemeSpec("notes"), cache);
    Require(authority.width == 1048 && authority.height == 289 && authority.contentInsetPixels == 24,
        "Chart/cuff rendering preserves authoritative physical content and one-pixel padding override");
    for (auto stitch : {Stitch::Rib, Stitch::Garter}) {
        TileSpec spec; spec.stitch = stitch;
        SavePng(KnitRaster::RenderFrame(frame, spec, cache),
            (output / (stitch == Stitch::Rib ? L"rib-2x.png" : L"garter-2x.png")).c_str());
    }
    std::cout << "catalogue fixtures: " << ws_design_theme_count() << " themes x 1x/2x, cache bytes=" << cache.ResidentBytes() << "\n";
}

void SpecialFinishContracts() {
    using namespace window_sweaters::designs;
    TileCache cache;
    auto redYarn = [](std::uint32_t p) { return ((p>>16)&255) > 2*((p>>8)&255); };
    TileSpec patch;
    ChartAppearance look{ChartSolidCorners, 0xffff0000u};
    patch.baseArgb = 0xffffffffu;
    patch.chart = std::make_shared<const Chart>("patch-boundary-test", 0, 6, 6,
        std::vector<std::uint32_t>(36, 0xffffffffu), look);
    for (float scale : {1.f, 2.f})
        for (auto anchor : {KnitAnchor::Corner, KnitAnchor::Centre})
            for (auto size : {std::array<float,2>{180,110}, {187,137}, {299,173}}) {
                KnitFrameSpec f; f.widthDip = size[0]; f.heightDip = size[1];
                f.deviceScale = scale; f.anchor = anchor;
                auto image = BandFrame(f, patch, cache);
                bool exact = true; std::uint32_t minimum = 255, maximum = 0;
                for (int side = 0; side < 4; ++side) {
                    int length = static_cast<int>((side%2 ? size[1] : size[0])+20);
                    for (int offset = static_cast<int>(15*scale); offset < (length-15)*scale; ++offset) {
                        int x = side%2 ? static_cast<int>((side==1 ? size[0]+15 : 5)*scale) : offset;
                        int y = side%2 ? offset : static_cast<int>((side==2 ? size[1]+15 : 5)*scale);
                        auto p = Pixel(image, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                        bool expected = offset < 20*scale || offset >= (length-20)*scale;
                        exact = exact && Alpha(p) == 255 && redYarn(p) == expected;
                        if (expected) { minimum = std::min(minimum, (p>>16)&255); maximum = std::max(maximum, (p>>16)&255); }
                    }
                }
                Require(exact, "Original contrasting patch test: equal tangent extents on four sides, both anchors, 1x/2x");
                Require(maximum-minimum > 8, "Solid tangent patches retain real raised-yarn relief");
            }
    TileSpec cuff; cuff.baseArgb = 0xffeeeeeeu;
    look = {}; look.flags = ChartOuterCuff | ChartSculptedYarn | ChartDefinedYarn; look.cuffArgb = 0xffff0000u;
    cuff.chart = std::make_shared<const Chart>("curved-cuff-test", 0, 1, 1,
        std::vector<std::uint32_t>{0}, look);
    for (float scale : {1.f, 2.f})
        for (float radius : {0.f,10.f,28.f}) {
            KnitFrameSpec f; f.widthDip = 180; f.heightDip = 110; f.radiusDip = radius; f.deviceScale = scale;
            auto image = BandFrame(f, cuff, cache);
            bool continuous = true, noIslands = true;
            float width = 10.f/3, tolerance = .75f/scale;
            for (std::uint32_t y = 0; y < image.height; ++y)
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    float dx=(x+.5f)/scale,dy=(y+.5f)/scale;
                    float rim = tolerance + ws_knit_fray_depth(10.f);
                    bool outer = InsideRounded(dx,dy,rim,rim,200-2*rim,130-2*rim,radius+10-rim);
                    bool insetExpanded = InsideRounded(dx,dy,width-tolerance,width-tolerance,
                        200-2*width+2*tolerance,130-2*width+2*tolerance,radius+10-width+tolerance);
                    bool insetContracted = InsideRounded(dx,dy,width+tolerance,width+tolerance,
                        200-2*width-2*tolerance,130-2*width-2*tolerance,radius+10-width-tolerance);
                    auto p=Pixel(image,x,y);
                    if (outer && !insetExpanded && !redYarn(p)) continuous=false;
                    if (insetContracted && Alpha(p)==255 && redYarn(p)) noIslands=false;
                }
            Require(continuous && noIslands, "Original cuff test: one-third curved band, no interior color islands, three radii at 1x/2x");
        }
    for (float band : {2.f, 10.f, 14.f, 26.f})
        for (float scale : {1.f, 2.f}) {
            KnitFrameSpec f; f.bandDip=band; f.deviceScale=scale; f.widthDip=180; f.heightDip=110;
            auto perimeter=KnitRaster::RenderPerimeter(f, ThemeSpec("messages"), cache);
            auto image=BandImage(perimeter);
            auto bandPixels=static_cast<std::uint32_t>(band*scale);
            Require(image.width==static_cast<std::uint32_t>((180+2*band)*scale)
                && image.height==static_cast<std::uint32_t>((110+2*band)*scale)
                && perimeter.shadowMarginPixels==(bandPixels+1)/2,
                "Narrow/wide fabric maintains exact physical band, content and half-band shadow margin");
        }
}

RasterImage Crop(const RasterImage& image, std::uint32_t x, std::uint32_t y,
                 std::uint32_t width, std::uint32_t height) {
    RasterImage crop{width, height, width*4, {}};
    crop.pixels.reserve(static_cast<std::size_t>(width)*height);
    for (std::uint32_t row = 0; row < height; ++row)
        for (std::uint32_t column = 0; column < width; ++column)
            crop.pixels.push_back(Pixel(image, x+column, y+row));
    return crop;
}

std::array<double, 4> TextureMetrics(const RasterImage& source, const RasterImage& candidate) {
    auto gray = [](std::uint32_t p) {
        return .2126*((p >> 16) & 255) + .7152*((p >> 8) & 255) + .0722*(p & 255);
    };
    auto chroma = [](std::uint32_t p, int channel) {
        double sum = ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255) + 1.;
        return 255. * ((p >> (channel * 8)) & 255) / sum;
    };
    double absolute = 0, sourceEnergy = 0, candidateEnergy = 0, n = 0;
    double sa = 0, sb = 0, saa = 0, sbb = 0;
    for (std::uint32_t y = 1; y < source.height; ++y)
        for (std::uint32_t x = 1; x < source.width; ++x) {
            auto a = Pixel(source, x, y), b = Pixel(candidate, x, y);
            if (Alpha(b) != 255 || Alpha(Pixel(candidate, x-1, y)) != 255 || Alpha(Pixel(candidate, x, y-1)) != 255) continue;
            ++n;
            double la = gray(a), lb = gray(b);
            sa += la; sb += lb; saa += la*la; sbb += lb*lb;
            for (int c = 0; c < 3; ++c) {
                absolute += std::abs(chroma(a, c) - chroma(b, c));
                sourceEnergy += std::abs(chroma(a, c) - chroma(Pixel(source, x-1, y), c))
                    + std::abs(chroma(a, c) - chroma(Pixel(source, x, y-1), c));
                candidateEnergy += std::abs(chroma(b, c) - chroma(Pixel(candidate, x-1, y), c))
                    + std::abs(chroma(b, c) - chroma(Pixel(candidate, x, y-1), c));
            }
        }
    Require(n > 0, "Texture metric has opaque candidate pixels");
    double sourceSpread = std::sqrt(saa/n - sa*sa/n/n), candidateSpread = std::sqrt(sbb/n - sb*sb/n/n);
    return {absolute/(3*n), candidateEnergy/sourceEnergy, sb/sa, candidateSpread/sourceSpread};
}

RasterImage RingLit(RasterImage image, float originX, float originY, float scale) {
    const ws_knit_ring ring{640.f, 160.f, 20.f, 11.f, 9.f};
    Require(ws_knit_shade_ring(&ring, originX, originY, scale, image.pixels.data(),
        static_cast<int>(image.width), static_cast<int>(image.height)) == 1, "Ring light applies to reference plate");
    return image;
}

void SourceCatalogueParity(const std::filesystem::path& root) {
    struct Example { const char* id; const wchar_t* plate; };
    const std::array<Example, 12> examples = {{
        {"finder", L"app-01.png"}, {"spotify", L"app-05.png"},
        {"whatsapp", L"app-07.png"}, {"messages", L"app-21.png"},
        {"notes", L"app-23.png"}, {"calendar", L"app-24.png"},
        {"adobe-photoshop", L"app-34.png"}, {"google-chrome", L"app-09.png"},
        {"grok-bot", L"app-12.png"}, {"granola", L"app-14.png"},
        {"adobe-illustrator", L"app-35.png"}, {"terminal", L"app-36.png"}
    }};
    TileCache cache;
    auto output = root / L"outputs";
    for (auto example : examples) {
        auto spec = ThemeSpec(example.id);
        auto plate = LoadPng(root / L"references" / example.plate);
        KnitFrameSpec frame; frame.deviceScale = 8;
        auto enlarged = BandFrame(frame, spec, cache);
        auto detail = Crop(enlarged, 496, 0, 1280, 80);
        auto baseline = RingLit(Crop(plate, 76, 602, 1280, 80), 62.f, 0.f, 8.f);
        SavePng(detail, (output / (std::string(example.id) + "-production-detail.png")).c_str());
        SavePng(baseline, (output / (std::string(example.id) + "-source-detail.png")).c_str());
        auto metric = TextureMetrics(baseline, detail);
        std::cout << "source " << example.id << " detail: chromaMAE=" << metric[0]
            << ", chromaEdgeRatio=" << metric[1] << ", meanLumaRatio=" << metric[2]
            << ", lumaSpreadRatio=" << metric[3] << "\n";
        Require(metric[0] < 4.,
            "Actual source plate versus production detail keeps authored yarn colours within AA tolerance");
        Require(metric[1] > .60 && metric[1] < 1.8,
            "Source-matched material retains motif and stitch colour edges, not flattened placeholder fabric");
        Require(metric[2] > .90 && metric[2] < 1.15,
            "Screen-space stitch light keeps yarn as bright as the authored plate");
        Require(metric[3] > .85 && metric[3] < 1.8,
            "Stitch relief is at least as strong as the authored plate without harsh contrast");
        frame.deviceScale = 4;
        auto corners = BandFrame(frame, spec, cache);
        const std::array<std::uint32_t, 4> cx = {0, 2400, 2400, 0}, cy = {0, 0, 480, 480};
        for (std::size_t corner = 0; corner < 4; ++corner) {
            auto native = Crop(corners, cx[corner], cy[corner], 160, 160);
            auto source = Crop(plate, static_cast<std::uint32_t>(116+corner*332), 780, 160, 160);
            auto name = std::string(example.id) + "-corner-" + std::to_string(corner);
            SavePng(native, (output / (name + "-native.png")).c_str());
            SavePng(source, (output / (name + "-source.png")).c_str());
            std::size_t count = 0, disagreements = 0;
            double colorError = 0; std::size_t colorCount = 0;
            for (std::uint32_t y = 0; y < 160; ++y)
                for (std::uint32_t x = 0; x < 160; ++x) {
                    float dx = (cx[corner]+x+.5f)/4, dy = (cy[corner]+y+.5f)/4;
                    if (InsideRounded(dx, dy, 9.75f, 9.75f, 620.5f, 140.5f, 10.25f)) continue;
                    const float fray = ws_knit_fray_depth(10.f);
                    if (InsideRounded(dx, dy, 0.f, 0.f, 640.f, 160.f, 20.f)
                        && !InsideRounded(dx, dy, fray, fray, 640.f - 2*fray, 160.f - 2*fray, 20.f - fray)) continue;
                    auto a = Pixel(source, x, y), b = Pixel(native, x, y);
                    int backgroundDistance = std::abs(static_cast<int>((a>>16)&255)-249)
                        + std::abs(static_cast<int>((a>>8)&255)-247) + std::abs(static_cast<int>(a&255)-243);
                    bool sourceYarn = backgroundDistance > 3, nativeYarn = Alpha(b) > 128;
                    ++count; if (sourceYarn != nativeYarn) ++disagreements;
                    bool outerInterior = InsideRounded(dx,dy,.25f,.25f,639.5f,159.5f,19.75f);
                    bool contentExpanded = InsideRounded(dx,dy,9.5f,9.5f,621.f,141.f,10.5f);
                    if (sourceYarn && Alpha(b) == 255 && outerInterior && !contentExpanded) {
                        double sumA = ((a>>16)&255) + ((a>>8)&255) + (a&255) + 1., sumB = ((b>>16)&255) + ((b>>8)&255) + (b&255) + 1.;
                        for (int shift = 0; shift < 24; shift += 8)
                            colorError += std::abs(255.*((a>>shift)&255)/sumA - 255.*((b>>shift)&255)/sumB);
                        colorCount += 3;
                    }
                }
            double disagreement = static_cast<double>(disagreements)/static_cast<double>(count);
            double mae = colorError/static_cast<double>(colorCount);
            std::cout << "source " << example.id << " corner " << corner << ": geometry="
                << disagreement*100 << "%, opaque chroma MAE=" << mae << "\n";
            Require(disagreement < .01, "All four enlarged source corners preserve exterior geometry");
            Require(mae < 4., "Special cap/cuff/fitted source corner yarn keeps authored colors and material");
        }

    }
}

void StitchLightConsistency() {
    TileCache cache;
    TileSpec tile; tile.material = Material::Raised; tile.baseArgb = 0xff7aa86au;
    KnitFrameSpec f; f.widthDip = 220; f.heightDip = 140; f.deviceScale = 4;
    auto perimeter = KnitRaster::RenderPerimeter(f, tile, cache);
    const auto& t = *perimeter.tile;
    float dev = f.deviceScale, band = f.bandDip, ow = f.widthDip + 2*band, oh = f.heightDip + 2*band;
    float gutter = static_cast<float>(perimeter.contentInsetPixels) / dev - band;
    float py = band*.5f - t.layout.height_dip*.5f, rs = static_cast<float>(t.layout.raster_scale);
    const float lx = -.26f, ly = -.42f, ln = std::hypot(lx, ly);
    auto tileFrame = [&](int side, float fx, float fy) {
        switch (side) {
            case 0: return std::array<float, 2>{fx, fy};
            case 1: return std::array<float, 2>{fy, ow - fx};
            case 2: return std::array<float, 2>{ow - fx, oh - fy};
            default: return std::array<float, 2>{oh - fy, fx};
        }
    };
    auto height = [&](int side, float fx, float fy) {
        auto q = tileFrame(side, fx, fy);
        auto wrap = [](float v, std::uint32_t n) {
            auto i = static_cast<long long>(std::floor(v)) % static_cast<long long>(n);
            return static_cast<std::uint32_t>(i < 0 ? i + n : i);
        };
        return (Pixel(t.height, wrap(q[0]*rs, t.height.width), wrap((q[1]-py)*rs, t.height.height)) & 255) / 255.;
    };
    std::array<double, 4> correlation{};
    for (int side = 0; side < 4; ++side) {
        const auto& piece = perimeter.pieces[side];
        double n = 0, sl = 0, sg = 0, sll = 0, sgg = 0, slg = 0;
        for (std::uint32_t y = 0; y < piece.image.height; ++y)
            for (std::uint32_t x = 0; x < piece.image.width; ++x) {
                auto p = Pixel(piece.image, x, y);
                float fx = (x + .5f + piece.x) / dev - gutter, fy = (y + .5f + piece.y) / dev - gutter;
                auto q = tileFrame(side, fx, fy);
                float length = side % 2 ? oh : ow;
                if (Alpha(p) != 255 || q[1] < band*.3f || q[1] > band*.7f
                    || q[0] < 40.f || q[0] > length - 40.f) continue;
                float e = .75f / dev;
                double g = height(side, fx + lx/ln*e, fy + ly/ln*e) - height(side, fx - lx/ln*e, fy - ly/ln*e);
                double l = .2126*((p >> 16) & 255) + .7152*((p >> 8) & 255) + .0722*(p & 255);
                ++n; sl += l; sg += g; sll += l*l; sgg += g*g; slg += l*g;
            }
        Require(n > 500, "Stitch light sample covers each side");
        correlation[side] = (slg - sl*sg/n) / std::sqrt((sll - sl*sl/n) * (sgg - sg*sg/n));
    }
    std::cout << "stitch light/slope correlation top/right/bottom/left: " << correlation[0] << "/"
        << correlation[1] << "/" << correlation[2] << "/" << correlation[3] << "\n";
    for (auto c : correlation)
        Require(c < -.15, "Stitch legs facing the upper-left light are brighter on every side");
    Require(std::abs(correlation[0] - correlation[2]) < .2 && std::abs(correlation[1] - correlation[3]) < .2,
        "Opposite sides agree on stitch light strength");
}

void RendererMetrics() {
    TileCache cache;
    KnitFrameSpec f; f.widthDip=620; f.heightDip=140; f.deviceScale=2;
    for (auto id : {"spotify","messages","notes"}) {
        auto spec=ThemeSpec(id);
        auto start=std::chrono::steady_clock::now();
        auto perimeter=KnitRaster::RenderPerimeter(f,spec,cache);
        double cold=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        start=std::chrono::steady_clock::now();
        constexpr int iterations=10;
        for(int i=0;i<iterations;++i) KnitRaster::RenderPerimeter(f,spec,cache);
        double warm=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/iterations;
        KnitRasterScratch scratch;
        KnitRaster::RenderPerimeter(f,spec,cache,{&scratch});
        start=std::chrono::steady_clock::now();
        for(int i=0;i<iterations;++i) KnitRaster::RenderPerimeter(f,spec,cache,{&scratch});
        double reused=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/iterations;
        std::size_t bytes=0;for(const auto& piece:perimeter.pieces)bytes+=piece.image.ByteSize();
        std::cout<<"measured "<<id<<" 620x140@2x: cold="<<cold<<"ms, warm="<<warm
            <<"ms, scratchWarm="<<reused<<"ms, scratchBytes="<<scratch.ResidentBytes()<<", narrowBytes="<<bytes<<", tileOwnedBytes="<<perimeter.tile->ByteSize()
            <<", cacheResident="<<cache.ResidentBytes()<<", generations="<<cache.GenerationCount()
            <<", hits="<<cache.HitCount()<<"\n";
    }
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
        if (argc > 2 && std::wstring(argv[2]) == L"--stitch-light") {
            StitchLightConsistency();
            std::cout << "PASS " << checks << " stitch light checks\n";
            CoUninitialize();
            return 0;
        }
        if (argc > 2 && std::wstring(argv[2]) == L"--metrics") { RendererMetrics(); CoUninitialize(); return 0; }
        PortableMath();
        CacheAndInvalidInputs();
        DpiTileReuse();
        ScratchContracts();
        RenderBehavior(root);
        ChartContracts();
        CatalogueFixtures(root);
        SpecialFinishContracts();
        StitchLightConsistency();
        SourceCatalogueParity(root);
        RendererMetrics();
        std::cout << "PASS " << checks << " native knitting checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << "\n";
        exitCode = 1;
    }
    CoUninitialize();
    return exitCode;
}
