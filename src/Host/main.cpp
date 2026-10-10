#include "..\Rendering\Overlay.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <sddl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {
using window_sweaters::rendering::GraphicsDevice;
using window_sweaters::rendering::Overlay;
constexpr wchar_t HostClass[] = L"WindowSweaters.Host";
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT QuitCommand = 1001;
constexpr UINT_PTR TargetCheckTimer = 1;

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
private:
    HANDLE value_;
};

struct Options {
    HWND target{};
    bool testMode{};
    bool help{};
    std::filesystem::path probeOutput;
};

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    HRESULT Result() const noexcept { return result_; }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
private:
    HRESULT result_;
};

void LogError(const wchar_t* operation, HRESULT error) noexcept {
    wchar_t text[256]{};
    swprintf_s(text, L"Window Sweaters: %s failed (0x%08lX).\n", operation, static_cast<unsigned long>(error));
    OutputDebugStringW(text);
    std::fwprintf(stderr, L"%s", text);
}

HRESULT LastError() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

bool ParseOptions(Options& options) {
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool valid = true;
    for (int index = 1; index < count && valid; ++index) {
        if (std::wcscmp(arguments[index], L"--test-mode") == 0) {
            valid = !options.testMode;
            options.testMode = true;
        } else if (std::wcscmp(arguments[index], L"--help") == 0) {
            options.help = true;
        } else if (std::wcscmp(arguments[index], L"--target") == 0 && index + 1 < count) {
            const wchar_t* text = arguments[++index];
            wchar_t* end{};
            errno = 0;
            const bool hex = text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
            const auto value = std::wcstoull(text, &end, hex ? 16 : 10);
            valid = !options.target && text[0] != L'-' && text[0] != L'+' &&
                value != 0 && errno != ERANGE && end != text && *end == L'\0' &&
                value <= std::numeric_limits<UINT_PTR>::max();
            if (valid) options.target = reinterpret_cast<HWND>(static_cast<UINT_PTR>(value));
        } else if (std::wcscmp(arguments[index], L"--probe-output") == 0 && index + 1 < count) {
            valid = options.probeOutput.empty() && arguments[index + 1][0] != L'\0';
            if (valid) options.probeOutput = arguments[++index];
        } else {
            valid = false;
        }
    }
    LocalFree(arguments);
    return valid && (options.help || options.target != nullptr);
}

bool SelectTarget(const Options& options, RECT& bounds, DWORD& process) noexcept {
    const HWND target = options.target;
    if (!IsWindow(target) || !IsWindowVisible(target) || IsIconic(target) || IsZoomed(target) ||
        GetAncestor(target, GA_ROOT) != target) return false;
    const auto style = GetWindowLongPtrW(target, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(target, GWL_EXSTYLE);
    if (!(style & WS_CAPTION) || (style & WS_CHILD) || (extended & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST))) return false;
    if (!GetWindowThreadProcessId(target, &process) || !process || process == GetCurrentProcessId()) return false;
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked) return false;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) return false;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return false;
    if (options.testMode) {
        wchar_t name[128]{};
        wchar_t title[128]{};
        if (!GetClassNameW(target, name, static_cast<int>(std::size(name))) ||
            !GetWindowTextW(target, title, static_cast<int>(std::size(title))) ||
            std::wcscmp(name, L"WindowSweaters.TestFixture") != 0 ||
            std::wcscmp(title, L"WindowSweaters Fixture") != 0) return false;
    }
    return true;
}

HRESULT MutexName(bool testMode, std::wstring& name) {
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return LastError();
    Handle token(rawToken);
    DWORD size{};
    GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    if (!size) return LastError();
    std::vector<unsigned char> storage(size);
    if (!GetTokenInformation(token.Get(), TokenUser, storage.data(), size, &size)) return LastError();
    auto user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    wchar_t* sid{};
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) return LastError();
    name = testMode ? L"Local\\WindowSweaters.Host.Test." : L"Local\\WindowSweaters.Host.";
    name += sid;
    LocalFree(sid);
    return S_OK;
}

