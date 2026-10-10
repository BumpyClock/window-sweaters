#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>

namespace window_sweaters::rendering {

// The host UI thread owns one device set; overlays release their references first.
class GraphicsDevice final {
public:
    GraphicsDevice() = default;
    ~GraphicsDevice() { Destroy(); }
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    HRESULT Initialize() noexcept;
    void Destroy() noexcept;

    ID2D1Device* Drawing() const noexcept { return drawing_.Get(); }
    IDCompositionDevice* Composition() const noexcept { return composition_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> graphics_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> drawing_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_;
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
    HRESULT UpdateGeometry(const RECT& targetBounds) noexcept;
    HRESULT ReorderBelow(HWND target) noexcept;
    HRESULT Show() noexcept;
    void Hide() noexcept;
    void Destroy() noexcept;

    HWND Window() const noexcept { return window_; }
    const RECT& Bounds() const noexcept { return bounds_; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    HRESULT Draw(int width, int height) noexcept;

    HWND window_{};
    RECT bounds_{};
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface_;
};

}
