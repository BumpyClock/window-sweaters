#include "Overlay.h"
#include "Stacking.h"

#include <dxgi1_2.h>

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace window_sweaters::rendering {
namespace {
constexpr wchar_t OverlayClass[] = L"WindowSweaters.Overlay";

HRESULT LastError() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

HRESULT TargetView(ID3D11Texture2D* texture, UINT subresource,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& view) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!desc.MipLevels || subresource >= desc.MipLevels * desc.ArraySize) return E_UNEXPECTED;
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (desc.ArraySize > 1) {
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray = {subresource % desc.MipLevels, subresource / desc.MipLevels, 1};
    } else {
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = subresource;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    return device->CreateRenderTargetView(texture, &viewDesc, view.ReleaseAndGetAddressOf());
}

HRESULT ReadRegion(ID3D11Texture2D* texture, UINT subresource, POINT offset, std::uint32_t width,
    std::uint32_t height, knitting::RasterImage& image) noexcept {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT result = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(result)) return result;
    try {
        image = {width, height, width * 4, {}};
        image.pixels.resize(static_cast<std::size_t>(width) * height);
    } catch (...) { return E_OUTOFMEMORY; }
    const D3D11_BOX region{static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
        static_cast<UINT>(offset.x) + width, static_cast<UINT>(offset.y) + height, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, subresource, &region);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) return result;
    for (std::uint32_t row = 0; row < height; ++row)
        std::memcpy(image.pixels.data() + static_cast<std::size_t>(row) * width,
            static_cast<const BYTE*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch, width * 4u);
    context->Unmap(staging.Get(), 0);
    return S_OK;
}
}

HRESULT GraphicsDevice::Initialize(D3D_DRIVER_TYPE driver) noexcept {
    Destroy();
    HRESULT result = D3D11CreateDevice(nullptr,
        driver == D3D_DRIVER_TYPE_UNKNOWN ? D3D_DRIVER_TYPE_HARDWARE : driver, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        graphics_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    if (FAILED(result) && driver == D3D_DRIVER_TYPE_UNKNOWN) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            graphics_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    }
    if (FAILED(result)) return result;
    try {
        shader_ = std::make_shared<GpuShader>(graphics_.Get());
        assembler_ = std::make_shared<KnitAssembler>(graphics_.Get());
    } catch (...) { return E_OUTOFMEMORY; }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    result = graphics_.As(&dxgi);
    if (FAILED(result)) return result;
    return DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(composition_.ReleaseAndGetAddressOf()));
}

void GraphicsDevice::Destroy() noexcept {
    shader_.reset();
    assembler_.reset();
    composition_.Reset();
    graphics_.Reset();
}

LRESULT CALLBACK Overlay::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

