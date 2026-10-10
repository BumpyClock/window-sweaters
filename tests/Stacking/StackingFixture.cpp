// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Rendering\Stacking.h"
#include "MenuCapture.h"

#include <commctrl.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace window_sweaters::rendering;
using namespace window_sweaters::stacking_fixture;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t TargetClass[] = L"WindowSweaters.StackingFixture.Target";
constexpr wchar_t ReceiverClass[] = L"WindowSweaters.StackingFixture.Receiver";
constexpr wchar_t RingClass[] = L"WindowSweaters.StackingFixture.Ring";
constexpr UINT Changed = WM_APP + 1;
constexpr UINT Capture = WM_APP + 2;
constexpr UINT Reconcile = WM_APP + 3;
constexpr UINT Stop = WM_APP + 4;
constexpr UINT FocusTarget = WM_APP + 5;
constexpr UINT ResetCase = WM_APP + 6;
constexpr UINT CaptureMenu = WM_APP + 7;
constexpr UINT MenuCaptureEnded = WM_APP + 8;
constexpr int TopmostButton = 101, DialogButton = 102, MenuButton = 103, TooltipButton = 104, CaptureButton = 105;
constexpr int CreateOverlapButton = 201, FocusAButton = 202, FocusBButton = 203, ModalAButton = 204,
    TopmostAButton = 205, TopmostBButton = 206, SamplePixelsButton = 207, CountersButton = 208,
    CloseFixtureButton = 209, MenuAButton = 210, TooltipAButton = 211;
constexpr UINT_PTR ExitTimer = 1;
constexpr int Border = 16;
constexpr std::array<COLORREF, 2> TargetColors{RGB(37, 85, 51), RGB(43, 49, 99)};
constexpr std::array<COLORREF, 2> RingColors{RGB(64, 190, 220), RGB(230, 65, 125)};
constexpr COLORREF ReceiverColor = RGB(31, 34, 40);
constexpr COLORREF DialogColor = RGB(115, 65, 25);
constexpr DWORD OverlayStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
    WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
constexpr UINT PositionFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Require(HRESULT result, const char* message) {
    Require(SUCCEEDED(result), message);
}

struct PeerState {
    HWND controller{};
    DWORD controllerPid{};
    HWND target{};
    HWND dialog{};
    HWND tooltip{};
    MenuCapture menuCapture;
    RECT bounds{};
    unsigned index{};
    unsigned clicks{};
    unsigned wheels{};
    unsigned keys{};
    POINT lastClick{};
};

class Mapping final {
public:
    Mapping(const std::wstring& name, bool create) {
        handle_ = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                             sizeof(PeerState), name.c_str())
                         : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        Require(handle_ != nullptr, "fixture IPC mapping unavailable");
        state = static_cast<PeerState*>(MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PeerState)));
        if (!state) {
            CloseHandle(handle_);
            handle_ = nullptr;
            Require(false, "fixture IPC view unavailable");
        }
    }
    ~Mapping() {
        if (state) UnmapViewOfFile(state);
        if (handle_) CloseHandle(handle_);
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    PeerState* state{};
private:
    HANDLE handle_{};
};

PeerState* State(HWND window) {
    return reinterpret_cast<PeerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void Notify(PeerState& state, UINT message = Changed) {
    DWORD process{};
    if (GetWindowThreadProcessId(state.controller, &process) && process == state.controllerPid)
        PostMessageW(state.controller, message, state.index, 0);
}

void Paint(HWND window, COLORREF color, const wchar_t* text, int textTop = 65) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &client, brush);
    DeleteObject(brush);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    client.left += 20;
    client.top += textTop;
    DrawTextW(dc, text, -1, &client, DT_LEFT | DT_TOP | DT_WORDBREAK);
    EndPaint(window, &paint);
}

struct DialogTemplate {
    DLGTEMPLATE dialog{};
    WORD menu{};
    WORD windowClass{};
    wchar_t title[32]{L"Owned modal dialog"};
};

INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        auto state = reinterpret_cast<PeerState*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->dialog = window;
        RECT bounds{};
        GetWindowRect(state->target, &bounds);
        SetWindowPos(window, nullptr, bounds.left - Border, bounds.top + 120, 240, 150,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        CreateWindowW(L"BUTTON", L"Close dialog", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      20, 65, 170, 30, window, reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
        Notify(*state);
        return TRUE;
    }
    if (message == WM_PAINT) {
        Paint(window, DialogColor, L"Modal dialog covers target yarn.");
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        EndDialog(window, IDOK);
        return TRUE;
    }
    if (message == WM_CLOSE) {
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

LRESULT CALLBACK TargetProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto state = State(window);
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_ENTERIDLE: {
        const HWND menuWindow = reinterpret_cast<HWND>(lparam);
        DWORD process{};
        std::array<wchar_t, 32> className{};
        const bool visibleOwnedMenu = IsWindowVisible(menuWindow) &&
            GetWindowThreadProcessId(menuWindow, &process) && process == GetCurrentProcessId() &&
            GetClassNameW(menuWindow, className.data(), static_cast<int>(className.size())) &&
            std::wstring_view(className.data()) == L"#32768";
        if (state->menuCapture.ObserveIdle(wparam == MSGF_MENU, visibleOwnedMenu)) {
            DWORD controllerProcess{};
            DWORD_PTR measured{};
            const bool controllerOwned = GetWindowThreadProcessId(state->controller, &controllerProcess) &&
                controllerProcess == state->controllerPid;
            // Send synchronously while the peer remains in its visible native menu loop.
            const bool captured = controllerOwned && SendMessageTimeoutW(state->controller, CaptureMenu,
                state->index, reinterpret_cast<LPARAM>(menuWindow), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &measured) &&
                measured != 0;
            state->menuCapture.Complete(captured);
        }
        return 0;
    }
    case FocusTarget:
        SetForegroundWindow(window);
        SetFocus(window);
        Notify(*state);
        return 0;
    case ResetCase:
        if (state->dialog) EndDialog(state->dialog, IDCANCEL);
        if (state->tooltip) {
            DestroyWindow(state->tooltip);
            state->tooltip = nullptr;
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
        SetWindowPos(window, HWND_NOTOPMOST, state->bounds.left, state->bounds.top,
                     state->bounds.right - state->bounds.left, state->bounds.bottom - state->bounds.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        Notify(*state);
        return 0;
    case WM_WINDOWPOSCHANGED:
    case WM_ACTIVATE:
        Notify(*state);
        break;
    case WM_LBUTTONDOWN: {
        ++state->clicks;
        state->lastClick = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ClientToScreen(window, &state->lastClick);
        Notify(*state);
        break;
    }
    case WM_MOUSEWHEEL:
        ++state->wheels;
        Notify(*state);
        break;
    case WM_KEYDOWN:
        ++state->keys;
        Notify(*state);
        break;
    case WM_PAINT:
        Paint(window, TargetColors[state->index],
              L"Click the solid body, scroll, and type.\nSwitch A/B focus by clicking their titles.\n"
              L"Buttons open native owned/modal, menu, tooltip surfaces.\n"
              L"Capture writes actual sampled pixels and input counters; it never injects input.");
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case TopmostButton: {
            const bool topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            SetWindowPos(window, topmost ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, PositionFlags);
            Notify(*state);
            break;
        }
        case DialogButton: {
            DialogTemplate modal{};
            modal.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
            modal.dialog.cx = 180;
            modal.dialog.cy = 100;
            DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &modal.dialog, window, DialogProc,
                                    reinterpret_cast<LPARAM>(state));
            state->dialog = nullptr;
            Notify(*state);
            break;
        }
        case MenuButton: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Native menu covers yarn");
            AppendMenuW(menu, MF_STRING, 2, L"Dismiss menu (capture occurs while open)");
            RECT bounds{};
            GetWindowRect(window, &bounds);
            state->menuCapture.Begin();
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                bounds.left - Border, bounds.top + 150, 0, window, nullptr);
            state->menuCapture.End();
            DestroyMenu(menu);
            Notify(*state, MenuCaptureEnded);
            Notify(*state);
            break;
        }
        case TooltipButton:
            if (state->tooltip) {
                DestroyWindow(state->tooltip);
                state->tooltip = nullptr;
            } else {
                state->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                    WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, window, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
                TOOLINFOW tool{sizeof(tool)};
                tool.uFlags = TTF_TRACK | TTF_ABSOLUTE;
                tool.hwnd = window;
                tool.uId = 1;
                tool.lpszText = const_cast<wchar_t*>(L"Native tooltip must cover background yarn");
                SendMessageW(state->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                RECT bounds{};
                GetWindowRect(window, &bounds);
                SendMessageW(state->tooltip, TTM_TRACKPOSITION, 0,
                             MAKELPARAM(bounds.left - Border, bounds.top + 180));
                SendMessageW(state->tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tool));
            }
            Notify(*state);
            break;
        case CaptureButton:
            Notify(*state, Capture);
            break;
        }
        return 0;
    case Stop:
    case WM_CLOSE:
        if (state->dialog) EndDialog(state->dialog, IDCANCEL);
        if (state->tooltip) DestroyWindow(state->tooltip);
        DestroyWindow(window);
        return 0;
    case WM_TIMER:
        if (wparam == ExitTimer) PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void RegisterClass(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpfnWndProc = proc;
    windowClass.lpszClassName = name;
    Require(RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "register fixture window class");
}

int MessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

int RunPeer(const std::wstring& name) {
    Mapping mapping(name, false);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    Require(InitCommonControlsEx(&controls) != FALSE, "initialize native tooltip controls");
    RegisterClass(TargetClass, TargetProc);
    auto& state = *mapping.state;
    const auto bounds = state.bounds;
    state.target = CreateWindowExW(0, TargetClass, state.index ? L"Stacking target B (separate process)"
        : L"Stacking target A (separate process)", WS_OVERLAPPEDWINDOW,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    Require(state.target != nullptr, "create separate-process fixture target");
    const std::array captions{L"Topmost", L"Dialog", L"Menu", L"Tooltip", L"Capture"};
    for (unsigned index = 0; index < captions.size(); ++index) {
        CreateWindowW(L"BUTTON", captions[index], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      8 + static_cast<int>(index) * 76, 10, 72, 30, state.target,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(TopmostButton + index)),
                      GetModuleHandleW(nullptr), nullptr);
    }
    ShowWindow(state.target, SW_SHOWNOACTIVATE);
    UpdateWindow(state.target);
    SetTimer(state.target, ExitTimer, 15 * 60 * 1000, nullptr);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L".ready").c_str());
    Require(ready && SetEvent(ready), "signal visible fixture target readiness");
    CloseHandle(ready);
    return MessageLoop();
}

class Peer final {
public:
    Peer(HWND controller, RECT bounds, unsigned index, HANDLE job)
        : name_(L"Local\\WindowSweaters.StackingFixture." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(index)),
          mapping(name_, true) {
        *mapping.state = {};
        mapping.state->controller = controller;
        mapping.state->controllerPid = GetCurrentProcessId();
        mapping.state->bounds = bounds;
        mapping.state->index = index;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (name_ + L".ready").c_str());
        Require(ready != nullptr, "create visible peer readiness event");
        std::array<wchar_t, 32768> executable{};
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --peer \"" + name_ + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        const bool launched = CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) != FALSE;
        if (!launched) {
            CloseHandle(ready);
            Require(false, "launch own opt-in visible fixture peer");
        }
        process_ = process.hProcess;
        if (!AssignProcessToJobObject(job, process_) || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 2000);
            CloseHandle(process_);
            process_ = nullptr;
            CloseHandle(process.hThread);
            CloseHandle(ready);
            Require(false, "bind visible peer lifetime to fixture");
        }
        CloseHandle(process.hThread);
        const std::array waits{ready, process_};
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 10000);
        CloseHandle(ready);
        if (wait != WAIT_OBJECT_0 || !LiveTarget()) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 2000);
            CloseHandle(process_);
            process_ = nullptr;
            Require(false, "visible fixture peer did not become ready");
        }
    }
    ~Peer() {
        if (LiveTarget()) PostMessageW(mapping.state->target, Stop, 0, 0);
        if (process_) {
            WaitForSingleObject(process_, 2000);
            CloseHandle(process_);
        }
    }
    DWORD ProcessId() const { return process_ ? GetProcessId(process_) : 0; }
    bool LiveTarget() const {
        DWORD process{};
        return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT &&
            GetWindowThreadProcessId(mapping.state->target, &process) && process == ProcessId();
    }
private:
    std::wstring name_;
public:
    Mapping mapping;
private:
    HANDLE process_{};
};

