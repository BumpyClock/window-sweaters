#include "Overlay.h"

#include <cstdint>
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

HRESULT Overlay::Draw(int width, int height) noexcept {
    Microsoft::WRL::ComPtr<IDCompositionSurface> nextSurface;
    HRESULT result = composition_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
        nextSurface.ReleaseAndGetAddressOf());
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<IDXGISurface> drawingSurface;
    POINT offset{};
    result = nextSurface->BeginDraw(nullptr, __uuidof(IDXGISurface),
        reinterpret_cast<void**>(drawingSurface.ReleaseAndGetAddressOf()), &offset);
    if (FAILED(result)) return result;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    result = context_->CreateBitmapFromDxgiSurface(drawingSurface.Get(), &properties,
        bitmap.ReleaseAndGetAddressOf());
    if (SUCCEEDED(result)) {
        context_->SetTarget(bitmap.Get());
        context_->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
            static_cast<float>(offset.y)));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        result = context_->CreateSolidColorBrush(D2D1::ColorF(64.0f / 255.0f, 190.0f / 255.0f,
            220.0f / 255.0f, 1.0f), brush.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) {
            const float w = static_cast<float>(width);
            const float h = static_cast<float>(height);
            const float b = static_cast<float>(BorderWidthPixels);
            context_->BeginDraw();
            context_->PushAxisAlignedClip(D2D1::RectF(0, 0, w, h), D2D1_ANTIALIAS_MODE_ALIASED);
            context_->Clear(D2D1::ColorF(0, 0, 0, 0));
            context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            context_->FillRectangle(D2D1::RectF(0, 0, w, b), brush.Get());
            context_->FillRectangle(D2D1::RectF(0, h - b, w, h), brush.Get());
            context_->FillRectangle(D2D1::RectF(0, b, b, h - b), brush.Get());
            context_->FillRectangle(D2D1::RectF(w - b, b, w, h - b), brush.Get());
            context_->PopAxisAlignedClip();
            result = context_->EndDraw();
        }
        context_->SetTarget(nullptr);
    }
    const HRESULT endResult = nextSurface->EndDraw();
    if (FAILED(result)) return result;
    if (FAILED(endResult)) return endResult;
    result = visual_->SetContent(nextSurface.Get());
    if (FAILED(result)) return result;
    result = composition_->Commit();
    if (FAILED(result)) return result;
    surface_ = std::move(nextSurface);
    return S_OK;
}

HRESULT Overlay::UpdateGeometry(const RECT& targetBounds) noexcept {
    if (!window_) return E_UNEXPECTED;
    constexpr int border = BorderWidthPixels;
    const std::int64_t width = static_cast<std::int64_t>(targetBounds.right) - targetBounds.left + 2 * border;
    const std::int64_t height = static_cast<std::int64_t>(targetBounds.bottom) - targetBounds.top + 2 * border;
    if (width <= 2 * border || height <= 2 * border || width > 16384 || height > 16384 ||
        targetBounds.left < std::numeric_limits<LONG>::min() + border ||
        targetBounds.top < std::numeric_limits<LONG>::min() + border ||
        targetBounds.right > std::numeric_limits<LONG>::max() - border ||
        targetBounds.bottom > std::numeric_limits<LONG>::max() - border) return E_INVALIDARG;

    RECT next{targetBounds.left - border, targetBounds.top - border,
        targetBounds.right + border, targetBounds.bottom + border};
    const bool resize = bounds_.right - bounds_.left != width || bounds_.bottom - bounds_.top != height;
    if (resize) {
        const HRESULT result = Draw(static_cast<int>(width), static_cast<int>(height));
        if (FAILED(result)) return result;
    }
    if (!SetWindowPos(window_, nullptr, next.left, next.top, static_cast<int>(width), static_cast<int>(height),
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)) return LastError();
    bounds_ = next;
    return S_OK;
}

HRESULT Overlay::ReorderBelow(HWND target) noexcept {
    if (!window_ || !IsWindow(target)) return E_INVALIDARG;
    // This slice supports restored ordinary targets; topmost-band tracking is a later concern.
    if (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) return E_INVALIDARG;
    if (GetWindow(target, GW_HWNDNEXT) == window_) return S_OK;
    if (!SetWindowPos(window_, target, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return LastError();
    return GetWindow(target, GW_HWNDNEXT) == window_ ? S_OK : HRESULT_FROM_WIN32(ERROR_RETRY);
}

HRESULT Overlay::Show() noexcept {
    if (!window_ || !surface_) return E_UNEXPECTED;
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
    surface_.Reset();
    visual_.Reset();
    target_.Reset();
    context_.Reset();
    composition_.Reset();
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
    bounds_ = {};
}

}
