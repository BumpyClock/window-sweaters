// SPDX-License-Identifier: GPL-3.0-only
#include "GpuSnapshot.h"
#include "GpuShader.h"
#include "KnitAssembler.h"
#include "../Knitting/WicSurface.h"
#include <cstring>
#include <stdexcept>

namespace window_sweaters::rendering {
using Microsoft::WRL::ComPtr;
using knitting::detail::Check;

struct GpuSnapshot::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11BlendState> blend;
    std::shared_ptr<ResourceLedger> resources = std::make_shared<ResourceLedger>();
    std::unique_ptr<KnitAssembler> assembler;
    std::unique_ptr<GpuShader> shader;

    explicit Impl(D3D_DRIVER_TYPE driver) {
        Check(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Create snapshot graphics device");
        D3D11_BLEND_DESC desc{};
        auto& target = desc.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        Check(device->CreateBlendState(&desc, &blend), "Create snapshot premultiplied blend");
        assembler = std::make_unique<KnitAssembler>(device.Get(), resources);
        shader = std::make_unique<GpuShader>(device.Get(), resources);
    }
};

GpuSnapshot::GpuSnapshot(D3D_DRIVER_TYPE driver) : impl_(std::make_unique<Impl>(driver)) {}
GpuSnapshot::~GpuSnapshot() = default;

knitting::RasterImage GpuSnapshot::Render(const KnitPerimeter& perimeter) {
    if (!perimeter.tile || !perimeter.width || !perimeter.height ||
        perimeter.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        perimeter.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        std::uint64_t(perimeter.width) * perimeter.height > 16u * 1024u * 1024u)
        throw std::invalid_argument("Snapshot dimensions exceed limits");
    std::uint64_t stripBytes = 0;
    for (const auto& piece : perimeter.pieces) {
        if (!perimeter.PieceValid(piece)) throw std::invalid_argument("Invalid snapshot perimeter strip");
        stripBytes += std::uint64_t(piece.width) * piece.height * 8u;
    }
    if (stripBytes > 64u * 1024u * 1024u) throw std::invalid_argument("Snapshot perimeter exceeds limits");
    auto& gpu = *impl_;
    ResourceLedger::Lease reservation;
    const auto recipe = std::make_shared<KnitPerimeter>(perimeter);
    const auto frameBytes = std::size_t(perimeter.width) * perimeter.height * 8u;
    auto admitted = gpu.resources->Reserve(recipe, reservation, frameBytes);
    if (admitted == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)) {
        gpu.shader->ClearCache();
        gpu.assembler->ClearCache();
        admitted = gpu.resources->Reserve(recipe, reservation, frameBytes);
    }
    Check(admitted, "Reserve snapshot graphics resources");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = perimeter.width; desc.Height = perimeter.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    Check(gpu.device->CreateTexture2D(&desc, nullptr, &texture), "Create snapshot frame texture");
    Check(gpu.device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Create snapshot frame target");
    const float clear[4]{};
    gpu.context->ClearRenderTargetView(target.Get(), clear);
    for (std::size_t side = 0; side < perimeter.pieces.size(); ++side) {
        const auto& piece = perimeter.pieces[side];
        ComPtr<ID3D11ShaderResourceView> colour, heights;
        Check(gpu.assembler->Assemble(perimeter, side, colour, heights, &reservation), "Assemble snapshot knit strip");
        const KnitSources sources{piece.width, piece.height, colour.Get(), heights.Get(),
            &perimeter.assembly.parameters[side]};
        const POINT offset{static_cast<LONG>(piece.x), static_cast<LONG>(piece.y)};
        Check(gpu.shader->Render(sources, {}, target.Get(), offset, &reservation, gpu.blend.Get()),
            "Shade snapshot knit strip");
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(gpu.device->CreateTexture2D(&desc, nullptr, &staging), "Create snapshot readback texture");
    knitting::RasterImage image{desc.Width, desc.Height, desc.Width * 4u, {}};
    image.pixels.resize(std::size_t(desc.Width) * desc.Height);
    gpu.context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read snapshot frame");
    for (std::uint32_t row = 0; row < image.height; ++row)
        std::memcpy(image.pixels.data() + std::size_t(row) * image.width,
            static_cast<const BYTE*>(mapped.pData) + std::size_t(row) * mapped.RowPitch, image.stride);
    gpu.context->Unmap(staging.Get(), 0);
    reservation.EndUpload();
    Check(gpu.device->GetDeviceRemovedReason(), "Complete snapshot frame");
    return image;
}
}