class Ring final {
public:
    Ring() = default;
    ~Ring() {
        if (window) ShowWindow(window, SW_HIDE);
        if (target_) target_->SetRoot(nullptr);
        if (composition_) composition_->Commit();
        visual_.Reset();
        surface_.Reset();
        target_.Reset();
        if (window) DestroyWindow(window);
    }
    void Create(ID2D1Device* device, IDCompositionDevice* composition, COLORREF color) {
        composition_ = composition;
        window = CreateWindowExW(OverlayStyle, RingClass, L"Stacking fixture overlay", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr && SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA), "create native noninteractive ring");
        Require(composition->CreateTargetForHwnd(window, TRUE, &target_), "create ring composition target");
        Require(composition->CreateVisual(&visual_), "create ring visual");
        Require(target_->SetRoot(visual_.Get()), "attach ring visual");
        Require(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &drawing_), "create fixture ring drawing context");
        color_ = color;
    }
    void Geometry(const RECT& bounds) {
        const int width = bounds.right - bounds.left + 2 * Border;
        const int height = bounds.bottom - bounds.top + 2 * Border;
        if (width != width_ || height != height_) {
            Require(composition_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, surface_.ReleaseAndGetAddressOf()),
                "create fixture ring surface");
            ComPtr<IDXGISurface> surface;
            POINT offset{};
            Require(surface_->BeginDraw(nullptr, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf()), &offset),
                    "begin fixture ring draw");
            ComPtr<ID2D1Bitmap1> bitmap;
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Require(drawing_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap), "create fixture ring bitmap");
            drawing_->SetTarget(bitmap.Get());
            drawing_->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
            ComPtr<ID2D1SolidColorBrush> brush;
            Require(drawing_->CreateSolidColorBrush(D2D1::ColorF(GetRValue(color_) / 255.0f,
                GetGValue(color_) / 255.0f, GetBValue(color_) / 255.0f), &brush), "create fixture ring brush");
            const float w = static_cast<float>(width), h = static_cast<float>(height), border = Border;
            drawing_->BeginDraw();
            drawing_->PushAxisAlignedClip(D2D1::RectF(0, 0, w, h), D2D1_ANTIALIAS_MODE_ALIASED);
            drawing_->Clear(D2D1::ColorF(0, 0, 0, 0));
            drawing_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            drawing_->FillRectangle(D2D1::RectF(0, 0, w, border), brush.Get());
            drawing_->FillRectangle(D2D1::RectF(0, h - border, w, h), brush.Get());
            drawing_->FillRectangle(D2D1::RectF(0, border, border, h - border), brush.Get());
            drawing_->FillRectangle(D2D1::RectF(w - border, border, w, h - border), brush.Get());
            drawing_->PopAxisAlignedClip();
            const HRESULT drawn = drawing_->EndDraw();
            drawing_->SetTarget(nullptr);
            Require(surface_->EndDraw(), "end fixture ring composition surface");
            Require(drawn, "draw fixture ring");
            Require(visual_->SetContent(surface_.Get()), "attach fixture ring surface");
            Require(composition_->Commit(), "commit fixture ring");
            width_ = width;
            height_ = height;
        }
        Require(SetWindowPos(window, nullptr, bounds.left - Border, bounds.top - Border, width, height,
                             SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER) != FALSE, "position fixture ring geometry");
    }
    HWND window{};
private:
    int width_{}, height_{};
    COLORREF color_{};
    ComPtr<ID2D1DeviceContext> drawing_;
    ComPtr<IDCompositionDevice> composition_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDCompositionSurface> surface_;
};

struct Fixture {
    HWND receiver{};
    HWND counterText{};
    HANDLE job{};
    unsigned receiverClicks{}, receiverWheels{}, receiverKeys{};
    POINT receiverLastClick{};
    bool reconciliationPosted{};
    std::array<std::unique_ptr<Peer>, 2> peers;
    std::array<Ring, 2> rings;
    ComPtr<ID3D11Device> graphics;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> drawing;
    ComPtr<IDCompositionDevice> composition;
    std::ofstream evidence;

    ~Fixture() {
        for (auto& ring : rings) if (ring.window) ShowWindow(ring.window, SW_HIDE);
        for (auto& peer : peers) peer.reset();
        if (job) CloseHandle(job);
        if (receiver && IsWindow(receiver)) DestroyWindow(receiver);
    }

