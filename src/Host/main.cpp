#include "..\Rendering\Overlay.h"
#include "..\Tracking\WindowTracker.h"
#include "OverlayState.h"

#include <shellapi.h>
#include <sddl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using window_sweaters::rendering::GraphicsDevice;
using window_sweaters::rendering::Overlay;
using window_sweaters::host::OverlayState;
using namespace window_sweaters::tracking;
constexpr wchar_t HostClass[] = L"WindowSweaters.Host";
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT QuitCommand = 1001;
constexpr UINT_PTR ProbeTimer = 2;
constexpr wchar_t Usage[] =
    L"WindowSweaters.Host.exe [--target <decimal-or-0x-HWND>] [--test-mode] [--probe-output <path>]\n"
    L"  No --target: track supported ordinary application windows.\n"
    L"  --target: track only an exact WindowSweaters Fixture lifetime, never an arbitrary app.\n"
    L"  --test-mode requires --target and uses an isolated test single-instance mutex.\n"
    L"  --probe-output requires --target; writes fixture snapshots and action counters.\n"
    L"WindowSweaters.Host.exe --integration-contract  (scripted checks; no desktop launch)\n"
    L"WindowSweaters.Host.exe --help\n";

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
    bool integrationContract{};
    std::optional<WindowIdentity> fixtureIdentity;
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
        } else if (std::wcscmp(arguments[index], L"--integration-contract") == 0) {
            valid = count == 2;
            options.integrationContract = true;
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
    return valid && (!options.testMode || options.target)
        && (options.probeOutput.empty() || options.target);
}

std::optional<WindowIdentity> SelectFixture(HWND target) {
    const auto observation = WindowTracker::Query(target);
    Policy policy;
    policy.mode = TrackingMode::FixtureOnly;
    policy.fixtureTarget = reinterpret_cast<WindowHandle>(target);
    policy.hostPid = GetCurrentProcessId();
    const auto eligibility = Evaluate(observation, policy);
    if (eligibility != Eligibility::Eligible && eligibility != Eligibility::Suppressed) return std::nullopt;
    const auto& snapshot = std::get<AliveWindow>(observation);
    wchar_t title[128]{};
    if (!GetWindowTextW(target, title, static_cast<int>(std::size(title)))
        || !MatchesFixtureSelection(snapshot, snapshot.identity, title)) return std::nullopt;
    return snapshot.identity;
}

bool HelperImage(const wchar_t* name, bool fixtureOnly) noexcept {
    return _wcsicmp(name, L"WindowSweaters.Host.exe") == 0
        || _wcsicmp(name, L"WindowSweaters.Settings.exe") == 0
        || (!fixtureOnly && (_wcsicmp(name, L"Fixture.exe") == 0
            || _wcsicmp(name, L"TrackingTests.exe") == 0));
}

