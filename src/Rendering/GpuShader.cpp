// SPDX-License-Identifier: GPL-3.0-only
#include "GpuShader.h"
#include "KnitVertex.h"
#include "KnitPixel.h"
#include <cmath>
#include <bit>

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
    if (knit_) return S_OK;
    device_->GetImmediateContext(context_.ReleaseAndGetAddressOf());
    HRESULT result = device_->CreateVertexShader(KnitVertex, sizeof(KnitVertex), nullptr, vertex_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) result = ConstantBuffer(device_.Get(), sizeof(KnitShaderParameters), parameters_);
    if (SUCCEEDED(result)) result = ConstantBuffer(device_.Get(), sizeof(Placement), placement_);
    if (SUCCEEDED(result)) result = device_->CreatePixelShader(KnitPixel, sizeof(KnitPixel), nullptr,
        knit_.ReleaseAndGetAddressOf());
    return result;
}

HRESULT GpuShader::CoordinateTable(std::uint32_t length, float origin, float scale,
    std::shared_ptr<Coordinates>& table, ResourceLedger::Lease* upload) noexcept {
    const auto originBits = std::bit_cast<std::uint32_t>(origin);
    const auto scaleBits = std::bit_cast<std::uint32_t>(scale);
    const auto matches = [&](const Coordinates& entry) {
        return entry.length == length && entry.originBits == originBits && entry.scaleBits == scaleBits;
    };
    if (upload) {
        table = upload->FindCache<Coordinates>(this, ResourceLedger::Lease::CacheKind::Coordinates, matches);
        if (table) { table->tick = ++tick_; ++coordinateHits_; return S_OK; }
    }
    const auto found = std::find_if(coordinates_.begin(), coordinates_.end(), [&](const auto& entry) {
        return matches(*entry);
    });
    if (found != coordinates_.end()) {
        table = *found; table->tick = ++tick_; ++coordinateHits_;
        return upload ? upload->PinCache(this, ResourceLedger::Lease::CacheKind::Coordinates, table) : S_OK;
    }
    try {
        const auto bytes = std::size_t(length) * sizeof(std::array<float, 2>);
        while (!coordinates_.empty() && (coordinates_.size() >= MaxCachedCoordinates ||
            bytes > CacheByteLimit - cachedBytes_ || bytes > resources_->CacheCapacity(upload))) {
            const auto oldest = std::min_element(coordinates_.begin(), coordinates_.end(),
                [](const auto& a, const auto& b) { return a->tick < b->tick; });
            cachedBytes_ -= std::size_t((*oldest)->length) * sizeof(std::array<float, 2>);
            coordinates_.erase(oldest);
        }
        auto entry = std::make_shared<Coordinates>();
        HRESULT result = resources_->ReserveCache(bytes, entry->lease, upload);
        if (FAILED(result)) return result;
        std::vector<std::array<float, 2>> values(length);
        // CPU float division fixes pixel-grid ties and fray hashes across GPU reciprocal implementations.
        for (std::uint32_t i = 0; i < length; ++i) {
            const auto coordinate = origin + (float(i) + .5f) / scale;
            values[i] = {coordinate, std::floor(coordinate * scale)};
        }
        D3D11_TEXTURE1D_DESC desc{};
        desc.Width = length;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
        ComPtr<ID3D11Texture1D> texture;
        result = device_->CreateTexture1D(&desc, &data, &texture);
        if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(texture.Get(), nullptr, &entry->view);
        if (FAILED(result)) return result;
        ++coordinateUploads_;
        entry->length = length; entry->originBits = originBits; entry->scaleBits = scaleBits; entry->tick = ++tick_;
        if (upload) {
            result = upload->PinCache(this, ResourceLedger::Lease::CacheKind::Coordinates, entry);
            if (FAILED(result)) return result;
        }
        coordinates_.push_back(entry); cachedBytes_ += bytes;
        table = std::move(entry);
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

HRESULT GpuShader::Render(const KnitSources& sources, const KnitMask& mask, ID3D11RenderTargetView* target,
    POINT offset, ResourceLedger::Lease* upload, ID3D11BlendState* blend) noexcept {
    if (!target || !sources.colour || !sources.heights || !sources.parameters || !sources.width || !sources.height ||
        sources.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || sources.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        offset.x < 0 || offset.y < 0 || !Valid(*sources.parameters) || !Valid(mask)) return E_INVALIDARG;
    HRESULT result = Initialize();
    if (FAILED(result)) return result;
    std::shared_ptr<Coordinates> coordinates[2];
    for (int axis = 0; axis < 2; ++axis) {
        const auto& p = *sources.parameters;
        result = CoordinateTable(axis ? sources.height : sources.width, axis ? p.originY : p.originX,
            p.scale, coordinates[axis], upload);
        if (FAILED(result)) return result;
    }
    const Placement placement{{mask.left, mask.top, mask.right, mask.bottom},
        float(offset.x), float(offset.y), mask.radius, mask.hole ? 1u : 0u};
    context_->ClearState();
    context_->UpdateSubresource(parameters_.Get(), 0, nullptr, sources.parameters, 0, 0);
    context_->UpdateSubresource(placement_.Get(), 0, nullptr, &placement, 0, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(knit_.Get(), nullptr, 0);
    ID3D11Buffer* buffers[] = {parameters_.Get(), placement_.Get()};
    context_->PSSetConstantBuffers(0, 2, buffers);
    ID3D11ShaderResourceView* views[] = {sources.colour, sources.heights,
        coordinates[0]->view.Get(), coordinates[1]->view.Get()};
    context_->PSSetShaderResources(0, 4, views);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->OMSetBlendState(blend, nullptr, 0xffffffffu);
    const D3D11_VIEWPORT viewport{float(offset.x), float(offset.y), float(sources.width), float(sources.height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->Draw(3, 0);
    ++draws_;
    ID3D11ShaderResourceView* emptyViews[4]{};
    context_->PSSetShaderResources(0, 4, emptyViews);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return device_->GetDeviceRemovedReason();
}

HRESULT GpuShader::Submit() noexcept {
    if (!device_) return E_UNEXPECTED;
    if (context_) context_->Flush();
    return device_->GetDeviceRemovedReason();
}
}