    void CreateOverlap() {
        RECT bounds{};
        Require(GetWindowRect(receiver, &bounds) != FALSE, "query our fixture receiver bounds");
        for (unsigned index = 0; index < peers.size(); ++index) {
            const int left = bounds.left + (index ? 340 : 70);
            const int top = bounds.top + (index ? 320 : 230);
            const RECT targetBounds{left, top, left + 430, top + 360};
            if (peers[index] && !peers[index]->LiveTarget()) peers[index].reset();
            if (!peers[index]) {
                peers[index] = std::make_unique<Peer>(receiver, targetBounds, index, job);
                evidence << "{\"event\":\"target_created\",\"target\":" << index
                         << ",\"pid\":" << peers[index]->ProcessId()
                         << ",\"hwnd\":" << reinterpret_cast<std::uintptr_t>(peers[index]->mapping.state->target)
                         << ",\"overlay_hwnd\":" << reinterpret_cast<std::uintptr_t>(rings[index].window) << "}\n";
            } else {
                peers[index]->mapping.state->bounds = targetBounds;
                PostMessageW(peers[index]->mapping.state->target, ResetCase, 0, 0);
            }
        }
        SetWindowPos(receiver, HWND_BOTTOM, 0, 0, 0, 0, PositionFlags);
        Reorder();
        Counters();
    }

    void Request(unsigned index, UINT message, WPARAM argument = 0) {
        if (!peers[index] || !peers[index]->LiveTarget()) {
            SetWindowTextW(counterText, L"Create overlapping targets first.");
            return;
        }
        const auto& peer = *peers[index];
        if (message == FocusTarget || (message == WM_COMMAND &&
            (argument == DialogButton || argument == MenuButton))) {
            AllowSetForegroundWindow(peer.ProcessId());
        }
        PostMessageW(peer.mapping.state->target, message, argument, 0);
        SetWindowPos(receiver, HWND_BOTTOM, 0, 0, 0, 0, PositionFlags);
    }

    void Counters() {
        std::wstring text = L"Receiver: clicks " + std::to_wstring(receiverClicks) + L", wheels " +
            std::to_wstring(receiverWheels) + L", keys " + std::to_wstring(receiverKeys);
        for (unsigned index = 0; index < peers.size(); ++index) {
            if (!peers[index]) continue;
            const auto& state = *peers[index]->mapping.state;
            text += index ? L" | B: " : L" | A: ";
            if (!peers[index]->LiveTarget()) {
                text += L"closed";
                continue;
            }
            text += L"clicks " + std::to_wstring(state.clicks) + L", wheels " +
                std::to_wstring(state.wheels) + L", keys " + std::to_wstring(state.keys) +
                L", topmost " + ((GetWindowLongPtrW(state.target, GWL_EXSTYLE) & WS_EX_TOPMOST) ? L"yes" : L"no") +
                L", menu " + (state.menuCapture.status == MenuCaptureStatus::Captured ? L"pixel measured" : L"not measured");
        }
        SetWindowTextW(counterText, text.c_str());
    }

