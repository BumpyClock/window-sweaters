// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "GpuReadback.h"
#include "..\..\src\Knitting\WicSurface.h"
#include <stdexcept>

namespace window_sweaters::rendering::testing {
inline void GpuCacheContracts(ID3D11Device* device) {
    using knitting::detail::Check;
    using Microsoft::WRL::ComPtr;
    const auto require = [](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    KnitFrameSpec frame;
    frame.widthDip = 60; frame.heightDip = 30; frame.renderer = Renderer::Gpu;
    knitting::TileCache cpuCache;
    auto perimeter = std::make_shared<KnitPerimeter>(KnitRaster::RenderPerimeter(frame, {}, cpuCache));
    auto admitted = std::make_shared<ResourceLedger>();
    ResourceLedger::Lease upload;
    Check(admitted->Reserve(perimeter, upload), "Admit complete cache working set");
    const auto initial = admitted->Status();
    GpuShader shader(device, admitted);
    KnitAssembler assembler(device, admitted);
    for (std::size_t side = 0; side < perimeter->pieces.size(); ++side) {
        ComPtr<ID3D11ShaderResourceView> colour, heights;
        Check(assembler.Assemble(*perimeter, side, colour, heights, &upload), "Use admitted native tile bytes");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = perimeter->pieces[side].width; desc.Height = perimeter->pieces[side].height;
        desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        Check(device->CreateTexture2D(&desc, nullptr, &texture), "Cache admission target");
        Check(device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Cache admission target view");
        const KnitSources sources{desc.Width, desc.Height, colour.Get(), heights.Get(),
            &perimeter->assembly->parameters[side]};
        Check(shader.Render(sources, {}, target.Get(), {}, &upload), "Use admitted coordinate bytes");
        if (side == 0) {
            const auto pinnedBytes = admitted->Status().cachedGpuBytes;
            const auto pinnedUploads = shader.CoordinateUploads();
            shader.ClearCache();
            require(admitted->Status().cachedGpuBytes == pinnedBytes && shader.CachedBytes() == 0,
                "Coordinate eviction must preserve staged resources and admission charges");
            Check(shader.Render(sources, {}, target.Get(), {}, &upload), "Reuse evicted staged coordinate tables");
            require(shader.CoordinateUploads() == pinnedUploads,
                "Staged coordinate lookup must reuse evicted upload pins");
        }
        const auto current = admitted->Status();
        require(current.estimatedGpuBytes == initial.estimatedGpuBytes &&
            current.uploadTransientBytes + current.cachedGpuBytes == initial.uploadTransientBytes,
            "Cache transfer must preserve admission total without double charging");
    }
    upload.EndUpload();
    std::size_t surfaces = 0;
    for (const auto& piece : perimeter->pieces) surfaces += std::size_t(piece.width) * piece.height * 4u;
    require(admitted->Status().uploadTransientBytes == 0 &&
        admitted->Status().estimatedGpuBytes == surfaces + admitted->Status().cachedGpuBytes,
        "Completed upload retains only surfaces and allocated cache bytes");
    upload.Reset();
    require(admitted->Status().estimatedGpuBytes == admitted->Status().cachedGpuBytes &&
        admitted->Status().retainedCpuBytes == 0, "Overlay release preserves only native caches");
    shader.ClearCache(); assembler.ClearCache();
    require(admitted->Status().estimatedGpuBytes == 0 && admitted->Status().cacheEntries == 0,
        "Cache clear releases all transferred bytes");
    ResourceLedger::Lease staged;
    Check(admitted->Reserve(perimeter, staged), "Admit staged cache lifetime");
    const auto stagedTotal = admitted->Status().estimatedGpuBytes;
    ComPtr<ID3D11ShaderResourceView> stagedColour, stagedHeights;
    Check(assembler.Assemble(*perimeter, 0, stagedColour, stagedHeights, &staged), "Stage native cache resources");
    const auto stagedCacheBytes = admitted->Status().cachedGpuBytes;
    assembler.ClearCache();
    require(stagedCacheBytes > 0 && admitted->Status().cachedGpuBytes == stagedCacheBytes &&
        admitted->Status().estimatedGpuBytes == stagedTotal && assembler.CachedBytes() == 0,
        "Cache clear must retain staged upload resources and their admission charge");
    const auto stagedUploads = assembler.TileUploads();
    Check(assembler.Assemble(*perimeter, 1, stagedColour, stagedHeights, &staged), "Reuse evicted staged native resources");
    require(assembler.TileUploads() == stagedUploads && admitted->Status().estimatedGpuBytes == stagedTotal,
        "Later staged sides must reuse pinned resources after persistent eviction");
    staged.EndUpload();
    require(admitted->Status().cachedGpuBytes == 0 && admitted->Status().estimatedGpuBytes == surfaces,
        "Publication completion releases evicted cache pins");
    staged.Reset();
    ResourceLedger::Lease canceled;
    Check(admitted->Reserve(perimeter, canceled), "Admit canceled cache lifetime");
    Check(assembler.Assemble(*perimeter, 0, stagedColour, stagedHeights, &canceled), "Prepare canceled native cache resources");
    assembler.ClearCache();
    const auto canceledTotal = admitted->Status().estimatedGpuBytes;
    ResourceLedger::Lease moved(std::move(canceled));
    canceled.Reset();
    require(admitted->Status().estimatedGpuBytes == canceledTotal && admitted->Status().cachedGpuBytes > 0,
        "Moved staged lease must preserve cache pins after donor reset");
    moved.Reset();
    require(admitted->Status().estimatedGpuBytes == 0 && admitted->Status().cachedGpuBytes == 0 &&
        admitted->Status().cacheEntries == 0 && admitted->Status().retainedCpuBytes == 0,
        "Canceled staged lease must release evicted cache pins without EndUpload");
    auto tight = std::make_shared<ResourceLedger>(128u * 1024u * 1024u,
        initial.estimatedGpuBytes - 1);
    ResourceLedger::Lease denied;
    require(tight->Reserve(perimeter, denied) == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA) &&
        tight->Status().estimatedGpuBytes == 0 && tight->Status().releaseGeneration == 0,
        "Insufficient complete working set must reject before transient allocation or retry generation");

    auto tiles = std::make_shared<ResourceLedger>();
    KnitAssembler bounded(device, tiles);
    auto mutableTile = std::make_shared<knitting::Tile>(*perimeter->tile);
    mutableTile->image = {2048, 1024, 2048 * 4, std::vector<std::uint32_t>(2048u * 1024u, 0xff608090)};
    mutableTile->height = {2048, 1024, 2048 * 4, std::vector<std::uint32_t>(2048u * 1024u, 0xff606060)};
    mutableTile->layout.width_dip = 2048; mutableTile->layout.height_dip = 1024;
    mutableTile->layout.raster_scale = 1;
    auto large = *perimeter;
    large.tile = mutableTile;
    for (std::uint64_t serial = 1000000; serial < 1000003; ++serial) {
        mutableTile->serial = serial;
        ComPtr<ID3D11ShaderResourceView> colour, heights;
        Check(bounded.Assemble(large, 0, colour, heights), "Byte-budgeted native tile eviction");
        require(bounded.CachedBytes() <= KnitAssembler::CacheByteLimit &&
            tiles->Status().peakEstimatedGpuBytes <= KnitAssembler::CacheByteLimit,
            "Native tile eviction must precede allocation at the byte bound");
    }
    const auto uploads = bounded.TileUploads();
    ComPtr<ID3D11ShaderResourceView> colour, heights;
    Check(bounded.Assemble(large, 0, colour, heights), "Reuse newest native tiles after eviction");
    require(bounded.TileUploads() == uploads && bounded.TileCacheHits() >= 2,
        "Newest native tiles must remain reusable after byte eviction");
    bounded.ClearCache();
    require(tiles->Status().estimatedGpuBytes == 0, "Native tile eviction cache clear");
    mutableTile->serial = 1000004;
    mutableTile->image = {4096, 2049, 4096 * 4, std::vector<std::uint32_t>(4096u * 2049u, 0xff608090)};
    mutableTile->height = {4096, 2049, 4096 * 4, std::vector<std::uint32_t>(4096u * 2049u, 0xff606060)};
    mutableTile->layout.width_dip = 4096; mutableTile->layout.height_dip = 2049;
    large.assembly->patch = mutableTile; large.assembly->cuff = mutableTile;
    const auto beforeOversized = bounded.TileUploads();
    Check(bounded.Assemble(large, 0, colour, heights), "Oversized tile temporary upload with aliased roles");
    require(bounded.TileUploads() == beforeOversized + 2 && bounded.CachedBytes() == 0 &&
        tiles->Status().estimatedGpuBytes == 0 && tiles->Status().cacheEntries == 0,
        "Oversized aliases must share charged temporary uploads and release them after drawing");
    Check(bounded.Assemble(large, 1, colour, heights), "Oversized tile repeated side upload");
    require(bounded.TileUploads() == beforeOversized + 4 && tiles->Status().estimatedGpuBytes == 0,
        "Oversized temporary tile lifetime must not block later strips");
    auto oversized = std::make_shared<KnitPerimeter>(large);
    ResourceLedger::Lease oversizedUpload;
    Check(tiles->Reserve(oversized, oversizedUpload), "Admit oversized temporary working set");
    const auto oversizedTotal = tiles->Status().estimatedGpuBytes;
    const auto beforeAdmittedOversized = bounded.TileUploads();
    for (std::size_t side = 0; side < oversized->pieces.size(); ++side) {
        Check(bounded.Assemble(*oversized, side, colour, heights, &oversizedUpload), "Reuse admitted oversized tile across sides");
        require(bounded.TileUploads() == beforeAdmittedOversized + 2 &&
            tiles->Status().estimatedGpuBytes == oversizedTotal && bounded.CachedBytes() == 0,
            "Oversized staged upload must reuse pinned bitmaps within prepaid working set");
    }
    oversizedUpload.EndUpload();
    require(tiles->Status().cachedGpuBytes == 0, "Oversized temporary pins release at publication completion");
    oversizedUpload.Reset();
    require(tiles->Status().estimatedGpuBytes == 0, "Oversized upload lease releases all resources");

    auto coordinates = std::make_shared<ResourceLedger>();
    Gpu gpu(device, coordinates);
    knitting::RasterImage pixel{1, 1, 4, {0xff808080}};
    Check(gpu.shader.Upload(pixel, colour), "Coordinate contract colour");
    Check(gpu.shader.Upload(pixel, heights), "Coordinate contract heights");
    auto parameters = perimeter->assembly->parameters[0];
    parameters.originY = .25f;
    KnitSources sources{64, 32, colour.Get(), heights.Get(), &parameters};
    ComPtr<ID3D11Texture2D> output;
    for (int origin = 0; origin < 72; ++origin) {
        parameters.originX = float(origin);
        Check(Shade(gpu, sources, output), "Coordinate count-bound eviction");
        require(gpu.shader.CachedBytes() <= GpuShader::CacheByteLimit &&
            coordinates->Status().cacheEntries <= 64, "Coordinate cache count and byte bounds");
    }
    const auto coordinateUploads = gpu.shader.CoordinateUploads();
    Check(Shade(gpu, sources, output), "Reuse newest coordinate tables");
    require(gpu.shader.CoordinateUploads() == coordinateUploads,
        "Newest coordinate tables must remain reusable after eviction");
    parameters.originX = 0.f;
    Check(Shade(gpu, sources, output), "Positive zero coordinate key");
    const auto beforeNegativeZero = gpu.shader.CoordinateUploads();
    parameters.originX = -0.f;
    Check(Shade(gpu, sources, output), "Negative zero coordinate key");
    require(gpu.shader.CoordinateUploads() == beforeNegativeZero + 1,
        "Coordinate keys must preserve exact float bits");
    gpu.shader.ClearCache(); gpu.assembler.ClearCache();
    require(coordinates->Status().estimatedGpuBytes == 0, "Coordinate cache clear releases charge");
    Check(Complete(device), "Cache contract GPU completion");
}
}
