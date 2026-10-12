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

ComPtr<ID2D1BitmapBrush> KnitAssembler::Brush(const knitting::Tile& tile, bool heights, float px, float py,
    float repeatWidth) {
    auto cached = std::find_if(tiles_.begin(), tiles_.end(),
        [&](const CachedTile& entry) { return entry.serial == tile.serial && entry.heights == heights; });
    if (cached == tiles_.end()) {
        const auto& image = heights ? tile.height : tile.image;
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f * tile.layout.raster_scale, 96.f * tile.layout.raster_scale);
        CachedTile entry{tile.serial, heights};
        knitting::detail::Check(static_cast<ID2D1RenderTarget*>(context_.Get())->CreateBitmap(
            D2D1::SizeU(image.width, image.height), image.pixels.data(), image.stride, properties, &entry.bitmap),
            "Upload hardware periodic tile");
        if (tiles_.size() >= MaxCachedTiles)
            tiles_.erase(std::min_element(tiles_.begin(), tiles_.end(),
                [](const CachedTile& a, const CachedTile& b) { return a.tick < b.tick; }));
        tiles_.push_back(std::move(entry));
        cached = std::prev(tiles_.end());
    }
    cached->tick = ++tick_;
    const auto sampling = tile.spec.sampling == knitting::Sampling::Nearest
        ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
    ComPtr<ID2D1BitmapBrush> brush;
    knitting::detail::Check(context_->CreateBitmapBrush(cached->bitmap.Get(), D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, sampling), &brush), "Create hardware periodic yarn brush");
    brush->SetTransform(D2D1::Matrix3x2F::Scale(repeatWidth / tile.layout.width_dip, 1.f)
        * D2D1::Matrix3x2F::Translation(px, py));
    return brush;
}

HRESULT KnitAssembler::Assemble(const KnitPerimeter& perimeter, std::size_t side,
    ComPtr<ID3D11ShaderResourceView>& colour, ComPtr<ID3D11ShaderResourceView>& heights) noexcept {
    colour.Reset();
    heights.Reset();
    if (!perimeter.assembly || !perimeter.tile || side >= perimeter.pieces.size() ||
        !perimeter.PieceValid(perimeter.pieces[side])) return E_INVALIDARG;
    HRESULT result = Initialize();
    if (FAILED(result)) return result;
    const auto& assembly = *perimeter.assembly;
    const auto& piece = perimeter.pieces[side];
    const float scale = assembly.frame.deviceScale;
    try {
        const KnitTileBrush brush = [this](int, const knitting::Tile& tile, bool stitchHeights, float px, float py,
                                           float repeatWidth) { return Brush(tile, stitchHeights, px, py, repeatWidth); };
        for (const bool stitchHeights : {false, true}) {
            ComPtr<ID2D1Bitmap1> target;
            result = Target(piece.width, piece.height, scale, target, stitchHeights ? heights : colour);
            if (FAILED(result)) break;
            context_->SetTarget(target.Get());
            context_->SetDpi(96.f * scale, 96.f * scale);
            KnitRaster::DrawStrip(*context_.Get(), assembly, *perimeter.tile, piece, static_cast<int>(side),
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