    void Reorder() {
        reconciliationPosted = false;
        for (unsigned index = 0; index < peers.size(); ++index) {
            if (!peers[index]) continue;
            const HWND target = peers[index]->mapping.state->target;
            RECT bounds{};
            if (!peers[index]->LiveTarget() || !IsWindowVisible(target) || IsIconic(target) || IsZoomed(target) ||
                !GetWindowRect(target, &bounds)) {
                ShowWindow(rings[index].window, SW_HIDE);
                continue;
            }
            rings[index].Geometry(bounds);
            const HRESULT result = PlaceOverlayBelow(rings[index].window, target);
            if (SUCCEEDED(result)) {
                SetWindowPos(rings[index].window, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            } else {
                ShowWindow(rings[index].window, SW_HIDE);
            }
            evidence << "{\"event\":\"reorder\",\"target\":" << index << ",\"hr\":" << result
                     << ",\"adjacent\":" << (GetWindow(target, GW_HWNDNEXT) == rings[index].window)
                     << ",\"target_topmost\":" << ((GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
                     << ",\"overlay_topmost\":" << ((GetWindowLongPtrW(rings[index].window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
                     << "}\n";
        }
        Counters();
        evidence.flush();
    }

    void Sample() {
        DwmFlush();
        HDC screen = GetDC(nullptr);
        evidence << "{\"event\":\"capture\",\"tick\":" << GetTickCount64()
                 << ",\"receiver_clicks\":" << receiverClicks << ",\"receiver_wheels\":" << receiverWheels
                 << ",\"receiver_keys\":" << receiverKeys << ",\"receiver_last_click\":["
                 << receiverLastClick.x << ',' << receiverLastClick.y << "]}\n";
        for (unsigned index = 0; index < peers.size(); ++index) {
            if (!peers[index]) continue;
            const auto& state = *peers[index]->mapping.state;
            if (!peers[index]->LiveTarget()) continue;
            RECT bounds{};
            GetWindowRect(state.target, &bounds);
            evidence << "{\"event\":\"input\",\"target\":" << index << ",\"clicks\":" << state.clicks
                     << ",\"wheels\":" << state.wheels << ",\"keys\":" << state.keys
                     << ",\"menu_capture_status\":" << static_cast<unsigned>(state.menuCapture.status)
                     << ",\"last_click\":[" << state.lastClick.x << ',' << state.lastClick.y << "]}\n";
            const std::array samples{
                POINT{bounds.left - Border / 2, bounds.top + 140},
                POINT{bounds.left - Border / 2, bounds.top + 200},
                POINT{bounds.right + Border / 2, bounds.top + 200},
                POINT{bounds.left + 250, bounds.bottom - 70}
            };
            for (unsigned sample = 0; sample < samples.size(); ++sample) {
                const auto point = samples[sample];
                const COLORREF actual = screen ? GetPixel(screen, point.x, point.y) : CLR_INVALID;
                const HWND hit = WindowFromPoint(point);
                evidence << "{\"event\":\"pixel\",\"target\":" << index << ",\"sample\":" << sample
                         << ",\"point\":[" << point.x << ',' << point.y << "],\"colorref\":" << actual
                         << ",\"ring_colorref\":" << RingColors[index]
                         << ",\"target_colorref\":" << TargetColors[index]
                         << ",\"dialog_colorref\":" << DialogColor
                         << ",\"hit_hwnd\":" << reinterpret_cast<std::uintptr_t>(hit)
                         << ",\"hit_root\":" << reinterpret_cast<std::uintptr_t>(GetAncestor(hit, GA_ROOT))
                         << ",\"dialog_hwnd\":" << reinterpret_cast<std::uintptr_t>(state.dialog)
                         << ",\"tooltip_hwnd\":" << reinterpret_cast<std::uintptr_t>(state.tooltip)
                         << "}\n";
            }
        }
        if (screen) ReleaseDC(nullptr, screen);
        Counters();
        evidence.flush();
    }

    bool SampleMenu(unsigned index, HWND menuWindow) {
        if (index >= peers.size() || !peers[index] || !peers[index]->LiveTarget()) return false;
        DWORD process{};
        RECT bounds{};
        std::array<wchar_t, 32> className{};
        const auto visibleOwnedMenu = [&] {
            return IsWindowVisible(menuWindow) && GetWindowThreadProcessId(menuWindow, &process) &&
                process == peers[index]->ProcessId() && GetWindowRect(menuWindow, &bounds) &&
                GetClassNameW(menuWindow, className.data(), static_cast<int>(className.size())) &&
                std::wstring_view(className.data()) == L"#32768";
        };
        if (!visibleOwnedMenu()) return false;
        Sample();
        DwmFlush();
        const bool visibleAtSample = visibleOwnedMenu();
        const POINT point{bounds.left + 8, bounds.top + (bounds.bottom - bounds.top) / 2};
        HDC screen = visibleAtSample ? GetDC(nullptr) : nullptr;
        const COLORREF color = screen ? GetPixel(screen, point.x, point.y) : CLR_INVALID;
        if (screen) ReleaseDC(nullptr, screen);
        const HWND hit = visibleAtSample ? WindowFromPoint(point) : nullptr;
        const bool visibleAfterSample = visibleOwnedMenu();
        evidence << "{\"event\":\"menu_pixel\",\"target\":" << index
                 << ",\"menu_hwnd\":" << reinterpret_cast<std::uintptr_t>(menuWindow)
                 << ",\"during_menu_loop\":true,\"visible_at_sample\":" << visibleAtSample
                 << ",\"visible_after_sample\":" << visibleAfterSample
                 << ",\"point\":[" << point.x << ',' << point.y << "],\"colorref\":" << color
                 << ",\"pixel_valid\":" << (color != CLR_INVALID)
                 << ",\"hit_hwnd\":" << reinterpret_cast<std::uintptr_t>(hit)
                 << ",\"ring_colorref\":" << RingColors[index] << "}\n";
        evidence.flush();
        return visibleAtSample && visibleAfterSample && color != CLR_INVALID;
    }
};

Fixture* Current{};

LRESULT CALLBACK ReceiverProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!Current) return DefWindowProcW(window, message, wparam, lparam);
    auto& fixture = *Current;
    try {
        switch (message) {
        case WM_PAINT:
            Paint(window, ReceiverColor,
                  L"Create overlapping targets, then use the named buttons or target titles to switch focus.\n"
                  L"Click opaque ring over this receiver, then scroll/type. Sample pixels records actual pixels and input counters.\n"
                  L"Only explicit Focus buttons request foreground for our fixture targets. No injected input or existing-app focus.\n"
                  L"Measurements are not an automatic pass; this tests stacking and overlay styles, not knitted rendering.", 140);
            return 0;
        case WM_COMMAND:
            fixture.evidence << "{\"event\":\"button\",\"id\":" << LOWORD(wparam)
                             << ",\"tick\":" << GetTickCount64() << "}\n";
            fixture.evidence.flush();
            switch (LOWORD(wparam)) {
            case CreateOverlapButton: fixture.CreateOverlap(); break;
            case FocusAButton: fixture.Request(0, FocusTarget); break;
            case FocusBButton: fixture.Request(1, FocusTarget); break;
            case ModalAButton: fixture.Request(0, WM_COMMAND, DialogButton); break;
            case TopmostAButton: fixture.Request(0, WM_COMMAND, TopmostButton); break;
            case TopmostBButton: fixture.Request(1, WM_COMMAND, TopmostButton); break;
            case MenuAButton: fixture.Request(0, WM_COMMAND, MenuButton); break;
            case TooltipAButton: fixture.Request(0, WM_COMMAND, TooltipButton); break;
            case SamplePixelsButton:
                SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, PositionFlags);
                fixture.Sample();
                break;
            case CountersButton: fixture.Counters(); break;
            case CloseFixtureButton: DestroyWindow(window); break;
            }
            return 0;
        case WM_LBUTTONDOWN:
            ++fixture.receiverClicks;
            fixture.receiverLastClick = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ClientToScreen(window, &fixture.receiverLastClick);
            fixture.Sample();
            break;
        case WM_MOUSEWHEEL:
            ++fixture.receiverWheels;
            fixture.Sample();
            break;
        case WM_KEYDOWN:
            ++fixture.receiverKeys;
            fixture.Sample();
            break;
        case Changed:
            if (!fixture.reconciliationPosted) {
                fixture.reconciliationPosted = true;
                PostMessageW(window, Reconcile, 0, 0);
            }
            return 0;
        case Reconcile:
            fixture.Reorder();
            return 0;
        case Capture:
            fixture.Sample();
            return 0;
        case CaptureMenu:
            return fixture.SampleMenu(static_cast<unsigned>(wparam), reinterpret_cast<HWND>(lparam)) ? 1 : 0;
        case MenuCaptureEnded:
            if (wparam < fixture.peers.size() && fixture.peers[wparam]) {
                const auto status = fixture.peers[wparam]->mapping.state->menuCapture.status;
                fixture.evidence << "{\"event\":\"menu_capture_result\",\"target\":" << wparam
                                 << ",\"status\":\"" << (status == MenuCaptureStatus::Captured
                                     ? "measured_visible_pixel" : "not_captured") << "\"}\n";
                fixture.evidence.flush();
                fixture.Counters();
            }
            return 0;
        case WM_TIMER:
            if (wparam == ExitTimer) DestroyWindow(window);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception& error) {
        fixture.evidence << "{\"event\":\"failure\",\"message\":\"" << error.what() << "\"}\n";
        fixture.evidence.flush();
        PostQuitMessage(1);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK RingProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

int RunDesktopFixture() {
    Fixture fixture;
    Current = &fixture;
    std::array<wchar_t, 32768> executable{};
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    const auto root = std::filesystem::path(executable.data()).parent_path().parent_path().parent_path().parent_path();
    Require(std::filesystem::exists(root / L"tests" / L"Stacking" / L"StackingFixture.vcxproj"),
            "fixture must run from the repository's out\\x64\\Debug or Release directory");
    const auto outputs = root / L"tests" / L"Stacking" / L"outputs";
    std::filesystem::create_directories(outputs);
    fixture.evidence.open(outputs / (L"desktop-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl"));
    Require(fixture.evidence.is_open(), "open repository-local desktop evidence");
    fixture.evidence << "{\"event\":\"started\",\"pid\":" << GetCurrentProcessId()
                     << ",\"receiver_colorref\":" << ReceiverColor << ",\"border_pixels\":" << Border << "}\n";
    fixture.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(fixture.job && SetInformationJobObject(fixture.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
            "bound visible fixture peer lifetime");
    RegisterClass(ReceiverClass, ReceiverProc);
    RegisterClass(RingClass, RingProc);
    HRESULT graphicsResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &fixture.graphics, nullptr, nullptr);
    if (FAILED(graphicsResult)) {
        graphicsResult = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &fixture.graphics, nullptr, nullptr);
    }
    Require(graphicsResult, "create fixture graphics device");
    ComPtr<IDXGIDevice> dxgi;
    Require(fixture.graphics.As(&dxgi), "query fixture DXGI");
    Require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, fixture.factory.GetAddressOf()), "create fixture D2D factory");
    Require(fixture.factory->CreateDevice(dxgi.Get(), &fixture.drawing), "create fixture D2D device");
    Require(DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(fixture.composition.GetAddressOf())), "create fixture DComp device");
    for (unsigned index = 0; index < fixture.rings.size(); ++index)
        fixture.rings[index].Create(fixture.drawing.Get(), fixture.composition.Get(), RingColors[index]);

    RECT work{};
    Require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE, "query fixture work area");
    const int left = work.left + 40, top = work.top + 40;
    fixture.receiver = CreateWindowExW(0, ReceiverClass, L"Native stacking fixture - input receiver",
        WS_OVERLAPPEDWINDOW, left, top, 1000, 740, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(fixture.receiver != nullptr, "create native click-through receiver");
    struct Button { const wchar_t* text; int id; int left; int top; int width; };
    constexpr std::array buttons{
        Button{L"Create overlapping targets", CreateOverlapButton, 8, 8, 190},
        Button{L"Focus A", FocusAButton, 206, 8, 80},
        Button{L"Focus B", FocusBButton, 294, 8, 80},
        Button{L"Modal dialog A", ModalAButton, 382, 8, 130},
        Button{L"Toggle topmost A", TopmostAButton, 520, 8, 140},
        Button{L"Toggle topmost B", TopmostBButton, 668, 8, 140},
        Button{L"Menu A", MenuAButton, 8, 48, 100},
        Button{L"Tooltip A", TooltipAButton, 116, 48, 100},
        Button{L"Sample pixels", SamplePixelsButton, 224, 48, 130},
        Button{L"Show input counters", CountersButton, 362, 48, 160},
        Button{L"Close fixture", CloseFixtureButton, 530, 48, 130}
    };
    for (const auto& button : buttons) {
        Require(CreateWindowW(L"BUTTON", button.text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            button.left, button.top, button.width, 32, fixture.receiver,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(button.id)), GetModuleHandleW(nullptr), nullptr) != nullptr,
            "create accessible fixture scenario button");
    }
    fixture.counterText = CreateWindowW(L"STATIC", L"Create overlapping targets first.",
        WS_CHILD | WS_VISIBLE, 8, 92, 965, 38, fixture.receiver, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(fixture.counterText != nullptr, "create accessible fixture input counters");
    ShowWindow(fixture.receiver, SW_SHOWNOACTIVATE);
    UpdateWindow(fixture.receiver);
    SetTimer(fixture.receiver, ExitTimer, 15 * 60 * 1000, nullptr);
    const int result = MessageLoop();
    Current = nullptr;
    return result;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    const bool peer = argc == 3 && std::wstring_view(argv[1]) == L"--peer";
    const bool desktop = argc == 1;
    const std::wstring name = peer ? argv[2] : L"";
    LocalFree(argv);
    if (!peer && !desktop) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        return peer ? RunPeer(name) : RunDesktopFixture();
    } catch (const std::exception&) {
        Current = nullptr;
        return 1;
    }
}
