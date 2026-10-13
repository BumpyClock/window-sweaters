#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>
#include "KnitRaster.h"
#include "ResourceLedger.h"
#include "GpuShader.h"
#include "KnitAssembler.h"
#include <optional>
#include <span>

namespace window_sweaters::rendering {

struct PublicationStatistics {
    std::uint64_t commits{}, waits{};
};

class GraphicsDevice final {
public:
    GraphicsDevice() = default;
    ~GraphicsDevice() { Destroy(); }
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    HRESULT Initialize(D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_UNKNOWN) noexcept;
    void Destroy() noexcept;

    IDCompositionDevice* Composition() const noexcept { return composition_.Get(); }
    ResourceLedger& Resources() const noexcept { return *resources_; }
    std::shared_ptr<ResourceLedger> ResourceOwner() const noexcept { return resources_; }
    std::shared_ptr<GpuShader> Shader() const noexcept { return shader_; }
    std::shared_ptr<KnitAssembler> Assembler() const noexcept { return assembler_; }
    PublicationStatistics PublicationStatus() const noexcept { return *publicationStats_; }
    std::shared_ptr<PublicationStatistics> PublicationOwner() const noexcept { return publicationStats_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> graphics_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_;
    std::shared_ptr<ResourceLedger> resources_{std::make_shared<ResourceLedger>()};
    std::shared_ptr<GpuShader> shader_;
    std::shared_ptr<KnitAssembler> assembler_;
    std::shared_ptr<PublicationStatistics> publicationStats_{std::make_shared<PublicationStatistics>()};
};

class Overlay final {
public:
    static constexpr int BorderWidthPixels = 12;
    static constexpr COLORREF BorderColor = RGB(64, 190, 220);

    Overlay() = default;
    ~Overlay() { Destroy(); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    HRESULT Create(HINSTANCE instance, HWND hostWindow, GraphicsDevice& graphics) noexcept;
    HRESULT Upload(std::shared_ptr<const KnitPerimeter> perimeter, const RECT& physicalOuterBounds,
        const RECT& clipRelativeToOuter, const RECT& contentExclusion, float radiusPixels = 0,
        float tuckPixels = 0, std::array<knitting::RasterImage, 4>* uploadedPixels = nullptr) noexcept;
    HRESULT PrepareUpload(std::shared_ptr<const KnitPerimeter> perimeter, const RECT& physicalOuterBounds,
        const RECT& clipRelativeToOuter, const RECT& contentExclusion, float radiusPixels = 0,
        float tuckPixels = 0, std::array<knitting::RasterImage, 4>* uploadedPixels = nullptr) noexcept;
    static HRESULT PublishUploads(std::span<Overlay* const> overlays) noexcept;
    void DiscardUpload() noexcept { pendingUpload_.reset(); }
    std::uint64_t PreparedUploadId() const noexcept { return pendingUpload_ ? uploadId_ : 0; }
    HRESULT UpdatePlacement(const RECT& physicalOuterBounds, const RECT& clipRelativeToOuter) noexcept;
    HRESULT ReorderBelow(HWND target) noexcept;
    HRESULT Show() noexcept;
    void Hide() noexcept;
    void ReleaseContent() noexcept;
    void Destroy() noexcept;

    HWND Window() const noexcept { return window_; }
    const RECT& Bounds() const noexcept { return bounds_; }
    std::uint32_t CommitCount() const noexcept { return commits_; }

private:
    friend struct OverlayPublicationTest;
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    HRESULT UploadPiece(const KnitPerimeter& perimeter, std::size_t side, const RECT& contentExclusion, float radiusPixels, float tuckPixels,
        Microsoft::WRL::ComPtr<IDCompositionSurface>& surface, knitting::RasterImage* uploadedPixels,
        ResourceLedger::Lease* reservation) noexcept;

    HWND window_{};
    RECT bounds_{};
    RECT clip_{};
    std::uint32_t commits_{};
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    std::array<Microsoft::WRL::ComPtr<IDCompositionSurface>, 4> surfaces_;
    std::shared_ptr<const KnitPerimeter> perimeter_;
    std::shared_ptr<ResourceLedger> resources_;
    ResourceLedger::Lease resourceLease_;
    std::shared_ptr<GpuShader> shader_;
    std::shared_ptr<KnitAssembler> assembler_;
    std::shared_ptr<PublicationStatistics> publicationStats_;
    struct PendingUpload {
        ResourceLedger::Lease lease;
        Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
        std::array<Microsoft::WRL::ComPtr<IDCompositionSurface>, 4> surfaces;
        std::shared_ptr<const KnitPerimeter> perimeter;
        RECT clip;
    };
    std::optional<PendingUpload> pendingUpload_;
    std::uint64_t uploadId_{};
};

}
