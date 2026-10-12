// SPDX-License-Identifier: GPL-3.0-only
#include "GpuShader.h"
#include "KnitVertex.h"
#include "KnitPixel.h"
#include "CopyPixel.h"
#include <cmath>

namespace window_sweaters::rendering {
using Microsoft::WRL::ComPtr;
namespace {
struct Placement {
    float hole[4];
    float offsetX, offsetY, radius;
    std::uint32_t hasHole;
};
static_assert(sizeof(Placement) == 32);

HRESULT ConstantBuffer(ID3D11Device* device, UINT bytes, ComPtr<ID3D11Buffer>& buffer) noexcept {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return device->CreateBuffer(&desc, nullptr, buffer.ReleaseAndGetAddressOf());
}

bool Valid(const KnitShaderParameters& p) noexcept {
    for (float value : {p.ringWidth, p.ringHeight, p.outerRadius, p.inset, p.innerRadius, p.originX, p.originY,
            p.scale, p.relief, p.band, p.pitch, p.phase, p.inverseScale})
        if (!std::isfinite(value)) return false;
    return p.ringWidth > 0 && p.ringHeight > 0 && p.outerRadius >= 0 && p.inset >= 0 && p.innerRadius >= 0 &&
        p.scale > 0 && p.inverseScale == 1.f / p.scale && p.relief >= 0 && p.band > 0 && p.pitch > 0 && p.side <= 3;
}

bool Valid(const KnitMask& m) noexcept {
    for (float value : {m.left, m.top, m.right, m.bottom, m.radius}) if (!std::isfinite(value)) return false;
    return !m.hole || (m.right > m.left && m.bottom > m.top && m.radius >= 0);
}
}

HRESULT GpuShader::Initialize() noexcept {
    if (!device_) return E_UNEXPECTED;
    if (copy_) return S_OK;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    HRESULT result = device_->CreateVertexShader(KnitVertex, sizeof(KnitVertex), nullptr, vertex_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) result = ConstantBuffer(device_.Get(), sizeof(KnitShaderParameters), parameters_);
    if (SUCCEEDED(result)) result = ConstantBuffer(device_.Get(), sizeof(Placement), placement_);
    if (SUCCEEDED(result)) result = device_->CreatePixelShader(KnitPixel, sizeof(KnitPixel), nullptr,
        knit_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) result = device_->CreatePixelShader(CopyPixel, sizeof(CopyPixel), nullptr,
        copy_.ReleaseAndGetAddressOf());
    return result;
}

HRESULT GpuShader::Upload(const knitting::RasterImage& image, ComPtr<ID3D11ShaderResourceView>& view) noexcept {
    view.Reset();
    if (!device_) return E_UNEXPECTED;
    if (!image.width || !image.height || image.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        image.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || image.stride != image.width * 4 ||
        image.pixels.size() != std::size_t(image.width) * image.height) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width; desc.Height = image.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{image.pixels.data(), image.stride, 0};
    ComPtr<ID3D11Texture2D> texture;
    HRESULT result = device_->CreateTexture2D(&desc, &data, &texture);
    if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
    return result;
}

HRESULT GpuShader::Render(const KnitSources& sources, const KnitMask& mask, ID3D11RenderTargetView* target,
    POINT offset) noexcept {
    const bool knit = sources.heights != nullptr;
    if (!target || !sources.colour || !sources.width || !sources.height ||
        sources.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || sources.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        offset.x < 0 || offset.y < 0 || knit != (sources.parameters != nullptr) ||
        (knit && !Valid(*sources.parameters)) || !Valid(mask)) return E_INVALIDARG;
    HRESULT result = Initialize();
    if (FAILED(result)) return result;
    ComPtr<ID3D11Texture1D> coordinates[2];
    ComPtr<ID3D11ShaderResourceView> coordinateViews[2];
    for (int axis = 0; knit && axis < 2; ++axis) {
        const auto& p = *sources.parameters;
        const auto length = axis ? sources.height : sources.width;
        std::vector<std::array<float, 2>> values;
        try { values.resize(length); } catch (...) { return E_OUTOFMEMORY; }
        // CPU float division fixes pixel-grid ties and fray hashes across GPU reciprocal implementations.
        for (std::uint32_t i = 0; i < length; ++i) {
            const auto coordinate = (axis ? p.originY : p.originX) + (float(i) + .5f) / p.scale;
            values[i] = {coordinate, std::floor(coordinate * p.scale)};
        }
        D3D11_TEXTURE1D_DESC desc{};
        desc.Width = length;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
        result = device_->CreateTexture1D(&desc, &data, &coordinates[axis]);
        if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(coordinates[axis].Get(), nullptr, &coordinateViews[axis]);
        if (FAILED(result)) return result;
    }
    const Placement placement{{mask.left, mask.top, mask.right, mask.bottom},
        float(offset.x), float(offset.y), mask.radius, mask.hole ? 1u : 0u};
    context_->ClearState();
    if (knit) context_->UpdateSubresource(parameters_.Get(), 0, nullptr, sources.parameters, 0, 0);
    context_->UpdateSubresource(placement_.Get(), 0, nullptr, &placement, 0, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(knit ? knit_.Get() : copy_.Get(), nullptr, 0);
    ID3D11Buffer* buffers[] = {parameters_.Get(), placement_.Get()};
    context_->PSSetConstantBuffers(0, 2, buffers);
    ID3D11ShaderResourceView* views[] = {sources.colour, sources.heights, coordinateViews[0].Get(), coordinateViews[1].Get()};
    context_->PSSetShaderResources(0, knit ? 4 : 1, views);
    context_->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport{float(offset.x), float(offset.y), float(sources.width), float(sources.height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->Draw(3, 0);
    if (knit) ++draws_;
    context_->ClearState();
    context_->Flush();
    return device_->GetDeviceRemovedReason();
}
}