HRESULT Overlay::Create(HINSTANCE instance, HWND hostWindow, GraphicsDevice& graphics) noexcept {
    if (window_) return E_UNEXPECTED;
    DWORD hostProcess{};
    if (!hostWindow || !GetWindowThreadProcessId(hostWindow, &hostProcess) || hostProcess != GetCurrentProcessId())
        return E_INVALIDARG;
    if (!graphics.Composition() || !graphics.Shader() || !graphics.Assembler()) return E_UNEXPECTED;
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = OverlayClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return LastError();

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, OverlayClass, L"Window Sweaters Border",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!window_) return LastError();
    if (!SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA)) return LastError();

    composition_ = graphics.Composition();
    resources_ = graphics.ResourceOwner();
    shader_ = graphics.Shader();
    assembler_ = graphics.Assembler();
    HRESULT result = composition_->CreateTargetForHwnd(window_, TRUE, target_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    result = composition_->CreateVisual(visual_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    return target_->SetRoot(visual_.Get());
}

HRESULT Overlay::UploadPiece(const KnitPerimeter& perimeter, std::size_t side, const RECT& exclusion,
    float radiusPixels, float tuckPixels, Microsoft::WRL::ComPtr<IDCompositionSurface>& surface,
    knitting::RasterImage* uploadedPixels) noexcept {
    const auto& piece = perimeter.pieces[side];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colour, heights;
    HRESULT result = perimeter.assembly ? assembler_->Assemble(perimeter, side, colour, heights)
        : shader_->Upload(piece.image, colour);
    if (FAILED(result)) return result;
    result = composition_->CreateSurface(piece.width, piece.height,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
        surface.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<IDXGISurface> drawingSurface;
    POINT offset{};
    result = surface->BeginDraw(nullptr, __uuidof(IDXGISurface),
        reinterpret_cast<void**>(drawingSurface.ReleaseAndGetAddressOf()), &offset);
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<IDXGISurface2> backing;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    UINT subresource{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    result = drawingSurface.As(&backing);
    if (SUCCEEDED(result)) result = backing->GetResource(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(texture.GetAddressOf()), &subresource);
    if (SUCCEEDED(result)) result = TargetView(texture.Get(), subresource, target);
    const float tuck = std::min(tuckPixels, std::min(
        static_cast<float>(exclusion.right - exclusion.left), static_cast<float>(exclusion.bottom - exclusion.top)) * .5f);
    KnitMask mask{static_cast<float>(exclusion.left) + tuck - piece.x, static_cast<float>(exclusion.top) + tuck - piece.y,
        static_cast<float>(exclusion.right) - tuck - piece.x, static_cast<float>(exclusion.bottom) - tuck - piece.y};
    mask.radius = std::min(std::max(0.f, radiusPixels - tuck), std::min(mask.right - mask.left, mask.bottom - mask.top) * .5f);
    mask.hole = mask.right > mask.left && mask.bottom > mask.top;
    const KnitSources sources{piece.width, piece.height, colour.Get(), heights.Get(),
        perimeter.assembly ? &perimeter.assembly->parameters[side] : nullptr};
    if (SUCCEEDED(result)) result = shader_->Render(sources, mask, target.Get(), offset);
    if (SUCCEEDED(result) && uploadedPixels)
        result = ReadRegion(texture.Get(), subresource, offset, piece.width, piece.height, *uploadedPixels);
    target.Reset();
    texture.Reset();
    backing.Reset();
    drawingSurface.Reset();
    const HRESULT endResult = surface->EndDraw();
    if (FAILED(result)) return result;
    return endResult;
}

HRESULT Overlay::Upload(std::shared_ptr<const KnitPerimeter> perimeter, const RECT& bounds,
    const RECT& clip, const RECT& exclusion, float radiusPixels, float tuckPixels,
    std::array<knitting::RasterImage, 4>* uploadedPixels) noexcept {
    if (!window_ || !perimeter || !perimeter->width || !perimeter->height ||
        perimeter->width > 32768 || perimeter->height > 32768 ||
        std::int64_t{bounds.right} - bounds.left != perimeter->width ||
        std::int64_t{bounds.bottom} - bounds.top != perimeter->height ||
        exclusion.left < 0 || exclusion.top < 0 || exclusion.right > static_cast<LONG>(perimeter->width) ||
        exclusion.bottom > static_cast<LONG>(perimeter->height) ||
        exclusion.right <= exclusion.left || exclusion.bottom <= exclusion.top ||
        clip.left < 0 || clip.top < 0 || clip.right > static_cast<LONG>(perimeter->width) ||
        clip.bottom > static_cast<LONG>(perimeter->height) || clip.right <= clip.left ||
        clip.bottom <= clip.top || !std::isfinite(radiusPixels) || radiusPixels < 0 ||
        !std::isfinite(tuckPixels) || tuckPixels < 0) return E_INVALIDARG;
    std::uint64_t bytes{};
    if (perimeter->assembly && !perimeter->tile) return E_INVALIDARG;
    for (const auto& piece : perimeter->pieces) {
        if (!perimeter->PieceValid(piece)) return E_INVALIDARG;
        bytes += std::uint64_t{piece.width} * piece.height * 4u * (perimeter->assembly ? 2u : 1u);
    }
    if (bytes > 64ull * 1024 * 1024) return E_OUTOFMEMORY;
    ResourceLedger::Lease reservation;
    const auto admitted=resources_->Reserve(perimeter,reservation);
    if (FAILED(admitted)) return admitted;
    Microsoft::WRL::ComPtr<IDCompositionVisual> nextRoot;
    std::array<Microsoft::WRL::ComPtr<IDCompositionSurface>, 4> surfaces;
    HRESULT result = composition_->CreateVisual(nextRoot.ReleaseAndGetAddressOf());
    for (std::size_t i = 0; i < surfaces.size() && SUCCEEDED(result); ++i) {
        const auto& piece = perimeter->pieces[i];
        result = UploadPiece(*perimeter, i, exclusion, radiusPixels, tuckPixels, surfaces[i],
            uploadedPixels ? &(*uploadedPixels)[i] : nullptr);
        Microsoft::WRL::ComPtr<IDCompositionVisual> child;
        if (SUCCEEDED(result)) result = composition_->CreateVisual(child.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) result = child->SetContent(surfaces[i].Get());
        if (SUCCEEDED(result)) result = child->SetOffsetX(static_cast<float>(piece.x));
        if (SUCCEEDED(result)) result = child->SetOffsetY(static_cast<float>(piece.y));
        if (SUCCEEDED(result)) result = nextRoot->AddVisual(child.Get(), TRUE, nullptr);
    }
    if (FAILED(result)) return result;
    result = nextRoot->SetClip(D2D1::RectF(static_cast<float>(clip.left), static_cast<float>(clip.top),
        static_cast<float>(clip.right), static_cast<float>(clip.bottom)));
    if (SUCCEEDED(result)) result = target_->SetRoot(nextRoot.Get());
    if (SUCCEEDED(result)) result = composition_->Commit();
    if (SUCCEEDED(result)) result = composition_->WaitForCommitCompletion();
    if (FAILED(result)) { Destroy(); return result; }
    ++commits_;
    visual_ = std::move(nextRoot);
    surfaces_ = std::move(surfaces);
    perimeter_ = std::move(perimeter);
    reservation.EndUpload();
    resourceLease_=std::move(reservation);
    clip_ = clip;
    return UpdatePlacement(bounds, clip);
}

HRESULT Overlay::UpdatePlacement(const RECT& next, const RECT& clip) noexcept {
    if (!window_ || !perimeter_) return E_UNEXPECTED;
    const auto width = std::int64_t{next.right} - next.left;
    const auto height = std::int64_t{next.bottom} - next.top;
    if (width != perimeter_->width || height != perimeter_->height ||
        clip.left < 0 || clip.top < 0 || clip.right > width || clip.bottom > height ||
        clip.right <= clip.left || clip.bottom <= clip.top) return E_INVALIDARG;
    if (!EqualRect(&clip, &clip_)) {
        HRESULT result = visual_->SetClip(D2D1::RectF(static_cast<float>(clip.left), static_cast<float>(clip.top),
            static_cast<float>(clip.right), static_cast<float>(clip.bottom)));
        if (SUCCEEDED(result)) result = composition_->Commit();
        if (FAILED(result)) return result;
        ++commits_;
        clip_ = clip;
    }
    if (!SetWindowPos(window_, nullptr, next.left, next.top, static_cast<int>(width), static_cast<int>(height),
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)) return LastError();
    bounds_ = next;
    return S_OK;
}

HRESULT Overlay::ReorderBelow(HWND target) noexcept {
    return PlaceOverlayBelow(window_, target);
}

HRESULT Overlay::Show() noexcept {
    if (!window_ || !perimeter_) return E_UNEXPECTED;
    return SetWindowPos(window_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) ? S_OK : LastError();
}

void Overlay::Hide() noexcept {
    if (window_) ShowWindow(window_, SW_HIDE);
}

void Overlay::Destroy() noexcept {
    ReleaseContent();
    target_.Reset();
    composition_.Reset();
    resources_.reset();
    shader_.reset();
    assembler_.reset();
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
    bounds_ = clip_ = {};
    commits_ = 0;
}

void Overlay::ReleaseContent() noexcept {
    Hide();
    if (!perimeter_ && !visual_) return;
    auto detached=target_?target_->SetRoot(nullptr):S_OK;
    if (SUCCEEDED(detached) && composition_) {
        detached=composition_->Commit();
        if (SUCCEEDED(detached)) detached=composition_->WaitForCommitCompletion();
    }
    if (FAILED(detached)) {
        target_.Reset();
        if (window_) DestroyWindow(window_);
        window_=nullptr;
    }
    for (auto& surface : surfaces_) surface.Reset();
    perimeter_.reset();
    visual_.Reset();
    resourceLease_.Reset();
    bounds_ = clip_ = {};
}

}
