// SPDX-License-Identifier: GPL-3.0-only
#include "KnitAssembler.h"
#include "..\Knitting\WicSurface.h"
#include <algorithm>
#include <new>
#include <system_error>

namespace window_sweaters::rendering {
using Microsoft::WRL::ComPtr;

HRESULT KnitAssembler::Initialize() noexcept {
    if (!device_) return E_UNEXPECTED;
    if (context_) return S_OK;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<IDXGIDevice> dxgi;
    HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    if (SUCCEEDED(result)) result = device_.As(&dxgi);
    if (SUCCEEDED(result)) result = factory->CreateDevice(dxgi.Get(), drawing_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) result = drawing_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        context_.ReleaseAndGetAddressOf());
    return result;
}

HRESULT KnitAssembler::Target(std::uint32_t width, std::uint32_t height, float scale,
    ComPtr<ID2D1Bitmap1>& target, ComPtr<ID3D11ShaderResourceView>& view) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGISurface> surface;
    HRESULT result = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(texture.Get(), nullptr, view.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) result = texture.As(&surface);
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * scale, 96.f * scale);
    if (SUCCEEDED(result)) result = context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
        target.ReleaseAndGetAddressOf());
    return result;
}

void KnitAssembler::EvictTile() noexcept {
    const auto oldest = std::min_element(tiles_.begin(), tiles_.end(),
        [](const auto& a, const auto& b) { return a->tick < b->tick; });
    cachedBytes_ -= (*oldest)->bytes;
    tiles_.erase(oldest);
}

ComPtr<ID2D1BitmapBrush> KnitAssembler::Brush(int role, const knitting::Tile& tile, bool heights, float px, float py,
    float repeatWidth, std::array<std::shared_ptr<CachedTile>, 3>& pinned, ResourceLedger::Lease* upload) {
    auto cached = std::find_if(tiles_.begin(), tiles_.end(),
        [&](const auto& entry) { return entry->serial == tile.serial && entry->heights == heights; });
    std::shared_ptr<CachedTile> entry;
    if (upload) entry = upload->FindCache<CachedTile>(this, ResourceLedger::Lease::CacheKind::Tile,
        [&](const CachedTile& tileEntry) { return tileEntry.serial == tile.serial && tileEntry.heights == heights; });
    const auto active = std::find_if(pinned.begin(), pinned.end(), [&](const auto& tileEntry) {
        return tileEntry && tileEntry->serial == tile.serial && tileEntry->heights == heights;
    });
    if (entry) ++hits_;
    else if (cached != tiles_.end()) { entry = *cached; ++hits_; }
    else if (active != pinned.end()) { entry = *active; ++hits_; }
    else {
        const auto& image = heights ? tile.height : tile.image;
        const auto bytes = std::size_t(image.width) * image.height * 4u;
        const bool retain = bytes <= CacheByteLimit;
        while (!tiles_.empty() && (bytes > resources_->CacheCapacity(upload) ||
            (retain && (tiles_.size() >= MaxCachedTiles || bytes > CacheByteLimit - cachedBytes_))))
            EvictTile();
        entry = std::make_shared<CachedTile>();
        knitting::detail::Check(resources_->ReserveCache(bytes, entry->lease, upload), "Reserve hardware periodic tile");
        entry->serial = tile.serial; entry->heights = heights; entry->bytes = bytes;
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f * tile.layout.raster_scale, 96.f * tile.layout.raster_scale);
        knitting::detail::Check(static_cast<ID2D1RenderTarget*>(context_.Get())->CreateBitmap(
            D2D1::SizeU(image.width, image.height), image.pixels.data(), image.stride, properties, &entry->bitmap),
            "Upload hardware periodic tile");
        ++uploads_;
        if (retain) { tiles_.push_back(entry); cachedBytes_ += bytes; }
    }
    entry->tick = ++tick_;
    if (upload) knitting::detail::Check(upload->PinCache(this, ResourceLedger::Lease::CacheKind::Tile, entry),
        "Retain admitted hardware periodic tile");
    pinned.at(static_cast<std::size_t>(role)) = entry;
    auto& brush = entry->brushes.at(static_cast<std::size_t>(role));
    if (!brush) {
        const auto sampling = tile.spec.sampling == knitting::Sampling::Nearest
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        knitting::detail::Check(context_->CreateBitmapBrush(entry->bitmap.Get(), D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, sampling), &brush),
            "Create hardware periodic yarn brush");
    }
    brush->SetTransform(D2D1::Matrix3x2F::Scale(repeatWidth / tile.layout.width_dip, 1.f)
        * D2D1::Matrix3x2F::Translation(px, py));
    return brush;
}

HRESULT KnitAssembler::Assemble(const KnitPerimeter& perimeter, std::size_t side,
    ComPtr<ID3D11ShaderResourceView>& colour, ComPtr<ID3D11ShaderResourceView>& heights,
    ResourceLedger::Lease* upload) noexcept {
    colour.Reset();
    heights.Reset();
    if (!perimeter.assembly || !perimeter.tile || side >= perimeter.pieces.size() ||
        !perimeter.PieceValid(perimeter.pieces[side]) ||
        perimeter.pieces[side].width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        perimeter.pieces[side].height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return E_INVALIDARG;
    HRESULT result = Initialize();
    if (FAILED(result)) return result;
    const auto& assembly = *perimeter.assembly;
    const auto& piece = perimeter.pieces[side];
    const float scale = assembly.frame.deviceScale;
    try {
        for (const bool stitchHeights : {false, true}) {
            std::array<std::shared_ptr<CachedTile>, 3> pinned;
            const KnitTileBrush brush = [this, &pinned, upload](int role, const knitting::Tile& tile, bool heights, float px, float py,
                float repeatWidth) { return Brush(role, tile, heights, px, py, repeatWidth, pinned, upload); };
            ComPtr<ID2D1Bitmap1> target;
            result = Target(piece.width, piece.height, scale, target, stitchHeights ? heights : colour);
            if (FAILED(result)) break;
            context_->SetTarget(target.Get());
            context_->SetDpi(96.f * scale, 96.f * scale);
            DrawKnitStrip(*context_.Get(), assembly, *perimeter.tile, piece, static_cast<int>(side),
                stitchHeights, brush);
            context_->SetTarget(nullptr);
        }
    } catch (const std::system_error& error) {
        result = static_cast<HRESULT>(error.code().value());
    } catch (const std::bad_alloc&) {
        result = E_OUTOFMEMORY;
    } catch (...) {
        result = E_FAIL;
    }
    context_->SetTarget(nullptr);
    if (FAILED(result)) { colour.Reset(); heights.Reset(); }
    return result;
}
}
