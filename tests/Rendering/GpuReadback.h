// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "..\..\src\Rendering\GpuShader.h"
#include "..\..\src\Rendering\KnitAssembler.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>

namespace window_sweaters::rendering::testing {
inline HRESULT Complete(ID3D11Device* device) noexcept {
    using Microsoft::WRL::ComPtr;
    if (!device) return E_UNEXPECTED;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<ID3D11Query> query;
    const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    HRESULT result = device->CreateQuery(&desc, &query);
    if (FAILED(result)) return result;
    context->End(query.Get());
    context->Flush();
    const auto deadline = GetTickCount64() + 30000;
    while ((result = context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
        const auto removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) return removed;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        SwitchToThread();
    }
    return result;
}

struct Gpu {
    explicit Gpu(ID3D11Device* device, std::shared_ptr<ResourceLedger> resources = {})
        : shader(device, resources), assembler(device, std::move(resources)) {}
    ID3D11Device* Device() const noexcept { return shader.Device(); }
    std::uint64_t DrawCount() const noexcept { return shader.DrawCount(); }
    GpuShader shader;
    KnitAssembler assembler;
};

inline HRESULT Shade(Gpu& gpu, const KnitSources& sources,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture) noexcept {
    using Microsoft::WRL::ComPtr;
    texture.Reset();
    if (!gpu.Device()) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = sources.width; desc.Height = sources.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT result = gpu.Device()->CreateTexture2D(&desc, nullptr, &texture);
    ComPtr<ID3D11RenderTargetView> target;
    if (SUCCEEDED(result)) result = gpu.Device()->CreateRenderTargetView(texture.Get(), nullptr, &target);
    if (SUCCEEDED(result)) result = gpu.shader.Render(sources, {}, target.Get(), {});
    return result;
}

inline HRESULT Shade(Gpu& gpu, const KnitPerimeter& perimeter, std::size_t side,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture) noexcept {
    using Microsoft::WRL::ComPtr;
    if (!gpu.Device() || side >= perimeter.pieces.size()) return E_INVALIDARG;
    const auto& piece = perimeter.pieces[side];
    ComPtr<ID3D11ShaderResourceView> colour, heights;
    HRESULT result = perimeter.assembly ? gpu.assembler.Assemble(perimeter, side, colour, heights)
        : gpu.shader.Upload(piece.image, colour);
    if (FAILED(result)) return result;
    const KnitSources sources{piece.width, piece.height, colour.Get(), heights.Get(),
        perimeter.assembly ? &perimeter.assembly->parameters[side] : nullptr};
    return Shade(gpu, sources, texture);
}

inline HRESULT Readback(ID3D11Texture2D* texture, knitting::RasterImage& image) noexcept {
    using Microsoft::WRL::ComPtr;
    if (!texture) return E_INVALIDARG;
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(result)) return result;
    try {
        image = {desc.Width, desc.Height, desc.Width * 4, {}};
        image.pixels.resize(std::size_t(desc.Width) * desc.Height);
    } catch (...) { return E_OUTOFMEMORY; }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) return result;
    for (std::uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(image.pixels.data() + std::size_t(y) * desc.Width,
            static_cast<const BYTE*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, image.stride);
    context->Unmap(staging.Get(), 0);
    return device->GetDeviceRemovedReason();
}
}
