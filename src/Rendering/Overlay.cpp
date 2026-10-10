#include "Overlay.h"
#include "Stacking.h"

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
}

HRESULT GraphicsDevice::Initialize() noexcept {
    Destroy();
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        graphics_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    if (FAILED(result)) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            graphics_.ReleaseAndGetAddressOf(), nullptr, nullptr);
    }
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    result = graphics_.As(&dxgi);
    if (FAILED(result)) return result;
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        factory_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    result = factory_->CreateDevice(dxgi.Get(), drawing_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    return DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(composition_.ReleaseAndGetAddressOf()));
}

void GraphicsDevice::Destroy() noexcept {
    composition_.Reset();
    drawing_.Reset();
    factory_.Reset();
    graphics_.Reset();
}

LRESULT CALLBACK Overlay::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    // This is defensive only: cross-process click-through comes from LAYERED|TRANSPARENT.
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
    if (!graphics.Drawing() || !graphics.Composition()) return E_UNEXPECTED;
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = OverlayClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return LastError();

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, OverlayClass, L"Window Sweaters Border",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!window_) return LastError();
    // DComp supplies per-pixel alpha. No redirected GDI bitmap or UpdateLayeredWindow is mixed in.
    if (!SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA)) return LastError();

    composition_ = graphics.Composition();
    HRESULT result = graphics.Drawing()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        context_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    context_->SetDpi(96.0f, 96.0f);
    result = composition_->CreateTargetForHwnd(window_, TRUE, target_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    result = composition_->CreateVisual(visual_.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;
    return target_->SetRoot(visual_.Get());
}

HRESULT Overlay::UploadPiece(const KnitRasterPiece& piece, const RECT& exclusion, float radiusPixels, float tuckPixels,
    Microsoft::WRL::ComPtr<IDCompositionSurface>& surface, knitting::RasterImage* uploadedPixels) noexcept {
    const auto& image = piece.image;
    HRESULT result = composition_->CreateSurface(image.width, image.height,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
        surface.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<IDXGISurface> drawingSurface;
    POINT offset{};
    result = surface->BeginDraw(nullptr, __uuidof(IDXGISurface),
        reinterpret_cast<void**>(drawingSurface.ReleaseAndGetAddressOf()), &offset);
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap, source;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    result = context_->CreateBitmapFromDxgiSurface(drawingSurface.Get(), &properties,
        bitmap.ReleaseAndGetAddressOf());
    const auto sourceProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (SUCCEEDED(result)) result = context_->CreateBitmap(D2D1::SizeU(image.width, image.height),
        image.pixels.data(), image.stride, &sourceProperties, source.ReleaseAndGetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    context_->GetFactory(factory.ReleaseAndGetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> outer;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> inner;
    Microsoft::WRL::ComPtr<ID2D1GeometryGroup> mask;
    const auto extent = D2D1::RectF(0, 0, static_cast<float>(image.width), static_cast<float>(image.height));
    if (SUCCEEDED(result)) result = factory->CreateRectangleGeometry(extent, outer.ReleaseAndGetAddressOf());
    const float tuck = std::min(tuckPixels, std::min(
        static_cast<float>(exclusion.right - exclusion.left), static_cast<float>(exclusion.bottom - exclusion.top)) * .5f);
    const auto content = D2D1::RectF(
        static_cast<float>(exclusion.left) + tuck - piece.x, static_cast<float>(exclusion.top) + tuck - piece.y,
        static_cast<float>(exclusion.right) - tuck - piece.x, static_cast<float>(exclusion.bottom) - tuck - piece.y);
    const float radius = std::min(std::max(0.f, radiusPixels - tuck),
        std::min(content.right - content.left, content.bottom - content.top) * .5f);
    const bool hasHole = content.right > content.left && content.bottom > content.top;
    if (SUCCEEDED(result) && hasHole) result = factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(content, radius, radius), inner.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) {
        ID2D1Geometry* contours[] = {outer.Get(), inner.Get()};
        result = factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, contours, hasHole ? 2u : 1u, mask.ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(result)) {
        context_->SetTarget(bitmap.Get());
        context_->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
            static_cast<float>(offset.y)));
        context_->BeginDraw();
        context_->PushAxisAlignedClip(extent, D2D1_ANTIALIAS_MODE_ALIASED);
        context_->Clear(D2D1::ColorF(0, 0, 0, 0));
        // Retain the source's inward tuck beneath the separately validated target;
        // the central client interior remains transparent.
        context_->PushLayer(D2D1::LayerParameters1(extent, mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
        context_->DrawBitmap(source.Get(), extent, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        context_->PopLayer();
        context_->PopAxisAlignedClip();
        result = context_->EndDraw();
        context_->SetTarget(nullptr);
    }
    if (SUCCEEDED(result) && uploadedPixels) {
        // Hidden native fixtures read the real masked GPU pixels. Production
        // leaves this null and allocates no staging bitmap or CPU perimeter.
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> staging;
        const auto readProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        result = context_->CreateBitmap(D2D1::SizeU(image.width, image.height), nullptr, 0,
            &readProperties, staging.ReleaseAndGetAddressOf());
        const D2D1_RECT_U region{static_cast<UINT32>(offset.x), static_cast<UINT32>(offset.y),
            static_cast<UINT32>(offset.x) + image.width, static_cast<UINT32>(offset.y) + image.height};
        if (SUCCEEDED(result)) result = staging->CopyFromBitmap(nullptr, bitmap.Get(), &region);
        if (SUCCEEDED(result)) {
            try {
                uploadedPixels->width = image.width;
                uploadedPixels->height = image.height;
                uploadedPixels->stride = image.width * 4;
                uploadedPixels->pixels.resize(static_cast<std::size_t>(image.width) * image.height);
            } catch (...) { result = E_OUTOFMEMORY; }
        }
        D2D1_MAPPED_RECT mapped{};
        if (SUCCEEDED(result)) result = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (SUCCEEDED(result)) {
            for (UINT32 row = 0; row < image.height; ++row)
                std::memcpy(uploadedPixels->pixels.data() + static_cast<std::size_t>(row) * image.width,
                    mapped.bits + static_cast<std::size_t>(row) * mapped.pitch, image.width * 4u);
            result = staging->Unmap();
        }
    }
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
    for (const auto& piece : perimeter->pieces) {
        const auto& image = piece.image;
        if (!image.width || !image.height || piece.x > perimeter->width || piece.y > perimeter->height ||
            image.width > perimeter->width - piece.x || image.height > perimeter->height - piece.y ||
            image.stride != image.width * 4u ||
            image.pixels.size() != static_cast<std::size_t>(image.width) * image.height) return E_INVALIDARG;
        bytes += image.ByteSize();
    }
    if (bytes > 64ull * 1024 * 1024) return E_OUTOFMEMORY;
    Microsoft::WRL::ComPtr<IDCompositionVisual> nextRoot;
    std::array<Microsoft::WRL::ComPtr<IDCompositionSurface>, 4> surfaces;
    HRESULT result = composition_->CreateVisual(nextRoot.ReleaseAndGetAddressOf());
    for (std::size_t i = 0; i < surfaces.size() && SUCCEEDED(result); ++i) {
        const auto& piece = perimeter->pieces[i];
        result = UploadPiece(piece, exclusion, radiusPixels, tuckPixels, surfaces[i],
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
    if (FAILED(result)) { Hide(); return result; }
    visual_ = std::move(nextRoot);
    surfaces_ = std::move(surfaces);
    perimeter_ = std::move(perimeter);
    return UpdatePlacement(bounds, clip);
}

HRESULT Overlay::UpdatePlacement(const RECT& next, const RECT& clip) noexcept {
    if (!window_ || !perimeter_) return E_UNEXPECTED;
    const auto width = std::int64_t{next.right} - next.left;
    const auto height = std::int64_t{next.bottom} - next.top;
    if (width != perimeter_->width || height != perimeter_->height ||
        clip.left < 0 || clip.top < 0 || clip.right > width || clip.bottom > height ||
        clip.right <= clip.left || clip.bottom <= clip.top) return E_INVALIDARG;
    HRESULT result = visual_->SetClip(D2D1::RectF(static_cast<float>(clip.left), static_cast<float>(clip.top),
        static_cast<float>(clip.right), static_cast<float>(clip.bottom)));
    if (SUCCEEDED(result)) result = composition_->Commit();
    if (FAILED(result)) return result;
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
    Hide();
    if (target_) target_->SetRoot(nullptr);
    if (composition_) composition_->Commit();
    for (auto& surface : surfaces_) surface.Reset();
    perimeter_.reset();
    visual_.Reset();
    target_.Reset();
    context_.Reset();
    composition_.Reset();
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
    bounds_ = {};
}

}