TrackerConfig MakeTrackerConfig(const Options& options) {
    TrackerConfig config;
    config.mode = options.target ? TrackingMode::FixtureOnly : TrackingMode::Production;
    config.fixtureTarget = options.target;
    config.initialFixtureIdentity = options.fixtureIdentity;
    config.excludedClasses = {HostClass, L"WindowSweaters.Overlay", L"WindowSweaters.Settings",
        L"WindowSweaters.Settings.Activation"};
    // External helper PIDs can be reused. Classify their complete lifetimes at allocation instead.
    config.excludedPids = {GetCurrentProcessId()};
    return config;
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
    Host(HINSTANCE instance, const Options& options) noexcept : instance_(instance), options_(options) {}
    ~Host() { Shutdown(); }

    HRESULT Initialize() {
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
        tracker_ = std::make_unique<WindowTracker>(window_, MakeTrackerConfig(options_));
        ActionBatch actions;
        if (!tracker_->Start(actions))
            return HRESULT_FROM_WIN32(tracker_->Stats().lastWin32Error
                ? tracker_->Stats().lastWin32Error : ERROR_GEN_FAILURE);
        result = ApplyBatch(actions);
        if (FAILED(result)) return result;
        if (stopped_) return E_FAIL;

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
        if (!options_.probeOutput.empty() && !SetTimer(window_, ProbeTimer, 250, nullptr)) return LastError();
        return graphics_.Composition()->WaitForCommitCompletion();
    }

    bool WriteProbe() {
        if (options_.probeOutput.empty()) return true;
        const auto next = std::filesystem::path(options_.probeOutput.wstring() + L".next");
        std::ofstream output(next, std::ios::binary | std::ios::trunc);
        const auto found = entries_.find(reinterpret_cast<WindowHandle>(options_.target));
        const Entry* entry = found == entries_.end() ? nullptr : found->second.get();
        const auto targetBounds = entry ? entry->state.pendingRect : PhysicalRect{};
        const RECT bounds = entry ? entry->overlay.Bounds() : RECT{};
        const auto stats = tracker_ ? tracker_->Stats() : finalStats_;
        output << "{\n"
            << "  \"host_pid\": " << GetCurrentProcessId() << ",\n"
            << "  \"host_hwnd\": " << reinterpret_cast<UINT_PTR>(window_) << ",\n"
            << "  \"overlay_hwnd\": " << reinterpret_cast<UINT_PTR>(entry ? entry->overlay.Window() : nullptr) << ",\n"
            << "  \"target_hwnd\": " << reinterpret_cast<UINT_PTR>(options_.target) << ",\n"
            << "  \"target_pid\": " << (entry ? entry->state.target.pid : 0) << ",\n"
            << "  \"target_process_start\": " << (entry ? entry->state.target.processStart : 0) << ",\n"
            << "  \"target_generation\": " << (entry ? entry->state.target.generation : 0) << ",\n"
            << "  \"target_bounds\": [" << targetBounds.left << ", " << targetBounds.top << ", "
            << targetBounds.right << ", " << targetBounds.bottom << "],\n"
            << "  \"overlay_bounds\": [" << bounds.left << ", " << bounds.top << ", "
            << bounds.right << ", " << bounds.bottom << "],\n"
            << "  \"overlay_visible\": " << (entry && entry->overlay.Window() && IsWindowVisible(entry->overlay.Window()) ? "true" : "false") << ",\n"
            << "  \"stopped\": " << (stopped_ ? "true" : "false") << ",\n"
            << "  \"tracked_entries\": " << entries_.size() << ",\n"
            << "  \"creates\": " << counters_.creates << ", \"removes\": " << counters_.removes << ",\n"
            << "  \"geometry_updates\": " << counters_.geometryUpdates << ", \"draws\": " << counters_.draws << ",\n"
            << "  \"pure_moves\": " << counters_.pureMoves << ", \"hides\": " << counters_.hides << ",\n"
            << "  \"shows\": " << counters_.shows << ", \"reorders\": " << counters_.reorders << ",\n"
            << "  \"stale_actions\": " << counters_.staleActions << ", \"placement_failures\": " << counters_.placementFailures << ",\n"
            << "  \"tracker_callbacks\": " << stats.callbacks << ", \"tracker_coalesced\": " << stats.coalesced << ",\n"
            << "  \"tracker_uncertain_queries\": " << stats.uncertainQueries << ", \"tracker_error\": " << stats.lastWin32Error << ",\n"
            << "  \"border_width_pixels\": " << Overlay::BorderWidthPixels << ",\n"
            << "  \"color_rgb\": [64, 190, 220],\n"
            << "  \"composition\": \"Direct2D surface / DirectComposition / layered alpha 255 + transparent + no-redirection\"\n"
            << "}\n";
        output.close();
        if (!output.good()) { DeleteFileW(next.c_str()); return false; }
        if (!MoveFileExW(next.c_str(), options_.probeOutput.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(next.c_str());
            return false;
        }
        probeDirty_ = false;
        return true;
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
    struct Entry {
        explicit Entry(TargetId target) : state(target) {}
        OverlayState state;
        Overlay overlay;
        bool placementPending{};
    };
    struct Counters {
        std::uint64_t creates{}, removes{}, geometryUpdates{}, draws{}, pureMoves{};
        std::uint64_t hides{}, shows{}, reorders{}, staleActions{}, placementFailures{};
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
            host = static_cast<Host*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (host) {
            try {
                if (host->tracker_ && (message == TrackerDispatchMessage
                    || (message == WM_TIMER && wparam == TrackerTimerId))) {
                    ActionBatch actions;
                    if (host->tracker_->HandleMessage(message, wparam, lparam, actions)) {
                        const HRESULT result = host->ApplyBatch(actions);
                        if (FAILED(result)) host->Fail(L"tracker action application", result);
                        else if (message == WM_TIMER) host->RetryPlacement();
                        return 0;
                    }
                }
                switch (message) {
                case WM_COMMAND:
                    if (LOWORD(wparam) == QuitCommand) { host->Shutdown(); return 0; }
                    break;
                case TrayMessage:
                    if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT ||
                        LOWORD(lparam) == NIN_KEYSELECT) host->ShowTrayMenu();
                    return 0;
                case WM_TIMER:
                    if (wparam == ProbeTimer) {
                        if (host->probeDirty_ && !host->WriteProbe()) host->Fail(L"--probe-output write", E_FAIL);
                        return 0;
                    }
                    break;
                case WM_CLOSE: host->Shutdown(); return 0;
                case WM_QUERYENDSESSION: return TRUE;
                case WM_ENDSESSION:
                    if (wparam) host->Shutdown();
                    return 0;
                case WM_DESTROY: PostQuitMessage(host->exitCode_); return 0;
                case WM_NCDESTROY:
                    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                    break;
                }
            } catch (const std::exception& error) {
                std::fprintf(stderr, "Window Sweaters: owner-thread operation failed: %s\n", error.what());
                host->Fail(L"owner-thread exception", E_FAIL);
                return 0;
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

    Entry* Find(const TargetId& target) noexcept {
        const auto found = entries_.find(target.hwnd);
        return found != entries_.end() && found->second->state.Matches(target) ? found->second.get() : nullptr;
    }

    bool Validate(const Action& action) {
        const WorkToken token{action.target, action.appearanceRevision};
        ActionBatch updates;
        const bool accepted = tracker_->ValidateWork(token, updates);
        const HRESULT result = ApplyBatch(updates);
        if (FAILED(result)) { Fail(L"work revalidation actions", result); return false; }
        const auto entry = Find(token.target);
        return !stopped_ && entry && entry->state.ApplyValidated(tracker_->Model(), token, accepted);
    }

    HRESULT ClassifyHelper(Entry& entry) {
        const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.state.target.pid));
        if (!process.Get()) return LastError();
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process.Get(), &creation, &exit, &kernel, &user)) return LastError();
        const auto processStart = (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
        if (processStart != entry.state.target.processStart) return HRESULT_FROM_WIN32(ERROR_RETRY);
        std::wstring image(32768, L'\0');
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process.Get(), 0, image.data(), &length)) return LastError();
        image.resize(length);
        const auto separator = image.find_last_of(L"\\");
        const auto name = image.c_str() + (separator == std::wstring::npos ? 0 : separator + 1);
        entry.state.excluded = HelperImage(name, options_.target != nullptr);
        return S_OK;
    }

    void Hide(Entry& entry) noexcept {
        if (entry.overlay.Window() && IsWindowVisible(entry.overlay.Window())) {
            entry.overlay.Hide();
            ++counters_.hides;
            probeDirty_ = true;
        }
    }

    HRESULT UpdateGeometry(Entry& entry, bool settled) {
        const auto rect = entry.state.GeometryUpdate(settled);
        if (!rect) return S_OK;
        const bool draws = entry.state.NeedsDraw();
        const RECT native{rect->left, rect->top, rect->right, rect->bottom};
        const HRESULT result = entry.overlay.UpdateGeometry(native);
        if (FAILED(result)) { Hide(entry); return result; }
        entry.state.appliedRect = *rect;
        ++counters_.geometryUpdates;
        if (draws) ++counters_.draws;
        else ++counters_.pureMoves;
        probeDirty_ = true;
        return S_OK;
    }

    HRESULT Prepare(Entry& entry) {
        if (entry.state.excluded || entry.state.topmost) { Hide(entry); return S_OK; }
        if (!entry.overlay.Window()) {
            const HRESULT classification = ClassifyHelper(entry);
            if (FAILED(classification)) {
                LogError(L"target helper identity query; decoration deferred", classification);
                entry.placementPending = true;
                return S_FALSE;
            }
            if (entry.state.excluded) return S_OK;
            const HRESULT creation = entry.overlay.Create(instance_, window_, graphics_);
            if (FAILED(creation)) { LogError(L"overlay creation", creation); Hide(entry); return creation; }
            ++counters_.creates;
        }
        return UpdateGeometry(entry, true);
    }

    HRESULT PlaceAndShow(Entry& entry) {
        if (entry.state.excluded || entry.state.topmost || !entry.state.requestedVisible) {
            Hide(entry);
            return S_OK;
        }
        HRESULT result = Prepare(entry);
        if (result != S_OK || entry.state.excluded) return result;
        const HWND target = reinterpret_cast<HWND>(entry.state.target.hwnd);
        result = entry.overlay.ReorderBelow(target);
        if (FAILED(result)) {
            Hide(entry);
            if (!entry.placementPending) LogError(L"ordinary-band placement; decoration hidden for retry", result);
            entry.placementPending = true;
            ++counters_.placementFailures;
            return S_FALSE;
        }
        ++counters_.reorders;
        if (!IsWindowVisible(entry.overlay.Window())) {
            result = entry.overlay.Show();
            if (FAILED(result)) { Hide(entry); return result; }
            ++counters_.shows;
            // Showing can change our position in the band. Never move the target.
            result = entry.overlay.ReorderBelow(target);
            if (FAILED(result)) {
                Hide(entry);
                entry.placementPending = true;
                ++counters_.placementFailures;
                LogError(L"post-show placement; decoration hidden for retry", result);
                return S_FALSE;
            }
            ++counters_.reorders;
        }
        entry.placementPending = false;
        probeDirty_ = true;
        return S_OK;
    }

    HRESULT ApplyBatch(const ActionBatch& actions) {
        for (auto action : actions) {
            if (stopped_ && action.kind != ActionKind::Remove) continue;
            Entry* entry = Find(action.target);
            if (action.kind == ActionKind::Create) {
                if (tracker_->Model().Current(action.target.hwnd) != action.target) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry) {
                    if (entries_.contains(action.target.hwnd)) return E_UNEXPECTED;
                    auto created = std::make_unique<Entry>(action.target);
                    entry = created.get();
                    entries_.emplace(action.target.hwnd, std::move(created));
                }
                const WorkToken token{action.target, action.appearanceRevision};
                if (!entry->state.Synchronize(tracker_->Model(), token)) {
                    ++counters_.staleActions;
                    continue;
                }
                // Retain a resource-free entry if preparation loses a race to suppression.
                // A later settled Show must still have a lifetime to recover.
                if (!Validate(action)) { ++counters_.staleActions; continue; }
                entry = Find(action.target);
                if (!entry) { ++counters_.staleActions; continue; }
                const HRESULT result = Prepare(*entry);
                if (FAILED(result)) return result;
            } else if (!entry) {
                ++counters_.staleActions;
                continue;
            } else if (action.kind == ActionKind::Remove) {
                if (entry->overlay.Window()) ++counters_.removes;
                entries_.erase(action.target.hwnd);
            } else if (action.kind == ActionKind::Hide) {
                const WorkToken token{action.target, action.appearanceRevision};
                if (!entry->state.Synchronize(tracker_->Model(), token)) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry->state.requestedVisible || entry->state.topmost) Hide(*entry);
            } else if (action.kind == ActionKind::Move) {
                const WorkToken token{action.target, action.appearanceRevision};
                if (!entry->state.Synchronize(tracker_->Model(), token)) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry->state.requestedVisible || entry->state.NeedsDraw() || entry->state.topmost) Hide(*entry);
                // UpdateGeometry draws on size changes. Do not pass live-size dimensions to it.
                const HRESULT result = entry->overlay.Window() ? UpdateGeometry(*entry, false) : S_OK;
                if (FAILED(result)) return result;
                if (!entry->overlay.Window() && entry->state.requestedVisible) entry->placementPending = true;
            } else {
                if (!Validate(action)) {
                    // The old work token is rejected. Retry current immutable input, not its result.
                    if (auto pending = Find(action.target)) pending->placementPending = true;
                    ++counters_.staleActions;
                    continue;
                }
                entry = Find(action.target); // Revalidation can remove or replace a lifetime.
                if (!entry) { ++counters_.staleActions; continue; }
                const HRESULT result = PlaceAndShow(*entry);
                if (FAILED(result)) return result;
            }
            probeDirty_ = true;
        }
        return S_OK;
    }

    void RetryPlacement() {
        std::vector<Action> retry;
        for (const auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            if (entry->placementPending && entry->state.requestedVisible
                && !entry->state.excluded && !entry->state.topmost) {
                retry.push_back({ActionKind::Reorder, entry->state.target, entry->state.pendingRect,
                    entry->state.dpi, entry->state.appearanceRevision, false, false});
                if (retry.size() == TrackerReconcileBudget) break;
            }
        }
        const HRESULT result = ApplyBatch(retry);
        if (FAILED(result)) Fail(L"deferred ordinary-band placement", result);
    }

    void Fail(const wchar_t* operation, HRESULT result) noexcept {
        LogError(operation, result);
        exitCode_ = 6;
        Shutdown();
    }

    void Shutdown() noexcept {
        if (stopped_) return;
        stopped_ = true;
        if (window_) KillTimer(window_, ProbeTimer);
        if (tracker_) {
            try {
                const auto removals = tracker_->Stop();
                const HRESULT result = ApplyBatch(removals);
                if (FAILED(result)) LogError(L"tracker stop removals", result);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "Window Sweaters: stop removals failed: %s\n", error.what());
                LogError(L"tracker stop; releasing all host-owned overlays", E_FAIL);
                exitCode_ = 6;
            }
            finalStats_ = tracker_->Stats();
            tracker_.reset();
        }
        entries_.clear();
        try {
            if (!WriteProbe()) LogError(L"shutdown --probe-output write", E_FAIL);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Window Sweaters: shutdown probe failed: %s\n", error.what());
        }
        graphics_.Destroy();
        if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE, &tray_); trayAdded_ = false; }
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
    }

    HINSTANCE instance_;
    const Options& options_;
    HWND window_{};
    GraphicsDevice graphics_;
    std::unique_ptr<WindowTracker> tracker_;
    std::unordered_map<WindowHandle, std::unique_ptr<Entry>> entries_;
    TrackerStats finalStats_;
    Counters counters_;
    NOTIFYICONDATAW tray_{};
    bool trayAdded_{};
    bool probeDirty_{true};
    bool stopped_{};
    int exitCode_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        Options options;
        if (!ParseOptions(options)) {
            LogError(L"arguments; use --help for supported production/fixture modes", E_INVALIDARG);
            return 2;
        }
        if (options.help) {
            std::fwprintf(stdout, L"%s", Usage);
            return 0;
        }
        if (options.integrationContract) return window_sweaters::host::RunHostContractTests();
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
        if (options.target) {
            options.fixtureIdentity = SelectFixture(options.target);
            if (!options.fixtureIdentity) {
                LogError(L"select exact foreign WindowSweaters Fixture target", E_INVALIDARG);
                return 3;
            }
        }

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
            Host host(instance, options);
            result = host.Initialize();
            if (FAILED(result)) { LogError(L"host initialization", result); exitCode = 6; }
            else if (!host.WriteProbe()) { LogError(L"--probe-output write", E_FAIL); exitCode = 7; }
            else exitCode = host.Run();
        }
        return exitCode;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Window Sweaters: startup failed: %s\n", error.what());
        LogError(L"allocation or file I/O", E_FAIL);
        return 9;
    }
}
