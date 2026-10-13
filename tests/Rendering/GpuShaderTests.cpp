// SPDX-License-Identifier: GPL-3.0-only
#include "GpuReadback.h"
#include "GpuCacheTests.h"
#include "..\..\src\Rendering\GpuShader.h"
#include "..\..\src\Rendering\Overlay.h"
#include "..\..\src\Knitting\WicSurface.h"
#include "..\..\src\Host\RenderState.h"
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace window_sweaters;
using namespace rendering;
using namespace knitting;
using Microsoft::WRL::ComPtr;
using knitting::detail::Check;
namespace {
std::uint64_t checks{}, cases{}, channels{}, unequal{};
int maximum{};
std::uint64_t shaderCases{}, lightingPixels{}, reliefPixels{}, frayPixels{}, shadowPixels{};
int shaderMaximum{};
bool exactRasterizer{};
bool shaderOnly{};
void Require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void Bounds(const RasterImage& image) {
    for (const auto p : image.pixels) {
        const auto a = p >> 24;
        Require((p & 255) <= a && ((p >> 8) & 255) <= a && ((p >> 16) & 255) <= a,
            "Premultiplied channel exceeds alpha");
    }
}
RasterImage Difference(const RasterImage& cpu, const RasterImage& gpu, int& peak, std::size_t* outliers = nullptr) {
    Require(cpu.width == gpu.width && cpu.height == gpu.height, "Pixel dimensions differ");
    RasterImage diff{cpu.width, cpu.height, cpu.stride, {}};
    diff.pixels.resize(cpu.pixels.size());
    for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
        std::uint32_t out = 0xff000000;
        int pixelPeak = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            int d = std::abs(int((cpu.pixels[i] >> shift) & 255) - int((gpu.pixels[i] >> shift) & 255));
            peak = std::max(peak, d);
            pixelPeak = std::max(pixelPeak, d);
            ++channels;
            if (d) ++unequal;
            if (shift < 24) out |= std::uint32_t(std::min(255, d * 32)) << shift;
            else out |= std::uint32_t(std::min(255, d * 32));
        }
        if (outliers && pixelPeak > 96) ++*outliers;
        diff.pixels[i] = out;
    }
    return diff;
}
// Hardware D2D anti-aliases geometry and resolves nearest-neighbour texel ties differently from software
// D2D; WARP matches software D2D exactly and keeps the two-level gate.
bool WithinParity(int peak, std::size_t outliers, Sampling sampling) {
    if (exactRasterizer) return peak <= 2;
    return sampling == Sampling::Nearest ? outliers <= 64 : peak <= 96;
}
RasterImage SoftwareStrip(const KnitPerimeter& perimeter, std::size_t side, bool heights) {
    const auto& piece = perimeter.pieces[side];
    detail::WicSurface surface(piece.width, piece.height, perimeter.deviceScale);
    const KnitTileBrush brush = [&](int, const Tile& tile, bool height, float px, float py, float repeat) {
        const auto& image = height ? tile.height : tile.image;
        ComPtr<ID2D1Bitmap> bitmap;
        Check(surface.target->CreateBitmap(D2D1::SizeU(image.width, image.height), image.pixels.data(),
            image.stride, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * tile.layout.raster_scale, 96.f * tile.layout.raster_scale),
            &bitmap), "Software reference tile bitmap");
        ComPtr<ID2D1BitmapBrush> result;
        Check(surface.target->CreateBitmapBrush(bitmap.Get(), D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, tile.spec.sampling == Sampling::Nearest
                ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
            &result), "Software reference tile brush");
        result->SetTransform(D2D1::Matrix3x2F::Scale(repeat / tile.layout.width_dip, 1.f)
            * D2D1::Matrix3x2F::Translation(px, py));
        return result;
    };
    DrawKnitStrip(*surface.target.Get(), *perimeter.assembly, *perimeter.tile, piece,
        static_cast<int>(side), heights, brush);
    return surface.Read();
}
int PixelError(std::uint32_t a, std::uint32_t b) {
    int peak = 0;
    for (int shift = 0; shift < 32; shift += 8)
        peak = std::max(peak, std::abs(int((a >> shift) & 255) - int((b >> shift) & 255)));
    return peak;
}
std::uint64_t SignificantPixels(const RasterImage& a, const RasterImage& b) {
    std::uint64_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) count += PixelError(a.pixels[i], b.pixels[i]) > 2;
    return count;
}
void ShaderOnly(testing::Gpu& shader, const KnitPerimeter& perimeter, const std::filesystem::path& output,
                const std::string& name) {
    for (std::size_t side = 0; side < perimeter.pieces.size(); ++side) {
        const auto flat = SoftwareStrip(perimeter, side, false);
        const auto heights = SoftwareStrip(perimeter, side, true);
        auto reference = flat;
        const auto& p = perimeter.assembly->parameters[side];
        const ws_knit_ring ring{p.ringWidth, p.ringHeight, p.outerRadius, p.inset, p.innerRadius};
        const auto width = static_cast<int>(flat.width), height = static_cast<int>(flat.height);
        auto tubeOnly = flat;
        Require(ws_knit_shade_ring(&ring, p.originX, p.originY, p.scale, tubeOnly.pixels.data(), width, height) != 0,
            "CPU tube lighting reference");
        Require(ws_knit_shade_ring_relief(&ring, p.originX, p.originY, p.scale, reference.pixels.data(),
            width, height, heights.pixels.data(), p.relief) != 0, "CPU relief lighting reference");
        lightingPixels += SignificantPixels(flat, tubeOnly);
        reliefPixels += SignificantPixels(tubeOnly, reference);
        const auto lit = reference;
        Require(ws_knit_fray_ring(&ring, static_cast<int>(side), ws_knit_fray_depth(p.band), p.pitch, p.phase,
            p.originX, p.originY, p.scale, reference.pixels.data(), width, height) != 0, "CPU fray reference");
        frayPixels += SignificantPixels(lit, reference);
        const auto frayed = reference;
        Require(ws_knit_ring_drop_shadow(&ring, static_cast<int>(side), p.band, p.originX, p.originY, p.scale,
            reference.pixels.data(), width, height) != 0, "CPU shadow reference");
        shadowPixels += SignificantPixels(frayed, reference);
        ComPtr<ID3D11ShaderResourceView> colourView, heightView;
        Check(shader.shader.Upload(flat, colourView), "Identical shader colour upload");
        Check(shader.shader.Upload(heights, heightView), "Identical shader height upload");
        ComPtr<ID3D11Texture2D> texture;
        Check(testing::Shade(shader, {flat.width, flat.height, colourView.Get(), heightView.Get(), &p}, texture),
            "Identical-input shader draw");
        RasterImage rendered;
        Check(testing::Readback(texture.Get(), rendered), "Identical-input shader readback");
        Bounds(rendered);
        int peak = 0;
        for (std::size_t i = 0; i < reference.pixels.size(); ++i)
            peak = std::max(peak, PixelError(reference.pixels[i], rendered.pixels[i]));
        shaderMaximum = std::max(shaderMaximum, peak);
        ++shaderCases;
        if (peak > 2) {
            const auto label = name + "-shader-side" + std::to_string(side);
            SavePng(reference, (output / (label + "-cpu.png")).c_str());
            SavePng(rendered, (output / (label + "-gpu.png")).c_str());
            int ignored = 0;
            SavePng(Difference(reference, rendered, ignored), (output / (label + "-diff-32x.png")).c_str());
            std::cerr << label << " strict shader error " << peak << '\n';
        }
        Require(peak <= 2, "Identical-input CPU/GPU shader difference exceeds two channel levels");
    }
}
TileSpec Theme(const designs::ThemeView& theme) {
    TileSpec tile;
    tile.baseArgb = theme.base_argb;
    if (theme.chart_name)
        tile.chart = MakeDesignChart(*designs::ws_design_chart_by_name(theme.chart_name), theme.id);
    return tile;
}
void Pair(testing::Gpu& shader, const KnitFrameSpec& input, const TileSpec& tile,
    TileCache& cache, KnitRasterScratch& scratch, const std::filesystem::path& output,
    const std::string& name, bool save) {
    auto frame = input;
    KnitRenderControl control{&scratch, {}};
    frame.renderer = Renderer::Gpu;
    auto gpu = KnitRaster::RenderPerimeter(frame, tile, cache, control);
    ShaderOnly(shader, gpu, output, name);
    if (shaderOnly) { ++cases; return; }
    frame.renderer = Renderer::Cpu;
    const auto cpu = KnitRaster::RenderPerimeter(frame, tile, cache, control);
    for (std::size_t i = 0; i < gpu.pieces.size(); ++i) {
        ComPtr<ID3D11Texture2D> texture;
        Check(testing::Shade(shader, gpu, i, texture), "Shader draw");
        Check(testing::Readback(texture.Get(), gpu.pieces[i].image), "Shader readback");
        Bounds(gpu.pieces[i].image);
    }
    gpu.assembly.reset();
    const auto reference = KnitRaster::Compose(cpu);
    const auto rendered = KnitRaster::Compose(gpu);
    int peak = 0;
    std::size_t outliers = 0;
    const auto diff = Difference(reference, rendered, peak, &outliers);
    maximum = std::max(maximum, peak);
    const bool within = WithinParity(peak, outliers, tile.sampling);
    if (save || !within) {
        SavePng(reference, (output / (name + "-cpu.png")).c_str());
        SavePng(rendered, (output / (name + "-gpu.png")).c_str());
        SavePng(diff, (output / (name + "-diff-32x.png")).c_str());
    }
    if (!within) std::cerr << name << " max error " << peak << " outliers " << outliers << '\n';
    Require(within, "GPU/CPU difference exceeds the backend parity limit");
    const auto x = rendered.width / 2, y = rendered.height / 2;
    if (input.widthDip > 2 * input.tuckDip && input.heightDip > 2 * input.tuckDip)
        Require(rendered.pixels[std::size_t(y) * rendered.width + x] == 0, "Transparent client hole changed");
    ++cases;
}
void SharedFinish(testing::Gpu& shader, const std::filesystem::path& output) {
    TileCache cache;
    KnitRasterScratch scratch;
    TileSpec sharedFinish;
    sharedFinish.chart = std::make_shared<Chart>("shared-corner-cuff", 1, 3, 2,
        std::vector<std::uint32_t>{0xffe07030, 0, 0xff35a0c0, 0xff35a0c0, 0xffe07030, 0},
        ChartAppearance{designs::ChartSolidCorners | designs::ChartOuterCuff,
            sharedFinish.baseArgb, sharedFinish.baseArgb, true});
    KnitFrameSpec finishFrame;
    finishFrame.renderer = Renderer::Gpu;
    finishFrame.deviceScale = 1.5f;
    finishFrame.bandDip = 24;
    auto finish = KnitRaster::RenderPerimeter(finishFrame, sharedFinish, cache);
    Require(finish.assembly->patch == finish.assembly->cuff,
        "Equal corner and cuff colours must share the immutable solid tile");
    Pair(shader, finishFrame, sharedFinish, cache, scratch, output, "shared-corner-cuff", true);
}
void Catalogue(testing::Gpu& shader, const std::filesystem::path& output) {
    TileCache cache;
    KnitRasterScratch scratch;
    const float bands[] = {2, 10, 24, 64};
    const float scales[] = {1, 1.25f, 2, 3};
    for (std::size_t t = 0; t < designs::ws_design_theme_count(); ++t) {
        const auto& theme = *designs::ws_design_theme_at(t);
        auto tile = Theme(theme);
        for (int variant = 0; variant < 4; ++variant)
            for (const auto material : {Material::Plain, Material::Raised})
                for (const float radius : {0.f, 10.f}) {
                    KnitFrameSpec frame;
                    frame.widthDip = variant == 3 ? 337.f : 620.f;
                    frame.heightDip = variant == 3 ? 213.f : 140.f;
                    frame.bandDip = bands[variant];
                    frame.deviceScale = scales[variant];
                    frame.radiusDip = radius;
                    frame.dim = variant == 1 ? .45f : 0.f;
                    frame.anchor = variant % 2 ? KnitAnchor::Centre : KnitAnchor::Corner;
                    tile.material = material;
                    tile.sampling = variant == 2 ? Sampling::Nearest : Sampling::Linear;
                    const auto name = std::string(theme.id) + "-" + std::to_string(variant) +
                        (material == Material::Raised ? "-raised" : "-plain") + (radius ? "-round" : "-square");
                    Pair(shader, frame, tile, cache, scratch, output, name,
                        variant == 2 && material == Material::Raised && radius > 0);
                }
    }
    TileSpec literal;
    literal.chart = std::make_shared<Chart>("imported-literal", 7, 3, 2,
        std::vector<std::uint32_t>{0xff123456, 0xfffedcba, 0, 0xffed4512, 0xff6f91ac, 0xff55ccee});
    for (const auto stitch : {Stitch::Stockinette, Stitch::Rib, Stitch::Garter})
        for (const float scale : {.125f, .75f, 1.5f, 4.f, 8.f}) {
            KnitFrameSpec frame;
            frame.deviceScale = scale;
            frame.widthDip = 73;
            frame.heightDip = 61;
            literal.stitch = stitch;
            Pair(shader, frame, literal, cache, scratch, output,
                "literal-" + std::to_string(int(stitch)) + "-" + std::to_string(scale), true);
        }
    KnitFrameSpec physical;
    physical.deviceScale = 1.25f;
    physical.contentWidthPixels = 773;
    physical.contentHeightPixels = 177;
    physical.physicalPaddingPixels = 12;
    Pair(shader, physical, literal, cache, scratch, output, "physical-padding", true);
    for (const float size : {1.f, 2.f, 9.f, 19.f}) {
        KnitFrameSpec frame;
        frame.widthDip = frame.heightDip = size;
        frame.radiusDip = 20;
        Pair(shader, frame, literal, cache, scratch, output, "tiny-" + std::to_string(size), true);
    }
    for (std::size_t i = 0; i < designs::ws_design_chart_count(); ++i) {
        const auto& chart = *designs::ws_design_chart_at(i);
        TileSpec tile;
        tile.chart = MakeDesignChart(chart, chart.name);
        KnitFrameSpec frame;
        frame.deviceScale = 1.5f;
        frame.bandDip = 24;
        Pair(shader, frame, tile, cache, scratch, output, "chart-" + std::string(chart.name), false);
    }
    SharedFinish(shader, output);
}
void RetentionAndRecovery(const std::filesystem::path& output, D3D_DRIVER_TYPE driver) {
    GraphicsDevice graphics;
    Check(graphics.Initialize(driver), "Hidden composition device");
    const auto host = CreateWindowExW(0, L"STATIC", L"GPU hidden contract", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(host != nullptr, "Hidden host creation");
    {
        TileCache cache;
        KnitFrameSpec frame;
        frame.renderer = Renderer::Gpu;
        frame.tuckDip = 6;
        auto p = std::make_shared<KnitPerimeter>(KnitRaster::RenderPerimeter(frame, {}, cache));
        Overlay overlay;
        Check(overlay.Create(GetModuleHandleW(nullptr), host, graphics), "Hidden overlay");
        RECT outer{0, 0, LONG(p->width), LONG(p->height)};
        const RECT clip = outer;
        const auto inset = LONG(p->contentInsetPixels);
        RECT exclusion{inset, inset, inset + 620, inset + 140};
        std::array<RasterImage, 4> unmasked, uploaded;
        Check(overlay.Upload(p, outer, clip, {0, 0, 1, 1}, 0, 0, &unmasked), "Unmasked GPU yarn readback");
        const auto unmaskedDraws = graphics.Shader()->DrawCount();
        const auto tileUploads = graphics.Assembler()->TileUploads();
        const auto coordinateUploads = graphics.Shader()->CoordinateUploads();
        const auto tileHits = graphics.Assembler()->TileCacheHits();
        const auto coordinateHits = graphics.Shader()->CoordinateCacheHits();
        Check(overlay.Upload(p, outer, clip, exclusion, 10, 1, &uploaded), "GPU composition upload and readback");
        Require(graphics.Assembler()->TileUploads() == tileUploads &&
            graphics.Shader()->CoordinateUploads() == coordinateUploads &&
            graphics.Assembler()->TileCacheHits() > tileHits &&
            graphics.Shader()->CoordinateCacheHits() > coordinateHits,
            "Repeated uploads reuse native tile bitmaps and coordinate textures");
        const auto draws = graphics.Shader()->DrawCount();
        Require(draws == unmaskedDraws + 4, "GPU backend must draw four strips per upload");
        auto composed = *p;
        composed.assembly.reset();
        for (std::size_t i = 0; i < uploaded.size(); ++i) {
            Bounds(uploaded[i]);
            composed.pieces[i].image = uploaded[i];
        }
        const auto screenshot = KnitRaster::Compose(composed);
        SavePng(screenshot, (output / L"hidden-upload.png").c_str());
        const auto alpha = [&](const RasterImage& image, int x, int y) {
            const auto& strip = p->pieces[0];
            Require(x >= int(strip.x) && y >= int(strip.y) &&
                x < int(strip.x + strip.width) && y < int(strip.y + strip.height),
                "Upload mask samples must lie inside the top strip");
            return image.pixels[std::size_t(y - strip.y) * strip.width + x - strip.x] >> 24;
        };
        Require(alpha(unmasked[0], inset + 310, inset + 2) == 255 &&
            alpha(unmasked[0], inset + 4, inset + 4) == 255,
            "Upload mask exclusion samples must be opaque unmasked yarn");
        Require(alpha(uploaded[0], inset + 310, inset + 2) == 0,
            "Upload mask must exclude straight-edge client yarn inside strip");
        Require(alpha(uploaded[0], inset + 4, inset + 4) == 0,
            "Upload mask must exclude rounded-corner client yarn inside strip");
        Require(alpha(uploaded[0], inset + 310, inset) > 0,
            "Upload mask must retain straight-edge yarn outside tucked hole");
        Require(alpha(uploaded[0], inset + 2, inset + 2) > 0,
            "Upload mask must retain rounded-corner yarn outside tucked hole");
        {
            auto cpuFrame = frame;
            cpuFrame.renderer = Renderer::Cpu;
            auto cpu = std::make_shared<KnitPerimeter>(KnitRaster::RenderPerimeter(cpuFrame, {}, cache));
            Overlay reference;
            Check(reference.Create(GetModuleHandleW(nullptr), host, graphics), "CPU hidden reference");
            std::array<RasterImage, 4> cpuPixels;
            Check(reference.Upload(cpu, outer, clip, exclusion, 10, 1, &cpuPixels), "CPU retained reference readback");
            auto cpuComposed = *cpu;
            for (std::size_t i = 0; i < cpuPixels.size(); ++i) cpuComposed.pieces[i].image = cpuPixels[i];
            const auto cpuScreenshot = KnitRaster::Compose(cpuComposed);
            int peak = 0;
            const auto diff = Difference(cpuScreenshot, screenshot, peak);
            Require(WithinParity(peak, 0, Sampling::Linear), "Native retained CPU/GPU uploads differ");
            SavePng(cpuScreenshot, (output / L"hidden-upload-cpu.png").c_str());
            SavePng(diff, (output / L"hidden-upload-diff-32x.png").c_str());
        }
        for (int i = 0; i < 32; ++i) {
            outer.left += 2; outer.right += 2;
            Check(overlay.UpdatePlacement(outer, clip), "Retained placement");
        }
        Sleep(100);
        Require(graphics.Shader()->DrawCount() == draws, "Idle and movement must not shade");
        auto dpiFrame = frame;
        dpiFrame.deviceScale = 1.25f;
        const auto generations = cache.GenerationCount();
        auto dpi = std::make_shared<KnitPerimeter>(KnitRaster::RenderPerimeter(dpiFrame, {}, cache));
        Require(dpi->tile == p->tile && cache.GenerationCount() == generations,
            "Monitor DPI change reuses the CPU raster tile");
        const RECT dpiBounds{0, 0, LONG(dpi->width), LONG(dpi->height)};
        const auto dpiInset = LONG(dpi->contentInsetPixels);
        const RECT dpiExclusion{dpiInset, dpiInset, dpiInset + 775, dpiInset + 175};
        const auto beforeDpiUploads = graphics.Assembler()->TileUploads();
        Check(overlay.Upload(dpi, dpiBounds, dpiBounds, dpiExclusion, 12.5f, 1.25f), "DPI GPU presentation upload");
        Require(graphics.Assembler()->TileUploads() == beforeDpiUploads,
            "Monitor DPI change reuses native tile bitmaps");
        ResourceLedger limited(1, 1);
        ResourceLedger::Lease rejected;
        Require(limited.Reserve(p, rejected) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA),
            "GPU memory must respect quota");
        const auto stats = graphics.Resources().Status();
        Require(stats.retainedCpuBytes > 0 && std::all_of(p->pieces.begin(), p->pieces.end(),
            [](const auto& piece) { return piece.image.pixels.empty(); }), "GPU perimeter retains no CPU strip pixels");
        overlay.Destroy();
        auto ledger = graphics.ResourceOwner();
        const auto cached = ledger->Status();
        Require(cached.retainedCpuBytes == 0 && cached.cachedGpuBytes > 0 && cached.cacheEntries > 0 &&
            cached.estimatedGpuBytes == cached.cachedGpuBytes && cached.uploadTransientBytes == 0,
            "Released overlay leaves only bounded native GPU caches accounted");
        graphics.Destroy();
        Require(ledger->Status().estimatedGpuBytes == 0 && ledger->Status().cachedGpuBytes == 0 &&
            ledger->Status().cacheEntries == 0 && ledger->Status().cacheAdmissions == ledger->Status().cacheReleases,
            "Device destruction releases every native cache reservation");
        Check(graphics.Initialize(driver), "Replacement graphics device");
        Check(overlay.Create(GetModuleHandleW(nullptr), host, graphics), "Recovered hidden overlay");
        Check(overlay.Upload(p, outer, clip, exclusion, 10, 1), "Recovered GPU upload");
        Check(testing::Complete(graphics.Shader()->Device()), "Recovered GPU completion");
        Require(graphics.Shader()->DrawCount() == 4, "Replacement device must shade anew");
        overlay.Destroy();
        auto invalid = *p;
        invalid.pieces[0].image = {1, 1, 4, {0}};
        ComPtr<ID3D11ShaderResourceView> colour, heights;
        Require(graphics.Assembler()->Assemble(invalid, 0, colour, heights) == E_INVALIDARG,
            "Invalid GPU assembly input must fail, not fall back");
    }
    DestroyWindow(host);
}
void CacheQuotaContracts(testing::Gpu& gpu) {
    TileCache cache;
    KnitFrameSpec frame;
    frame.widthDip = 73; frame.heightDip = 61; frame.renderer = Renderer::Gpu;
    const auto perimeter = KnitRaster::RenderPerimeter(frame, {}, cache);
    auto ledger = std::make_shared<ResourceLedger>(1024u * 1024u, 1);
    testing::Gpu limited(gpu.Device(), ledger);
    ComPtr<ID3D11ShaderResourceView> colour, heights;
    Require(limited.assembler.Assemble(perimeter, 0, colour, heights) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA),
        "Native tile cache rejects allocation before exceeding aggregate quota");
    Require(limited.assembler.TileUploads() == 0 && limited.assembler.CachedBytes() == 0,
        "Denied tile cache admission creates no native bitmap");
    const auto flat = SoftwareStrip(perimeter, 0, false);
    const auto height = SoftwareStrip(perimeter, 0, true);
    Check(limited.shader.Upload(flat, colour), "Quota test colour input");
    Check(limited.shader.Upload(height, heights), "Quota test height input");
    ComPtr<ID3D11Texture2D> texture;
    const KnitSources sources{flat.width, flat.height, colour.Get(), heights.Get(), &perimeter.assembly->parameters[0]};
    Require(testing::Shade(limited, sources, texture) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA),
        "Coordinate cache rejects allocation before exceeding aggregate quota");
    Require(limited.shader.CoordinateUploads() == 0 && limited.shader.DrawCount() == 0 &&
        ledger->Status().estimatedGpuBytes == 0 && ledger->Status().cacheEntries == 0 &&
        ledger->Status().quotaRejected == 2, "Rejected caches and draw leave resource accounting balanced");
}
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void Benchmarks(const std::filesystem::path& output) {
    GraphicsDevice graphics;
    Check(graphics.Initialize(D3D_DRIVER_TYPE_HARDWARE), "Benchmark graphics device (no fallback)");
    const auto host = CreateWindowExW(0, L"STATIC", L"GPU benchmark", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(host != nullptr, "Benchmark hidden host");
    std::ofstream report(output / L"benchmark.csv");
    report << "theme,renderer,cache,iteration,generation_ms,upload_commit_gpu_completion_ms,total_ms,retained_cpu_bytes,estimated_gpu_bytes\n";
    for (const auto id : {"spotify", "messages", "notes"})
        for (const auto renderer : {Renderer::Cpu, Renderer::Gpu}) {
            TileCache cache;
            KnitRasterScratch scratch;
            const auto theme = designs::ws_design_theme_by_id(id);
            Require(theme != nullptr, "Benchmark theme exists");
            KnitFrameSpec frame;
            frame.deviceScale = 2;
            frame.renderer = renderer;
            Overlay overlay;
            Check(overlay.Create(GetModuleHandleW(nullptr), host, graphics), "Benchmark hidden overlay");
            for (int i = 0; i < 25; ++i) {
                const bool cold = i < 5;
                if (cold) { cache.Clear(); scratch.Clear(); }
                const auto start = Clock::now();
                auto p = std::make_shared<KnitPerimeter>(KnitRaster::RenderPerimeter(frame, Theme(*theme), cache,
                    {&scratch, {}}));
                const auto generation = Milliseconds(start);
                RECT bounds{0, 0, LONG(p->width), LONG(p->height)};
                const auto inset = LONG(p->contentInsetPixels);
                RECT exclusion{inset, inset, inset + 1240, inset + 280};
                const auto uploadStart = Clock::now();
                Check(overlay.Upload(p, bounds, bounds, exclusion, 20, 2), "Benchmark retained upload");
                Check(testing::Complete(graphics.Shader()->Device()), "Benchmark GPU completion");
                const auto upload = Milliseconds(uploadStart);
                const auto total = Milliseconds(start);
                const auto resources = graphics.Resources().Status();
                report << id << ',' << (renderer == Renderer::Cpu ? "cpu" : "gpu") << ','
                    << (cold ? "cold" : "warm") << ',' << i << ',' << generation << ',' << upload << ',' << total << ','
                    << resources.retainedCpuBytes << ',' << resources.estimatedGpuBytes << '\n';
            }
            overlay.Destroy();
        }
    DestroyWindow(host);
}
void GenerationBenchmarks(testing::Gpu& shader, const std::filesystem::path& output) {
    std::ofstream report(output / L"generation-benchmark.csv");
    report << "theme,renderer,cache,iteration,cpu_preparation_ms,shader_upload_gpu_completion_ms,total_ms\n";
    for (const auto id : {"spotify", "messages", "notes"})
        for (const auto renderer : {Renderer::Cpu, Renderer::Gpu}) {
            TileCache cache;
            KnitRasterScratch scratch;
            const auto theme = designs::ws_design_theme_by_id(id);
            Require(theme != nullptr, "Generation benchmark theme exists");
            KnitFrameSpec frame;
            frame.deviceScale = 2;
            frame.renderer = renderer;
            for (int i = 0; i < 25; ++i) {
                const bool cold = i < 5;
                if (cold) { cache.Clear(); scratch.Clear(); }
                const auto start = Clock::now();
                const auto p = KnitRaster::RenderPerimeter(frame, Theme(*theme), cache, {&scratch, {}});
                const auto preparation = Milliseconds(start);
                const auto shaderStart = Clock::now();
                std::array<ComPtr<ID3D11Texture2D>, 4> outputs;
                if (renderer == Renderer::Gpu) {
                    for (std::size_t side = 0; side < outputs.size(); ++side)
                        Check(testing::Shade(shader, p, side, outputs[side]), "Generation benchmark shader");
                    Check(testing::Complete(shader.Device()), "Generation benchmark completed GPU");
                }
                const auto shaderTime = renderer == Renderer::Gpu ? Milliseconds(shaderStart) : 0.;
                const auto total = Milliseconds(start);
                report << id << ',' << (renderer == Renderer::Cpu ? "cpu" : "gpu") << ','
                    << (cold ? "cold" : "warm") << ',' << i << ',' << preparation << ',' << shaderTime << ',' << total << '\n';
            }
        }
}
void MultiOverlayBenchmarks(const std::filesystem::path& output) {
    const auto host = CreateWindowExW(0, L"STATIC", L"GPU batch benchmark", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(host != nullptr, "Batch benchmark hidden host");
    std::ofstream report(output / L"multi-overlay-benchmark.csv");
    report << "theme,publication,cache,iteration,overlays,upload_commit_gpu_completion_ms,commits,waits,tile_uploads,coordinate_uploads,estimated_gpu_bytes\n";
    for (const auto id : {"spotify", "messages", "notes"})
        for (const bool batched : {false, true}) {
            GraphicsDevice graphics;
            Check(graphics.Initialize(D3D_DRIVER_TYPE_HARDWARE), "Batch benchmark graphics (no fallback)");
            TileCache cache;
            std::array<Overlay, 8> overlays;
            std::array<Overlay*, 8> group{};
            std::array<std::shared_ptr<KnitPerimeter>, 8> perimeters;
            const auto* theme = designs::ws_design_theme_by_id(id);
            Require(theme != nullptr, "Batch benchmark theme exists");
            for (std::size_t i = 0; i < overlays.size(); ++i) {
                group[i] = &overlays[i];
                Check(overlays[i].Create(GetModuleHandleW(nullptr), host, graphics), "Batch benchmark overlay");
                KnitFrameSpec frame;
                frame.renderer = Renderer::Gpu; frame.deviceScale = 2;
                frame.widthDip += static_cast<float>(i) * 17;
                frame.heightDip += static_cast<float>(i) * 5;
                perimeters[i] = std::make_shared<KnitPerimeter>(KnitRaster::RenderPerimeter(frame, Theme(*theme), cache));
            }
            for (int iteration = 0; iteration < 10; ++iteration) {
                const auto publication = graphics.PublicationStatus();
                const auto tileUploads = graphics.Assembler()->TileUploads();
                const auto coordinateUploads = graphics.Shader()->CoordinateUploads();
                const auto start = Clock::now();
                for (std::size_t i = 0; i < overlays.size(); ++i) {
                    const auto& p = perimeters[i];
                    const RECT bounds{0, 0, LONG(p->width), LONG(p->height)};
                    const auto inset = LONG(p->contentInsetPixels);
                    const RECT exclusion{inset, inset, LONG(p->width) - inset, LONG(p->height) - inset};
                    Check(batched ? overlays[i].PrepareUpload(p, bounds, bounds, exclusion, 20, 2)
                        : overlays[i].Upload(p, bounds, bounds, exclusion, 20, 2), "Batch benchmark surface preparation");
                }
                if (batched) {
                    Check(Overlay::PublishUploads(group), "Batch benchmark single publication");
                    for (std::size_t i = 0; i < overlays.size(); ++i) {
                        const auto& p = perimeters[i];
                        const RECT bounds{0, 0, LONG(p->width), LONG(p->height)};
                        Check(overlays[i].UpdatePlacement(bounds, bounds), "Batch benchmark placement");
                    }
                }
                Check(testing::Complete(graphics.Shader()->Device()), "Batch benchmark completed GPU");
                const auto elapsed = Milliseconds(start);
                const auto completed = graphics.PublicationStatus();
                const auto commits = completed.commits - publication.commits;
                const auto waits = completed.waits - publication.waits;
                Require(commits == (batched ? 1u : 8u) && waits == commits,
                    "Eight-overlay batch uses one completed publication; individual uploads use eight");
                report << id << ',' << (batched ? "batch" : "individual") << ',' << (iteration ? "warm" : "cold")
                    << ',' << iteration << ',' << overlays.size() << ',' << elapsed << ',' << commits << ',' << waits
                    << ',' << graphics.Assembler()->TileUploads() - tileUploads
                    << ',' << graphics.Shader()->CoordinateUploads() - coordinateUploads
                    << ',' << graphics.Resources().Status().estimatedGpuBytes << '\n';
            }
        }
    DestroyWindow(host);
}
struct StripDelta { int peak = 0; std::size_t over = 0, opaqueOver = 0; };
StripDelta Compare(const RasterImage& a, const RasterImage& b) {
    Require(a.width == b.width && a.height == b.height, "Strip dimensions differ");
    StripDelta delta;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        int d = 0;
        for (int shift = 0; shift < 32; shift += 8)
            d = std::max(d, std::abs(int((a.pixels[i] >> shift) & 255) - int((b.pixels[i] >> shift) & 255)));
        delta.peak = std::max(delta.peak, d);
        if (d > 2) {
            ++delta.over;
            if ((a.pixels[i] >> 24) == 255 && (b.pixels[i] >> 24) == 255) ++delta.opaqueOver;
        }
    }
    return delta;
}
void StripProbe(testing::Gpu& gpu, const std::filesystem::path& output) {
    TileCache cache;
    const float bands[] = {2, 10, 24, 64};
    const float scales[] = {1, 1.25f, 2, 3};
    const auto& theme = *designs::ws_design_theme_by_id("vs-code");
    auto tile = Theme(theme);
    for (int variant = 0; variant < 4; ++variant)
        for (const float radius : {0.f, 10.f}) {
            KnitFrameSpec frame;
            frame.widthDip = variant == 3 ? 337.f : 620.f;
            frame.heightDip = variant == 3 ? 213.f : 140.f;
            frame.bandDip = bands[variant];
            frame.deviceScale = scales[variant];
            frame.radiusDip = radius;
            frame.dim = variant == 1 ? .45f : 0.f;
            frame.anchor = variant % 2 ? KnitAnchor::Centre : KnitAnchor::Corner;
            frame.renderer = Renderer::Gpu;
            tile.sampling = variant == 2 ? Sampling::Nearest : Sampling::Linear;
            const auto p = KnitRaster::RenderPerimeter(frame, tile, cache);
            for (int side = 0; side < 4; ++side) {
                RasterImage software[2], hardware[2];
                for (const bool heights : {false, true})
                    software[heights] = SoftwareStrip(p, side, heights);
                ComPtr<ID3D11ShaderResourceView> views[2];
                Check(gpu.assembler.Assemble(p, side, views[0], views[1]), "Probe hardware assembly");
                for (int i = 0; i < 2; ++i) {
                    ComPtr<ID3D11Resource> resource;
                    ComPtr<ID3D11Texture2D> texture;
                    views[i]->GetResource(&resource);
                    Check(resource.As(&texture), "Probe strip texture");
                    Check(testing::Readback(texture.Get(), hardware[i]), "Probe strip readback");
                }
                const auto name = "v" + std::to_string(variant) + (radius ? "-round" : "-square") + "-side" + std::to_string(side);
                for (int i = 0; i < 2; ++i) {
                    const auto d = Compare(software[i], hardware[i]);
                    std::cout << name << (i ? " heights" : " colour ") << " peak=" << d.peak << " over2=" << d.over
                        << " opaqueOver2=" << d.opaqueOver << '\n';
                    if (side == 0 && d.over) {
                        int peak = 0;
                        const auto label = name + (i ? "-heights" : "-colour");
                        SavePng(software[i], (output / (label + "-software.png")).c_str());
                        SavePng(hardware[i], (output / (label + "-hardware.png")).c_str());
                        SavePng(Difference(software[i], hardware[i], peak), (output / (label + "-diff-32x.png")).c_str());
                    }
                }
            }
        }
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        bool warp = false;
        bool stripProbe = false;
        bool contractsOnly = false;
        bool benchmarksOnly = false;
        std::filesystem::path output = L"out\\gpu-comparison";
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--warp") warp = true;
            else if (std::wstring_view(argv[i]) == L"--strip-probe") stripProbe = true;
            else if (std::wstring_view(argv[i]) == L"--shader-only") shaderOnly = true;
            else if (std::wstring_view(argv[i]) == L"--contracts-only") contractsOnly = true;
            else if (std::wstring_view(argv[i]) == L"--benchmarks-only") benchmarksOnly = true;
            else if (std::wstring_view(argv[i]) == L"--output" && i + 1 < argc) output = argv[++i];
            else throw std::invalid_argument("Use --warp, --shader-only, --contracts-only, --benchmarks-only, --strip-probe and/or --output <directory>");
        }
        Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "Test apartment");
        std::filesystem::create_directories(output);
        ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL feature{};
        Check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, &feature, nullptr),
            "Requested comparison adapter (no fallback)");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        Check(device.As(&dxgi), "Comparison DXGI");
        Check(dxgi->GetAdapter(&adapter), "Comparison adapter");
        DXGI_ADAPTER_DESC desc{};
        Check(adapter->GetDesc(&desc), "Adapter description");
        LARGE_INTEGER driver{};
        const auto driverResult = adapter->CheckInterfaceSupport(__uuidof(ID3D11Device), &driver);
        std::array<char, 512> adapterName{};
        Require(WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName.data(),
            static_cast<int>(adapterName.size()), nullptr, nullptr) != 0, "Adapter name UTF-8");
        std::ofstream metadata(output / L"adapter.txt");
        metadata << "adapter=" << adapterName.data() << "\nbackend=" << (warp ? "WARP" : "hardware") << "\nfeature_level=" << unsigned(feature)
            << "\nvendor=" << desc.VendorId << "\ndevice=" << desc.DeviceId
            << "\ndriver_result=" << driverResult << "\ndriver=" << driver.QuadPart << '\n';
        std::wcout << L"Adapter: " << desc.Description << L", backend=" << (warp ? L"WARP" : L"hardware") << '\n';
        const auto shaderLedger = std::make_shared<ResourceLedger>();
        testing::Gpu shader(device.Get(), shaderLedger);
        exactRasterizer = warp;
        if (stripProbe) { StripProbe(shader, output); return 0; }
        if (benchmarksOnly) {
            Require(!warp, "Benchmarks require hardware");
            GenerationBenchmarks(shader, output);
            Benchmarks(output);
            MultiOverlayBenchmarks(output);
            std::cout << "PASS hardware benchmarks checks=" << checks << '\n';
            CoUninitialize();
            return 0;
        }
        if (contractsOnly) {
            SharedFinish(shader, output);
            CacheQuotaContracts(shader);
            testing::GpuCacheContracts(device.Get());
            RetentionAndRecovery(output, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
            std::cout << "PASS hidden/cache contracts checks=" << checks << " shader_max_byte_error=" << shaderMaximum << '\n';
            CoUninitialize();
            return 0;
        }
        Catalogue(shader, output);
        Require(lightingPixels > 0 && reliefPixels > 0 && frayPixels > 0 && shadowPixels > 0,
            "Strict shader matrix must exercise tube lighting, height relief, fray and shadow above tolerance");
        CacheQuotaContracts(shader);
        testing::GpuCacheContracts(device.Get());
        Require(shader.shader.CachedBytes() <= GpuShader::CacheByteLimit &&
            shader.assembler.CachedBytes() <= KnitAssembler::CacheByteLimit &&
            shaderLedger->Status().cacheEntries <= 76 && shaderLedger->Status().cachedGpuBytes ==
                shader.shader.CachedBytes() + shader.assembler.CachedBytes(), "GPU caches stay bounded and fully accounted");
        const auto draws = shader.DrawCount();
        Check(testing::Complete(shader.Device()), "Comparison GPU complete");
        Sleep(100);
        Require(shader.DrawCount() == draws, "Standalone GPU shader must have no idle loop");
        if (!shaderOnly) {
            RetentionAndRecovery(output, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE);
            if (!warp) { GenerationBenchmarks(shader, output); Benchmarks(output); MultiOverlayBenchmarks(output); }
        }
        metadata << "cases=" << cases << "\nchecks=" << checks << "\nchannels=" << channels
            << "\nnonidentical_channels=" << unequal << "\nmaximum_byte_error=" << maximum
            << "\nshader_draws=" << draws << '\n';
        metadata << "shader_only_strips=" << shaderCases << "\nshader_only_maximum_byte_error=" << shaderMaximum
            << "\nlighting_pixels_over2=" << lightingPixels << "\nrelief_pixels_over2=" << reliefPixels
            << "\nfray_pixels_over2=" << frayPixels << "\nshadow_pixels_over2=" << shadowPixels << '\n';
        std::cout << "PASS cases=" << cases << " checks=" << checks << " max_byte_error=" << maximum
            << " nonidentical_channels=" << unequal << "/" << channels << " draws=" << draws << '\n';
        std::cout << "PASS shader_only_strips=" << shaderCases << " max_byte_error=" << shaderMaximum
            << " lighting_pixels_over2=" << lightingPixels << " relief_pixels_over2=" << reliefPixels
            << " fray_pixels_over2=" << frayPixels << " shadow_pixels_over2=" << shadowPixels << '\n';
        shader.shader.ClearCache();
        shader.assembler.ClearCache();
        Require(shaderLedger->Status().cachedGpuBytes == 0 && shaderLedger->Status().cacheEntries == 0 &&
            shaderLedger->Status().estimatedGpuBytes == 0, "Standalone cache clear releases native reservations");
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