class Host final {
public:
    Host(HINSTANCE instance, const Options& options, RECT bounds, DWORD targetProcess) noexcept
        : instance_(instance), options_(options), initialBounds_(bounds), targetProcess_(targetProcess) {}
    ~Host() { Shutdown(); }

    HRESULT Initialize() noexcept {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = HostClass;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return LastError();
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, HostClass, L"Window Sweaters",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
        if (!window_) return LastError();
        HRESULT result = graphics_.Initialize();
        if (FAILED(result)) return result;
        result = overlay_.Create(instance_, window_, graphics_);
        if (FAILED(result)) return result;
        result = overlay_.UpdateGeometry(initialBounds_);
        if (FAILED(result)) return result;
        result = overlay_.ReorderBelow(options_.target);
        if (FAILED(result)) return result;
        result = overlay_.Show();
        if (FAILED(result)) return result;
        // Recheck placement after showing; only move our own window.
        result = overlay_.ReorderBelow(options_.target);
        if (FAILED(result)) return result;

        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = TrayMessage;
        tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray_.szTip, L"Window Sweaters");
        if (!Shell_NotifyIconW(NIM_ADD, &tray_)) return E_FAIL;
        trayAdded_ = true;
        tray_.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &tray_)) return E_FAIL;
        if (!SetTimer(window_, TargetCheckTimer, 250, nullptr)) return LastError();
        return graphics_.Composition()->WaitForCommitCompletion();
    }

    bool WriteProbe() const {
        if (options_.probeOutput.empty()) return true;
        std::ofstream output(options_.probeOutput, std::ios::binary | std::ios::trunc);
        const RECT bounds = overlay_.Bounds();
        output << "{\n"
            << "  \"host_pid\": " << GetCurrentProcessId() << ",\n"
            << "  \"host_hwnd\": " << reinterpret_cast<UINT_PTR>(window_) << ",\n"
            << "  \"overlay_hwnd\": " << reinterpret_cast<UINT_PTR>(overlay_.Window()) << ",\n"
            << "  \"target_hwnd\": " << reinterpret_cast<UINT_PTR>(options_.target) << ",\n"
            << "  \"target_bounds\": [" << initialBounds_.left << ", " << initialBounds_.top << ", "
            << initialBounds_.right << ", " << initialBounds_.bottom << "],\n"
            << "  \"overlay_bounds\": [" << bounds.left << ", " << bounds.top << ", "
            << bounds.right << ", " << bounds.bottom << "],\n"
            << "  \"border_width_pixels\": " << Overlay::BorderWidthPixels << ",\n"
            << "  \"color_rgb\": [64, 190, 220],\n"
            << "  \"composition\": \"Direct2D surface / DirectComposition / layered alpha 255 + transparent + no-redirection\"\n"
            << "}\n";
        output.close();
        return output.good();
    }

    int Run() noexcept {
        MSG message{};
        int result{};
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (result < 0) {
            LogError(L"GetMessageW", LastError());
            return 8;
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
            host = static_cast<Host*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (host) {
            switch (message) {
            case WM_COMMAND:
                if (LOWORD(wparam) == QuitCommand) { host->Shutdown(); return 0; }
                break;
            case TrayMessage:
                if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT ||
                    LOWORD(lparam) == NIN_KEYSELECT) host->ShowTrayMenu();
                return 0;
            case WM_TIMER:
                if (wparam == TargetCheckTimer) { host->CheckTarget(); return 0; }
                break;
            case WM_CLOSE: host->Shutdown(); return 0;
            case WM_QUERYENDSESSION: return TRUE;
            case WM_ENDSESSION:
                if (wparam) host->Shutdown();
                return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void ShowTrayMenu() noexcept {
        const HMENU menu = CreatePopupMenu();
        if (!menu) return;
        if (!AppendMenuW(menu, MF_STRING, QuitCommand, L"Quit")) { DestroyMenu(menu); return; }
        POINT position{};
        GetCursorPos(&position);
        // The tray gesture authorizes activation of the menu host, never the decoration.
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            position.x, position.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command == QuitCommand) Shutdown();
    }

    void CheckTarget() noexcept {
        DWORD process{};
        if (!IsWindow(options_.target) || !GetWindowThreadProcessId(options_.target, &process) ||
            process != targetProcess_) {
            Shutdown();
            return;
        }
        RECT bounds{};
        DWORD cloaked{};
        if (!IsWindowVisible(options_.target) || IsIconic(options_.target) || IsZoomed(options_.target) ||
            FAILED(DwmGetWindowAttribute(options_.target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked ||
            FAILED(DwmGetWindowAttribute(options_.target, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) ||
            !EqualRect(&bounds, &initialBounds_)) {
            // This is a fixed-geometry proof, not a window tracker. Avoid leaving a stale border.
            overlay_.Hide();
            suppressed_ = true;
        } else if (!suppressed_) {
            const HRESULT result = overlay_.ReorderBelow(options_.target);
            if (FAILED(result)) { overlay_.Hide(); suppressed_ = true; LogError(L"ReorderBelow", result); }
        }
    }

    void Shutdown() noexcept {
        if (stopped_) return;
        stopped_ = true;
        if (window_) KillTimer(window_, TargetCheckTimer);
        overlay_.Destroy();
        graphics_.Destroy();
        if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE, &tray_); trayAdded_ = false; }
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
    }

    HINSTANCE instance_;
    const Options& options_;
    RECT initialBounds_;
    DWORD targetProcess_;
    HWND window_{};
    GraphicsDevice graphics_;
    Overlay overlay_;
    NOTIFYICONDATAW tray_{};
    bool trayAdded_{};
    bool suppressed_{};
    bool stopped_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        Options options;
        if (!ParseOptions(options)) {
            LogError(L"arguments: --target <decimal-or-0x-HWND> [--test-mode] [--probe-output <path>]", E_INVALIDARG);
            return 2;
        }
        if (options.help) {
            std::fwprintf(stdout, L"WindowSweaters.Host.exe --target <decimal-or-0x-HWND> [--test-mode] [--probe-output <path>]\n");
            return 0;
        }
        OSVERSIONINFOEXW minimum{sizeof(minimum)};
        minimum.dwMajorVersion = 10;
        minimum.dwBuildNumber = 22000;
        const DWORD versionFields = VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER;
        DWORDLONG conditions = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
        conditions = VerSetConditionMask(conditions, VER_MINORVERSION, VER_GREATER_EQUAL);
        conditions = VerSetConditionMask(conditions, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        if (!VerifyVersionInfoW(&minimum, versionFields, conditions)) {
            LogError(L"Windows 11 build 22000 minimum", HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION));
            return 5;
        }
        RECT bounds{};
        DWORD process{};
        if (!SelectTarget(options, bounds, process)) { LogError(L"select restored ordinary target", E_INVALIDARG); return 3; }

        std::wstring mutexName;
        HRESULT result = MutexName(options.testMode, mutexName);
        if (FAILED(result)) { LogError(L"single-instance user identity", result); return 5; }
        Handle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
        const auto mutexError = GetLastError();
        if (!mutex.Get()) { LogError(L"CreateMutexW", HRESULT_FROM_WIN32(mutexError)); return 5; }
        if (mutexError == ERROR_ALREADY_EXISTS) return 4;

        ComApartment apartment;
        result = apartment.Result();
        if (FAILED(result)) { LogError(L"CoInitializeEx", result); return 5; }
        int exitCode = 0;
        {
            Host host(instance, options, bounds, process);
            result = host.Initialize();
            if (FAILED(result)) { LogError(L"host initialization", result); exitCode = 6; }
            else if (!host.WriteProbe()) { LogError(L"--probe-output write", E_FAIL); exitCode = 7; }
            else exitCode = host.Run();
        }
        return exitCode;
    } catch (const std::exception&) {
        LogError(L"allocation or file I/O", E_FAIL);
        return 9;
    }
}
