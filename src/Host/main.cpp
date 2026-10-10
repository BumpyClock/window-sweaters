#include "..\Rendering\Overlay.h"
#include "..\Tracking\WindowTracker.h"
#include "OverlayState.h"
#include "RenderState.h"
#include "Appearance.h"
#include "..\Geometry\NativeGeometry.h"
#include "..\Integration\ConfigurationHost.h"
#include "..\Configuration\Pipe.h"
#include "..\Visibility\NativeVisibility.h"
#include "..\Rendering\GraphicsRecovery.h"

#include <shellapi.h>
#include <sddl.h>
#include <aclapi.h>
#include <cerrno>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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
constexpr UINT SettingsCommand = 1002;
constexpr UINT PauseCommand = 1003;
constexpr UINT ResumeCommand = 1004;
constexpr UINT SimulateGraphicsFailureCommand = 1005;
constexpr UINT_PTR ProbeTimer = 2;
constexpr UINT_PTR ConfigurationTimer = 3;
constexpr UINT_PTR RecoveryTimer = 4;
constexpr wchar_t Usage[] =
    L"WindowSweaters.Host.exe [--target <decimal-or-0x-HWND>] [--test-mode] [--probe-output <path>]\n"
    L"  No --target: track supported ordinary application windows.\n"
    L"  --target: track only an exact WindowSweaters Fixture lifetime, never an arbitrary app.\n"
    L"  --test-mode requires --target and isolated fixture storage/IPC.\n"
    L"  --storage-dir <absolute-path> --endpoint-suffix <safe-name>: test mode only; use both.\n"
    L"  Without overrides, test mode derives storage/IPC from an owned --probe-output parent.\n"
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
    window_sweaters::integration::ConfigurationHostOptions configuration;
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
        } else if (std::wcscmp(arguments[index], L"--storage-dir") == 0 && index + 1 < count) {
            valid = options.configuration.storage_directory.empty() && arguments[index + 1][0] != L'\0';
            if (valid) options.configuration.storage_directory = arguments[++index];
        } else if (std::wcscmp(arguments[index], L"--endpoint-suffix") == 0 && index + 1 < count) {
            valid = options.configuration.endpoint_suffix.empty() && arguments[index + 1][0] != L'\0';
            if (valid) options.configuration.endpoint_suffix = arguments[++index];
        } else {
            valid = false;
        }
    }
    LocalFree(arguments);
    options.configuration.test_mode = options.testMode;
    const auto& storage = options.configuration.storage_directory;
    const auto& suffix = options.configuration.endpoint_suffix;
    if (!storage.empty() || !suffix.empty()) {
        valid = valid && options.testMode && storage.is_absolute() && !suffix.empty() && suffix.size() <= 48;
        for (const auto c : suffix)
            valid = valid && ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                (c >= L'0' && c <= L'9') || c == L'-' || c == L'_');
    }
    return valid && (!options.testMode || options.target)
        && (options.probeOutput.empty() || options.target);
}

bool BindFixtureStorage(Options& options) {
    if (!options.testMode || !options.configuration.storage_directory.empty()) return true;
    if (options.probeOutput.empty()) return false;
    const auto parent = std::filesystem::weakly_canonical(
        std::filesystem::absolute(options.probeOutput).parent_path());
    if (!std::filesystem::is_directory(parent)) return false;
    Handle directory(CreateFileW(parent.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!directory.Get() || directory.Get() == INVALID_HANDLE_VALUE) return false;
    PSECURITY_DESCRIPTOR descriptor{};
    PSID owner{};
    const auto error = GetSecurityInfo(directory.Get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &descriptor);
    wchar_t* ownerSid{};
    const bool owned = error == ERROR_SUCCESS && owner && ConvertSidToStringSidW(owner, &ownerSid)
        && window_sweaters::configuration::CurrentIdentity().user_sid == ownerSid;
    LocalFree(ownerSid);
    LocalFree(descriptor);
    if (!owned) return false;
    options.probeOutput = parent / options.probeOutput.filename();
    options.configuration.storage_directory = parent / L"fixture-configuration";
    options.configuration.endpoint_suffix = window_sweaters::configuration::TestEndpointSuffix(
        options.configuration.storage_directory);
    return true;
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

class Host final {
public:
    Host(HINSTANCE instance, const Options& options,
        window_sweaters::integration::ConfigurationHost& configuration)
        : instance_(instance), options_(options), configuration_(configuration), committed_(configuration.Snapshot()),
          appearance_(window_sweaters::host::MakeAppearance(committed_)), renderRevision_(committed_.revision),
          metadataGate_(committed_.revision) {}
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
        visibility_ = std::make_unique<window_sweaters::visibility::NativeVisibility>(window_);
        const auto visibilityStart = visibility_->Start();
        if (stopped_ || !window_) return E_ABORT;
        if (FAILED(visibilityStart.notifications)) LogError(L"visibility notification registration; decoration suppressed", visibilityStart.notifications);
        if (FAILED(visibilityStart.virtualDesktop)) LogError(L"public desktop manager; decoration suppressed", visibilityStart.virtualDesktop);
        visibility_->SetPaused(committed_.paused);
        HRESULT result = graphics_.Initialize();
        if (FAILED(result)) return result;
        result = window_sweaters::rendering::RenderQueue::Create(window_, queueLimits_, queue_);
        if (SUCCEEDED(result)) result = queue_->Start(committed_.revision, recovery_.Generation());
        if (FAILED(result)) return result;
        if (!SetTimer(window_, ConfigurationTimer, 100, nullptr)) return LastError();
        metadataWorker_=std::make_unique<window_sweaters::applications::MetadataWorker>();
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
        return FinishInitialPublication();
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
        const auto queueStats = queue_ ? queue_->Status() : finalQueueStats_;
        const auto configurationStatus = configuration_.Status();
        const auto visibilityStats = visibility_ ? visibility_->Stats() : window_sweaters::visibility::NativeStats{};
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
            << "  \"configuration_revision\": " << committed_.revision << ", \"paused\": " << (committed_.paused ? "true" : "false") << ",\n"
            << "  \"settings_launch_attempts\": " << settingsLaunchAttempts_
            << ", \"settings_launch_error\": " << configurationStatus.last_launch_error << ",\n"
            << "  \"settings_launch_pid\": " << configurationStatus.last_launch_pid
            << ", \"settings_launch_exited\": " << (configurationStatus.last_launch_exited ? "true" : "false")
            << ", \"settings_launch_exit\": " << configurationStatus.last_launch_exit << ",\n"
            << "  \"border_width_dip\": " << committed_.border_width_dip << ", \"device_generation\": " << recovery_.Generation() << ",\n"
            << "  \"graphics_recovery_state\": " << static_cast<unsigned>(recovery_.State())
            << ", \"graphics_recovery_error\": " << recovery_.LastError()
            << ", \"graphics_recovery_attempts\": " << recoveryAttempts_
            << ", \"graphics_injected_failures\": " << injectedGraphicsFailures_ << ",\n"
            << "  \"design_selection\": " << static_cast<unsigned>(committed_.design_selection)
            << ", \"design_id\": \"" << committed_.design_id << "\",\n"
            << "  \"stitch_style\": " << static_cast<unsigned>(committed_.stitch_style)
            << ", \"material_style\": " << static_cast<unsigned>(committed_.material_style)
            << ", \"stitch_rows\": " << committed_.stitch_rows << ", \"yarn_argb\": " << committed_.yarn_argb << ",\n"
            << "  \"chart_id\": \"" << (appearance_.chart ? appearance_.chart->semanticId : "")
            << "\", \"by_app_plain_fallback\": "
            << (committed_.design_selection == window_sweaters::contracts::DesignSelection::by_app ? "true" : "false") << ",\n"
            << "  \"render_requests\": " << queueStats.renderRequests << ", \"render_accepted\": " << queueStats.acceptedRequests << ",\n"
            << "  \"render_preparations\": " << queueStats.renderPreparations << ", \"tile_generations\": " << queueStats.tileGenerations << ",\n"
            << "  \"tile_cache_hits\": " << queueStats.cacheHits << ", \"render_pending\": " << queueStats.pending << ",\n"
            << "  \"render_active\": " << queueStats.active << ", \"render_ready\": " << queueStats.ready << ",\n"
            << "  \"render_quota_rejected\": " << queueStats.quotaRejected << ", \"render_error\": " << queueStats.error << ",\n"
            << "  \"visibility_queries\": " << visibilityStats.queries << ", \"visibility_query_failures\": " << visibilityStats.queryFailures
            << ", \"visibility_overlay_moves\": " << visibilityStats.overlayMoves << ", \"visibility_error\": " << visibilityStats.lastQueryError << ",\n"
            << "  \"visibility_policy_revision\": " << (visibility_ ? visibility_->Snapshot().Revision() : 0)
            << ", \"visibility_policy_reason\": " << (visibility_ ? static_cast<unsigned>(visibility_->Snapshot().Reason()) : 0)
            << ", \"visibility_authorized\": " << (entry && entry->visibilityToken &&
                entry->visibility.CanPublish(VisibilityPolicy(), *entry->visibilityToken) ? "true" : "false") << ",\n"
            << "  \"gpu_uploads\": " << counters_.uploads << ", \"render_errors\": " << counters_.renderErrors << ", \"stale_results\": " << counters_.staleResults << ",\n"
            << "  \"tracker_callbacks\": " << stats.callbacks << ", \"tracker_coalesced\": " << stats.coalesced << ",\n"
            << "  \"tracker_uncertain_queries\": " << stats.uncertainQueries << ", \"tracker_error\": " << stats.lastWin32Error << ",\n"
            << "  \"border_width_pixels\": " << (entry && entry->render.prepared
                ? entry->render.prepared->frame.physicalPaddingPixels : 0) << ",\n"
            << "  \"color_rgb\": [" << ((committed_.yarn_argb >> 16) & 255) << ", "
            << ((committed_.yarn_argb >> 8) & 255) << ", " << (committed_.yarn_argb & 255)
            << "], \"render_kind\": \"native-knit\",\n"
            << "  \"composition\": \"Four narrow BGRA perimeter surfaces / DirectComposition / content-excluded / layered alpha 255 + transparent + no-redirection\"\n"
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

    static void RunConsumerContracts();

private:
    struct ContractState {
        TrackingModel model{Policy{}};
        std::unordered_map<WindowHandle, window_sweaters::geometry::WindowFacts> geometry;
        std::optional<TargetId> suppressDuringValidation;
        window_sweaters::visibility::VisibilityPolicy policy;
        std::optional<WindowHandle> offDesktop;
        bool uncertainDesktop{};
        bool recreateOverlay{};
        bool requireMetadata{};
        std::optional<window_sweaters::contracts::Configuration> pendingConfiguration;
        std::map<window_sweaters::applications::AppIdentity,std::uint64_t> eligibilityRevisions;
        std::function<void()> duringVisibilityProbe;
        unsigned failDeviceInitializations{};
        std::optional<HRESULT> nextOverlayCreateFailure;
    };
    struct Entry {
        explicit Entry(TargetId target) : state(target), visibility({target.hwnd, target.pid, target.processStart, target.generation}) {}
        OverlayState state;
        window_sweaters::visibility::TargetVisibility visibility;
        std::optional<window_sweaters::visibility::ProbeToken> visibilityToken;
        bool visibilityRegistered{};
        bool presentationVisible{};
        window_sweaters::host::RenderState render;
        Overlay overlay;
        bool placementPending{};
        window_sweaters::applications::ImmutableMetadata metadata;
        bool metadataPending{};
        std::uint64_t nextMetadataQuery{};
        std::uint64_t renderEpoch{};
        std::uint64_t eligibilityRevision{};
        bool eligibilityObserved{};
    };
    struct Counters {
        std::uint64_t creates{}, removes{}, geometryUpdates{}, draws{}, pureMoves{};
        std::uint64_t hides{}, shows{}, reorders{}, staleActions{}, placementFailures{};
        std::uint64_t uploads{}, renderErrors{}, staleResults{};
    };
    static constexpr std::uint64_t DeviceGeneration = 1;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
            host = static_cast<Host*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (host) {
            try {
                if (host->visibility_ && (message == WM_WTSSESSION_CHANGE || message == WM_POWERBROADCAST)) {
                    const auto result = host->visibility_->HandleMessage(message, wparam, lparam);
                    if (result.handled) {
                        host->ApplyVisibilityPolicy(result.policy.snapshot);
                        return message == WM_POWERBROADCAST ? TRUE : 0;
                    }
                }
                if (message == window_sweaters::rendering::RenderQueue::ReadyMessage && host->queue_ &&
                    host->queue_->OwnsNotification(wparam, lparam)) {
                    host->DrainRendered(wparam, lparam);
                    return 0;
                }
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
                    if (host->TrayCommand(LOWORD(wparam))) return 0;
                    break;
                case TrayMessage:
                    if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_SELECT ||
                        LOWORD(lparam) == NIN_KEYSELECT) host->ShowTrayMenu();
                    return 0;
                case WM_TIMER:
                    if (wparam == RecoveryTimer) {
                        host->RecoveryTick(window_sweaters::rendering::GraphicsRecovery::Clock::now());
                        return 0;
                    }
                    if (wparam == ConfigurationTimer) {
                        host->PollConfiguration();
                        host->PollMetadata();
                        host->CheckQueue();
                        host->PollVisibility();
                        return 0;
                    }
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

    bool TrayCommand(UINT command) {
        if (command == QuitCommand) { Shutdown(); return true; }
        if (command == SimulateGraphicsFailureCommand && options_.testMode) {
            ++injectedGraphicsFailures_;
            OutputDebugStringW(L"Window Sweaters: fixture injected GPU failure; no driver reset was performed.\n");
            GpuFailure(L"injected fixture graphics failure", DXGI_ERROR_DEVICE_RESET, recovery_.Generation());
            return true;
        }
        if (command == SettingsCommand) {
            ++settingsLaunchAttempts_;
            const auto error = configuration_.LaunchSettings();
            if (error) LogError(L"trusted Settings companion launch", HRESULT_FROM_WIN32(error));
            else OutputDebugStringW(L"Window Sweaters: trusted Settings process launch accepted; UI readiness is not yet established.\n");
            probeDirty_ = true;
            return true;
        }
        if (command == PauseCommand || command == ResumeCommand) {
            const auto submission = configuration_.SubmitLocal(command == PauseCommand,
                committed_.border_width_dip, committed_.revision);
            if (submission.state != window_sweaters::integration::LocalSubmissionState::queued)
                LogError(L"tray mutation rejected; committed appearance unchanged", HRESULT_FROM_WIN32(ERROR_BUSY));
            return true;
        }
        return false;
    }

    void ShowTrayMenu() {
        const HMENU menu = CreatePopupMenu();
        if (!menu) return;
        if (!AppendMenuW(menu, MF_STRING, SettingsCommand, L"Settings") ||
            !AppendMenuW(menu, MF_STRING, committed_.paused ? ResumeCommand : PauseCommand,
                committed_.paused ? L"Resume" : L"Pause") ||
            !AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) ||
            !AppendMenuW(menu, MF_STRING, QuitCommand, L"Quit")) { DestroyMenu(menu); return; }
        POINT position{};
        GetCursorPos(&position);
        // The tray gesture authorizes activation of the menu host, never the decoration.
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            position.x, position.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        TrayCommand(command);
    }

    void PollConfiguration() {
        const auto launch = configuration_.Status();
        if (launch.last_launch_pid != observedSettingsPid_ || launch.last_launch_exited != observedSettingsExited_) {
            observedSettingsPid_ = launch.last_launch_pid;
            observedSettingsExited_ = launch.last_launch_exited;
            probeDirty_ = true;
            if (launch.last_launch_exited && launch.last_launch_exit != 0)
                LogError(L"trusted Settings process exited with nonzero status",
                    static_cast<HRESULT>(launch.last_launch_exit));
        }
        if (launch.pending_local) {
            const auto drain = configuration_.DrainLocalCompletions();
            for (const auto& completion : drain.completions)
                if (completion.result.status != window_sweaters::contracts::Status::saved)
                    LogError(L"tray save rejected; committed appearance unchanged",
                        HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        }
        const auto snapshot = configuration_.PollSnapshot(committed_.revision);
        if (!snapshot) return;
        ApplyConfiguration(*snapshot);
    }

    void ApplyConfiguration(const window_sweaters::contracts::Configuration& snapshot) {
        auto appearanceOnly=snapshot;
        appearanceOnly.revision=committed_.revision;
        appearanceOnly.eligibility=committed_.eligibility;
        if (appearanceOnly==committed_) {
            const auto previous=committed_.eligibility;
            committed_=snapshot;
            metadataGate_.SetConfigurationRevision(committed_.revision);
            for (auto& [hwnd,entry]:entries_) {
                (void)hwnd;
                entry->metadataPending=false;
                if (!entry->metadata) continue;
                auto known=std::get_if<window_sweaters::applications::AppMetadata>(entry->metadata.get());
                if (!known) continue;
                const bool allowedChanged=previous.Allows(*entry->metadata)!=committed_.eligibility.Allows(*entry->metadata);
                if (contract_ && allowedChanged)
                    contract_->eligibilityRevisions.insert_or_assign(known->Identity(),snapshot.revision);
                auto revision=EligibilityRevision(known->Identity());
                const bool epochChanged=entry->eligibilityObserved && entry->eligibilityRevision!=revision;
                entry->eligibilityRevision=revision; entry->eligibilityObserved=true;
                if (!allowedChanged && !epochChanged) continue;
                AdvanceRenderEpoch(*entry);
                if (!committed_.eligibility.Allows(*entry->metadata)) Hide(*entry);
                else entry->placementPending=entry->state.requestedVisible;
            }
            PublishApplications(); probeDirty_=true; return;
        }
        auto appearance = window_sweaters::host::MakeAppearance(snapshot);
        renderRetries_.clear();
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            Hide(*entry);
            entry->render.Invalidate();
            AdvanceRenderEpoch(*entry);
            entry->placementPending = false;
        }
        committed_ = snapshot;
        renderRevision_=snapshot.revision;
        metadataGate_.SetConfigurationRevision(committed_.revision);
        for (auto& [hwnd,entry]:entries_) { (void)hwnd; entry->metadataPending=false; }
        appearance_ = std::move(appearance);
        if (visibility_) ApplyVisibilityPolicy(visibility_->SetPaused(committed_.paused).snapshot);
        else if (contract_) ApplyVisibilityPolicy(contract_->policy.SetPaused(committed_.paused).snapshot);
        const HRESULT result = queue_->Invalidate(renderRevision_, recovery_.Generation());
        if (FAILED(result)) { QueueFailure(L"configuration render invalidation", result); return; }
        if (tracker_) tracker_->InvalidateAppearance();
        PublishApplications();
        probeDirty_ = true;
    }

    void QueueFailure(const wchar_t* operation, HRESULT error) noexcept {
        LogError(operation, error);
        ++counters_.renderErrors;
        renderRetries_.clear();
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            Hide(*entry);
            entry->render.Invalidate();
            entry->placementPending = false;
        }
        probeDirty_ = true;
    }

    void CheckQueue() noexcept {
        if (!queue_ || queueFaulted_) return;
        const auto status = queue_->Status();
        if (status.state == window_sweaters::rendering::RenderQueueState::Faulted) {
            queueFaulted_ = true;
            QueueFailure(L"render worker terminal failure; decoration hidden", status.error);
        }
    }

    HRESULT GpuFailure(const wchar_t* operation, HRESULT error, std::uint64_t generation) {
        using window_sweaters::rendering::GraphicsRecovery;
        if (!GraphicsRecovery::IsRecoverable(error)) return error;
        if (recovery_.State() != window_sweaters::rendering::RecoveryState::Ready ||
            generation != recovery_.Generation()) return S_FALSE;
        LogError(operation, error);
        ApplyRecovery(recovery_.BeginFault(error, generation, GraphicsRecovery::Clock::now()));
        return S_FALSE;
    }

    HRESULT FinishInitialPublication() {
        // Initial tracker batches can encounter a GPU fault and arm recovery.
        // The revoked device has no composition object to wait on.
        if (!recovery_.CanRender(recovery_.Generation())) return S_FALSE;
        if (!graphics_.Composition()) return E_UNEXPECTED;
        const auto generation = recovery_.Generation();
        const auto result = graphics_.Composition()->WaitForCommitCompletion();
        return FAILED(result) ? GpuFailure(L"initial composition commit", result, generation) : result;
    }

    HRESULT CreateOverlay(Entry& entry) {
        HRESULT result = entry.overlay.Create(instance_, window_, graphics_);
        if (SUCCEEDED(result) && contract_ && contract_->nextOverlayCreateFailure) {
            result = *contract_->nextOverlayCreateFailure;
            contract_->nextOverlayCreateFailure.reset();
        }
        return result;
    }

    void ReleaseOverlayDevices() {
        renderRetries_.clear();
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            Hide(*entry);
            if (visibility_ && entry->visibilityRegistered) visibility_->UnregisterOverlay(entry->overlay.Window());
            entry->visibilityRegistered = false;
            entry->visibilityToken.reset();
            entry->render.Invalidate();
            entry->state.appliedRect.reset();
            entry->placementPending = false;
            entry->overlay.Destroy();
        }
        graphics_.Destroy();
        probeDirty_ = true;
    }

    void ApplyRecovery(const window_sweaters::rendering::RecoveryUpdate& update) {
        using namespace window_sweaters::rendering;
        for (const auto action : update.Actions()) {
            if (action == RecoveryAction::HideCurrent) {
                ReleaseOverlayDevices();
                if (queue_) {
                    const auto result = queue_->Invalidate(renderRevision_, update.generation);
                    if (FAILED(result)) QueueFailure(L"graphics-generation queue retirement", result);
                }
            } else if (action == RecoveryAction::InitDevice && update.initialization) {
                ++recoveryAttempts_;
                HRESULT result = graphics_.Initialize();
                if (contract_ && contract_->failDeviceInitializations) {
                    --contract_->failDeviceInitializations;
                    result = DXGI_ERROR_DEVICE_RESET;
                }
                if (stopped_ || update.generation != recovery_.Generation()) {
                    graphics_.Destroy();
                    return;
                }
                if (FAILED(result)) {
                    graphics_.Destroy();
                    LogError(L"shared graphics recovery initialization", result);
                }
                ApplyRecovery(recovery_.CompleteInitialization(*update.initialization, result, GraphicsRecovery::Clock::now()));
                return;
            } else if (action == RecoveryAction::RebuildCurrent) {
                for (auto& [hwnd, entry] : entries_) {
                    (void)hwnd;
                    entry->placementPending = entry->state.requestedVisible && !entry->state.excluded && !committed_.paused;
                }
                // One bounded receiver pass, followed by existing tracker maintenance.
                // Each target still revalidates lifetime/config/policy/geometry.
                RetryPlacement();
                if (stopped_ || update.generation != recovery_.Generation()) return;
            } else if (action == RecoveryAction::NotifyUnavailable) {
                LogError(L"graphics unavailable after bounded recovery; borders remain hidden until explicit Host restart", update.error);
            }
        }
        if (window_) KillTimer(window_, RecoveryTimer);
        recoveryRetry_.reset();
        if (!stopped_ && update.retry) {
            recoveryRetry_ = update.retry;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                update.retry->due - GraphicsRecovery::Clock::now()).count();
            if (!SetTimer(window_, RecoveryTimer, static_cast<UINT>(std::max<std::int64_t>(1, remaining)), nullptr))
                Fail(L"shared graphics recovery timer", LastError());
        }
        probeDirty_ = true;
    }

    void RecoveryTick(window_sweaters::rendering::GraphicsRecovery::TimePoint now) {
        if (stopped_ || !recoveryRetry_) return;
        const auto ticket = *recoveryRetry_;
        if (now < ticket.due) return;
        if (window_) KillTimer(window_, RecoveryTimer);
        recoveryRetry_.reset();
        ApplyRecovery(recovery_.RetryDue(now, ticket));
    }

    Entry* Find(const TargetId& target) noexcept {
        const auto found = entries_.find(target.hwnd);
        return found != entries_.end() && found->second->state.Matches(target) ? found->second.get() : nullptr;
    }

    const TrackingModel& Model() const noexcept {
        return contract_ ? contract_->model : tracker_->Model();
    }

    window_sweaters::visibility::PolicySnapshot VisibilityPolicy() const noexcept {
        return contract_ ? contract_->policy.Snapshot() : visibility_->Snapshot();
    }

    void ApplyVisibilityPolicy(const window_sweaters::visibility::PolicySnapshot& policy) {
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            const auto transition = entry->visibility.ApplyPolicy(policy);
            if (transition.hide || !policy.MayWork()) Hide(*entry);
            entry->visibilityToken.reset();
            if (!policy.MayWork()) RetireRenderRetry(*entry);
        }
        probeDirty_ = true;
    }

    bool VisibilityAllows(TargetId target) {
        using namespace window_sweaters::visibility;
        if (stopped_ || !recovery_.CanRender(recovery_.Generation())) return false;
        auto entry = Find(target);
        if (!entry) return false;
        if (!EligibleNow(*entry)) {
            Hide(*entry);
            RetireRenderRetry(*entry);
            return false;
        }
        auto policy = VisibilityPolicy();
        const auto applied = entry->visibility.ApplyPolicy(policy);
        if (applied.hide || !policy.MayWork()) Hide(*entry);
        if (!policy.MayWork()) { RetireRenderRetry(*entry); return false; }
        const auto token = entry->visibility.BeginProbe(policy);
        const HWND overlay = entry->overlay.Window();
        window_sweaters::visibility::Observation observation;
        if (contract_) {
            if (contract_->duringVisibilityProbe) {
                auto callback = std::move(contract_->duringVisibilityProbe);
                contract_->duringVisibilityProbe = {};
                callback();
                if (stopped_ || !(entry = Find(target))) return false;
            }
            WindowSnapshot facts;
            facts.desktop.bytes[0] = 1;
            facts.onCurrentDesktop = contract_->offDesktop != target.hwnd;
            facts.visible = entry->state.requestedVisible;
            facts.overlay = contract_->recreateOverlay ? OverlayAssociation::RecreateRequired
                : OverlayAssociation::CurrentDesktopVerified;
            observation = {token, contract_->uncertainDesktop ? std::nullopt : std::optional{facts}, S_OK};
        } else {
            observation = visibility_->Observe(token, overlay);
        }
        // Public desktop COM may pump WM_CLOSE and remove this entry. Neither the
        // Entry reference nor native authority may survive that reentrancy.
        if (stopped_ || !(entry = Find(target))) return false;
        if (!EligibleNow(*entry)) { Hide(*entry); RetireRenderRetry(*entry); return false; }
        policy = VisibilityPolicy();
        auto transition = entry->visibility.Reconcile(policy, observation);
        if (!transition.accepted) return false;
        if (transition.hide || transition.reason != Suppression::None) Hide(*entry);
        if (transition.recreate) {
            Hide(*entry);
            if (entry->visibilityRegistered && visibility_) visibility_->UnregisterOverlay(overlay);
            entry->visibilityRegistered = false;
            entry->visibilityToken.reset();
            entry->render.Invalidate();
            RetireRenderRetry(*entry);
            entry->overlay.Destroy();
            const auto creation = CreateOverlay(*entry);
            if (FAILED(creation)) {
                if (window_sweaters::rendering::GraphicsRecovery::IsRecoverable(creation))
                    GpuFailure(L"GPU desktop-overlay recreation", creation, recovery_.Generation());
                else {
                    entry->overlay.Destroy();
                    LogError(L"own hidden desktop-overlay recreation", creation);
                }
                return false;
            }
            if (visibility_) {
                const auto registration = visibility_->RegisterOverlay(entry->overlay.Window());
                if (FAILED(registration)) {
                    entry->overlay.Destroy();
                    LogError(L"recreated own overlay registration", registration);
                    return false;
                }
                entry->visibilityRegistered = true;
            }
            ++counters_.creates;
            if (contract_) contract_->recreateOverlay = false;
            // Retirement above rejects old results; the next submission coalesces
            // same-target work without ending its still-current tracking lifetime.
            entry->placementPending = true;
            probeDirty_ = true;
            return false;
        }
        if (transition.associate && transition.desktop && entry->visibilityRegistered) {
            Hide(*entry);
            if (!Model().Accepts({target, entry->state.appearanceRevision}) ||
                !entry->visibility.CanAssociate(policy, token, *transition.desktop)) return false;
            const auto desktop = *transition.desktop;
            const HRESULT result = visibility_->AssociateOverlay(overlay, token, desktop);
            if (stopped_ || !(entry = Find(target))) return false;
            policy = VisibilityPolicy();
            transition = entry->visibility.CompleteAssociation(policy, token, desktop, SUCCEEDED(result));
            if (!transition.accepted) return false;
            if (FAILED(result)) LogError(L"own hidden overlay desktop association; decoration suppressed", result);
            if (transition.hide) Hide(*entry);
        }
        entry->visibilityToken = token;
        const bool allowed = entry->visibility.CanPublish(policy, token);
        if (!allowed) RetireRenderRetry(*entry);
        return allowed;
    }

    void PollVisibility() {
        if (stopped_) return;
        const auto now = GetTickCount64();
        if (visibility_) {
            const auto refresh = visibility_->RefreshUncertainSession(now);
            if (refresh.changed) ApplyVisibilityPolicy(refresh.snapshot);
            visibility_->RetryVirtualDesktop(now);
        }
        if (stopped_) return;
        if (!visibilityPoll_.Due(now, VisibilityPolicy())) return;
        const auto count = std::min(visibilityOrder_.size(), window_sweaters::visibility::PollBudget);
        for (std::size_t index = 0; index < count && !stopped_; ++index) {
            const auto target = visibilityOrder_.front();
            visibilityOrder_.pop_front();
            auto entry = Find(target);
            if (!entry) continue;
            visibilityOrder_.push_back(target);
            const bool shown = entry->visibility.Shown();
            if (VisibilityAllows(target) && !shown) {
                entry = Find(target);
                if (!entry) continue;
                const HRESULT result = PlaceAndShow(target);
                if (FAILED(result)) {
                    if (auto remaining = Find(target)) Hide(*remaining);
                    LogError(L"visibility reactivation publication", result);
                }
            }
            if (count) probeDirty_ = true;
        }
    }

    bool Validate(const Action& action) {
        const WorkToken token{action.target, action.appearanceRevision};
        if (contract_) {
            if (contract_->suppressDuringValidation == token.target) {
                contract_->suppressDuringValidation.reset();
                const auto hidden = contract_->model.Observe(UncertainWindow{token.target.hwnd},
                    Dirty::Lifecycle, GetTickCount64());
                if (FAILED(ApplyBatch(hidden))) return false;
            }

            const auto entry = Find(token.target);
            return entry && entry->state.ApplyValidated(Model(), token, Model().Accepts(token));
        }
        ActionBatch updates;
        const bool accepted = tracker_->ValidateWork(token, updates);
        const HRESULT result = ApplyBatch(updates);
        if (FAILED(result)) { Fail(L"work revalidation actions", result); return false; }
        const auto entry = Find(token.target);
        return !stopped_ && entry && entry->state.ApplyValidated(tracker_->Model(), token, accepted);
    }

    bool ConfigurationCurrent() {
        if (stopped_) return false;
        if (contract_) {
            if (contract_->pendingConfiguration) {
                auto snapshot=std::move(*contract_->pendingConfiguration);
                contract_->pendingConfiguration.reset();
                ApplyConfiguration(snapshot);
            }
            return true;
        }
        for (unsigned attempt=0;attempt<2;++attempt) {
            if (configuration_.Revision()==committed_.revision) return true;
            if (auto snapshot=configuration_.PollSnapshot(committed_.revision)) ApplyConfiguration(*snapshot);
        }
        return configuration_.Revision()==committed_.revision;
    }

    std::uint64_t EligibilityRevision(window_sweaters::applications::AppIdentity const& identity) const {
        if (!contract_) return configuration_.EligibilityRevision(identity);
        auto found=contract_->eligibilityRevisions.find(identity);
        return found==contract_->eligibilityRevisions.end()?0:found->second;
    }

    bool EligibleNow(Entry& entry) {
        if (!ConfigurationCurrent()) return false;
        if (entry.state.excluded || !entry.state.requestedVisible || committed_.paused) return false;
        if (contract_ && !contract_->requireMetadata) return true;
        auto known=entry.metadata?std::get_if<window_sweaters::applications::AppMetadata>(entry.metadata.get()):nullptr;
        if (!known) return false;
        const auto revision=EligibilityRevision(known->Identity());
        if (entry.eligibilityObserved && revision!=entry.eligibilityRevision) {
            AdvanceRenderEpoch(entry);
            Hide(entry);
            entry.placementPending=entry.state.requestedVisible;
        }
        entry.eligibilityObserved=true; entry.eligibilityRevision=revision;
        return ConfigurationCurrent() && committed_.eligibility.Allows(known->Identity());
    }

    void AdvanceRenderEpoch(Entry& entry) {
        if (entry.renderEpoch==UINT64_MAX) throw std::overflow_error("Target render epoch exhausted");
        ++entry.renderEpoch;
        entry.render.Invalidate(); RetireRenderRetry(entry);
        entry.placementPending=false;
    }

    bool Synchronize(Entry& entry,WorkToken const& token) {
        auto previous=entry.state.appearanceRevision;
        if (!entry.state.Synchronize(Model(),token)) return false;
        if (!entry.renderEpoch) entry.renderEpoch=entry.state.appearanceRevision;
        else if (previous!=entry.state.appearanceRevision) AdvanceRenderEpoch(entry);
        return true;
    }

    void PublishApplications() {
        if (contract_ || stopped_) return;
        std::vector<window_sweaters::applications::RunningApplicationWindow> windows;
        windows.reserve(entries_.size());
        for (auto const& [hwnd,entry]:entries_) {
            (void)hwnd;
            if (entry->state.excluded) continue;
            auto token=window_sweaters::applications::TargetToken::FromTarget(entry->state.target,committed_.revision);
            if (!token) throw std::runtime_error("Invalid application lifetime");
            auto metadata=entry->metadata;
            if (!metadata) metadata=std::make_shared<window_sweaters::applications::MetadataResult>(
                window_sweaters::applications::MetadataUnavailable{
                    window_sweaters::applications::UnavailableReason::QueryFailed,
                    window_sweaters::applications::QueryStage::Worker,ERROR_IO_PENDING});
            windows.push_back({*token,std::move(metadata)});
        }
        configuration_.PublishApplications(std::move(windows));
    }

    void PollMetadata() {
        using namespace window_sweaters::applications;
        if (!metadataWorker_ || stopped_) return;
        bool changed=false;
        for (auto const& completion:metadataWorker_->DrainCompletions()) {
            auto entry=Find(completion.request.Token().Target());
            auto current=entry && Model().Current(entry->state.target.hwnd)==entry->state.target?
                TargetToken::FromTarget(entry->state.target,committed_.revision):std::nullopt;
            if (!metadataGate_.Accept(completion,current)) continue;
            entry->metadataPending=false;
            auto previousMetadata=entry->metadata;
            std::size_t retained=0;
            for (auto const& [hwnd,owner]:entries_) {
                (void)hwnd;
                if (owner.get()==entry || !owner->metadata) continue;
                if (auto known=std::get_if<AppMetadata>(owner->metadata.get())) retained+=known->RetainedBytes();
            }
            auto incoming=std::get_if<AppMetadata>(completion.metadata.get());
            if (incoming && incoming->RetainedBytes()>window_sweaters::contracts::maximum_running_metadata_bytes-retained)
                entry->metadata=std::make_shared<MetadataResult const>(
                    MetadataUnavailable{UnavailableReason::ResourceLimit,QueryStage::Worker,ERROR_NOT_ENOUGH_QUOTA});
            else entry->metadata=completion.metadata;
            entry->nextMetadataQuery=GetTickCount64()+30000;
            if (previousMetadata) {
                auto previous=std::get_if<AppMetadata>(previousMetadata.get());
                auto refreshed=std::get_if<AppMetadata>(entry->metadata.get());
                if ((previous!=nullptr)!=(refreshed!=nullptr) ||
                    (previous && refreshed && previous->Identity()!=refreshed->Identity())) {
                    AdvanceRenderEpoch(*entry); Hide(*entry); entry->eligibilityObserved=false;
                }
            }
            if (auto known=std::get_if<AppMetadata>(entry->metadata.get()))
                entry->state.excluded=known->ExecutableAlias() &&
                    HelperImage(known->ExecutableAlias()->c_str(),options_.target!=nullptr);
            if (EligibleNow(*entry)) entry->placementPending=true;
            else Hide(*entry);
            changed=true;
        }
        std::size_t budget=8;
        for (auto& [hwnd,entry]:entries_) {
            (void)hwnd;
            if (!budget || (entry->metadata && GetTickCount64()<entry->nextMetadataQuery) ||
                entry->metadataPending || entry->state.excluded) continue;
            auto token=TargetToken::FromTarget(entry->state.target,committed_.revision);
            if (!token) continue;
            auto request=metadataGate_.Begin(*token);
            if (!request) continue;
            auto submitted=metadataWorker_->Submit(*request);
            if (submitted==SubmitResult::Accepted || submitted==SubmitResult::Coalesced)
                entry->metadataPending=true;
            else metadataGate_.Retire(*request);
            --budget;
        }
        if (changed) { PublishApplications(); probeDirty_=true; }
        RetryPlacement();
    }
    void Hide(Entry& entry) noexcept {
        entry.presentationVisible = false;
        if (entry.overlay.Window() && IsWindowVisible(entry.overlay.Window())) {
            entry.overlay.Hide();
            ++counters_.hides;
            probeDirty_ = true;
        }
    }

    void RetireRenderRetry(Entry& entry) {
        std::erase(renderRetries_, entry.state.target);
        entry.render.retry.reset();
        entry.render.retryQueued = false;
    }

    static RECT NativeRect(const window_sweaters::geometry::PhysicalRect& rect) noexcept {
        return {rect.left, rect.top, rect.right, rect.bottom};
    }

    std::optional<window_sweaters::geometry::RenderInput> CurrentGeometry(Entry& entry) {
        using namespace window_sweaters::geometry;
        const WindowQuery query = contract_ ? WindowQuery{contract_->geometry.at(entry.state.target.hwnd)}
            : QueryWindowGeometry(reinterpret_cast<HWND>(entry.state.target.hwnd));
        const auto geometry = BuildRenderInput(query,
            {committed_.border_width_dip, 8.0});
        const auto input = std::get_if<RenderInput>(&geometry);
        const WorkToken token{entry.state.target, entry.state.appearanceRevision};
        const auto current = Model().CurrentSnapshot(token);
        if (!input || !current || !current->visible || current->dpi != input->dpi ||
            current->rect != window_sweaters::host::TrackedRect(input->physicalTargetBounds)) {
            Hide(entry);
            entry.placementPending = entry.state.requestedVisible;
            return std::nullopt;
        }
        return *input;
    }

    bool Matches(const window_sweaters::rendering::RenderRequest& request, Entry& entry,
        const window_sweaters::geometry::RenderInput& input) const noexcept {
        return window_sweaters::host::MatchesRenderInput(request,
            {entry.state.target, entry.renderEpoch}, renderRevision_, recovery_.Generation(), input)
            && recovery_.CanRender(request.deviceGeneration);
    }

    HRESULT MovePrepared(TargetId target) {
        if (!VisibilityAllows(target)) return S_FALSE;
        auto current = Find(target);
        if (stopped_ || !current) return S_FALSE;
        const Action action{ActionKind::Move, target, current->state.pendingRect, current->state.dpi,
            current->state.appearanceRevision};
        if (!Validate(action)) return S_FALSE;
        current = Find(target);
        if (stopped_ || !current) return S_FALSE;
        const auto fresh = CurrentGeometry(*current);
        if (!EligibleNow(*current) || !fresh || !current->visibilityToken ||
            !current->visibility.CanPublish(VisibilityPolicy(), *current->visibilityToken)) {
            Hide(*current);
            return S_FALSE;
        }
        auto& entry = *current;
        const auto& input = *fresh;
        if (!entry.render.prepared || !Matches(*entry.render.prepared, entry, input)) {
            Hide(entry);
            return S_FALSE;
        }
        const auto rect = window_sweaters::host::TrackedRect(input.physicalTargetBounds);
        const RECT outer = NativeRect(input.physicalOuterBounds);
        const auto& previous = entry.overlay.Bounds();
        // Moving within a monitor is a SetWindowPos/clip update, never a GPU upload.
        if (previous.left == outer.left && previous.top == outer.top &&
            entry.render.prepared->clipRelativeToOuter ==
                window_sweaters::host::TrackedRect(input.clipRelativeToOuter)) return S_OK;
        const HRESULT result = entry.overlay.UpdatePlacement(outer, NativeRect(input.clipRelativeToOuter));
        if (FAILED(result)) { Hide(entry); return GpuFailure(L"GPU perimeter placement", result, recovery_.Generation()); }
        entry.render.prepared->clipRelativeToOuter = window_sweaters::host::TrackedRect(input.clipRelativeToOuter);
        entry.state.appliedRect = rect;
        ++counters_.geometryUpdates;
        ++counters_.pureMoves;
        probeDirty_ = true;
        return S_OK;
    }

    HRESULT Prepare(TargetId target) {
        auto current = Find(target);
        if (stopped_ || !current || !recovery_.CanRender(recovery_.Generation())) return S_FALSE;
        auto& entry = *current;
        if (!EligibleNow(entry)) { Hide(entry); return S_FALSE; }
        if (!entry.overlay.Window()) {
            const HRESULT creation = CreateOverlay(entry);
            if (FAILED(creation)) {
                if (window_sweaters::rendering::GraphicsRecovery::IsRecoverable(creation))
                    return GpuFailure(L"GPU overlay creation", creation, recovery_.Generation());
                entry.overlay.Destroy();
                return creation;
            }
            if (visibility_) {
                const HRESULT registration = visibility_->RegisterOverlay(entry.overlay.Window());
                if (FAILED(registration)) {
                    entry.overlay.Destroy();
                    LogError(L"own overlay visibility registration", registration);
                    return registration;
                }
                entry.visibilityRegistered = true;
            }
            ++counters_.creates;
        }
        if (committed_.paused || !entry.state.requestedVisible || queueFaulted_) { Hide(entry); return S_FALSE; }
        if (!VisibilityAllows(target)) return S_FALSE;
        current = Find(target);
        if (stopped_ || !current || !queue_) return S_FALSE;
        const Action action{ActionKind::Move, target, current->state.pendingRect, current->state.dpi,
            current->state.appearanceRevision};
        if (!Validate(action)) return S_FALSE;
        current = Find(target);
        if (stopped_ || !current || !queue_) return S_FALSE;
        return PrepareCurrent(*current);
    }

    HRESULT PrepareCurrent(Entry& entry) {
        if (!EligibleNow(entry) || !recovery_.CanRender(recovery_.Generation()) || !entry.visibilityToken ||
            !entry.visibility.CanPublish(VisibilityPolicy(), *entry.visibilityToken)) {
            Hide(entry);
            return S_FALSE;
        }
        const auto input = CurrentGeometry(entry);
        if (!input) return S_FALSE;
        if (entry.render.prepared && Matches(*entry.render.prepared, entry, *input))
            return S_OK;
        Hide(entry);
        if (entry.render.pending && Matches(*entry.render.pending, entry, *input)) return S_FALSE;
        if (entry.render.failed && Matches(*entry.render.failed, entry, *input)) return S_FALSE;
        if (entry.render.retry && Matches(*entry.render.retry, entry, *input)) return S_FALSE;
        const auto request = window_sweaters::host::MakeRenderRequest(
            {entry.state.target, entry.renderEpoch}, renderRevision_, recovery_.Generation(), *input, appearance_);
        const HRESULT result = queue_->Submit(request);
        if (result == S_OK) {
            entry.render.pending = request;
            entry.render.prepared.reset();
            entry.render.failed.reset();
            entry.render.retry.reset();
            entry.placementPending = false;
        } else {
            entry.render.pending.reset();
            entry.render.prepared.reset();
            // A full host-wide queue is not an invalid appearance. Keep a bounded
            // exact-input gate and retry when a consumed completion frees capacity.
            if (result == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)) {
                entry.render.retry = request;
                entry.render.failed.reset();
                if (!entry.render.retryQueued) {
                    renderRetries_.push_back(entry.state.target);
                    entry.render.retryQueued = true;
                }
            } else {
                entry.render.failed = request;
                entry.render.retry.reset();
            }
            entry.placementPending = false;
            LogError(result == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)
                ? L"render queue backpressure; decoration hidden until capacity recovery"
                : L"render submission rejected; decoration hidden", result == S_FALSE ? E_ABORT : result);
            ++counters_.renderErrors;
        }
        probeDirty_ = true;
        return S_FALSE;
    }

    void DrainRendered(WPARAM cookie, LPARAM reserved) {
        ConfigurationCurrent();
        std::vector<window_sweaters::rendering::RenderReady> results;
        const HRESULT drained = queue_->Drain(cookie, reserved, results);
        if (FAILED(drained)) { QueueFailure(L"render result drain", drained); return; }
        for (const auto& ready : results) {
            const auto& request = *ready.request;
            if (!recovery_.CanRender(request.deviceGeneration)) { ++counters_.staleResults; continue; }
            auto entry = Find(request.token.target);
            if (!entry || !entry->render.pending ||
                !window_sweaters::host::SamePreparation(*entry->render.pending, request)) {
                ++counters_.staleResults;
                continue;
            }
            const Action action{ActionKind::Show, request.token.target, request.targetBounds, 0,
                entry->state.appearanceRevision};
            if (!Validate(action)) {
                if (auto current = Find(request.token.target); current && current->render.pending &&
                    window_sweaters::host::SamePreparation(*current->render.pending, request))
                    current->render.pending.reset();
                ++counters_.staleResults;
                continue;
            }
            entry = Find(request.token.target);
            if (!entry) { ++counters_.staleResults; continue; }
            const auto input = CurrentGeometry(*entry);
            if (entry->render.pending && window_sweaters::host::SamePreparation(*entry->render.pending, request))
                entry->render.pending.reset();
            if (!EligibleNow(*entry) || !input || !Matches(request, *entry, *input)) {
                Hide(*entry);
                ++counters_.staleResults;
                continue;
            }
            if (const auto error = std::get_if<window_sweaters::rendering::RenderError>(&ready.outcome)) {
                Hide(*entry);
                LogError(L"knit preparation failed; decoration hidden", error->result);
                entry->render.failed = request;
                ++counters_.renderErrors;
                continue;
            }
            const auto perimeter = std::get<std::shared_ptr<const window_sweaters::rendering::KnitPerimeter>>(ready.outcome);
            if (!VisibilityAllows(request.token.target)) { ++counters_.staleResults; continue; }
            if (!Validate(action)) { ++counters_.staleResults; continue; }
            entry = Find(request.token.target);
            if (!entry) continue;
            const auto publication = CurrentGeometry(*entry);
            if (!EligibleNow(*entry) || !publication || !Matches(request, *entry, *publication) || !entry->visibilityToken ||
                !entry->visibility.CanPublish(VisibilityPolicy(), *entry->visibilityToken)) {
                Hide(*entry);
                ++counters_.staleResults;
                continue;
            }
            HRESULT result = entry->overlay.Upload(perimeter, NativeRect(publication->physicalOuterBounds),
                NativeRect(publication->clipRelativeToOuter), NativeRect(publication->targetRelativeToOuter),
                static_cast<float>(publication->radiusDip * publication->deviceScale),
                static_cast<float>(request.frame.tuckDip * publication->deviceScale));
            if (FAILED(result)) {
                Hide(*entry);
                LogError(L"knit perimeter GPU upload; decoration hidden", result);
                if (window_sweaters::rendering::GraphicsRecovery::IsRecoverable(result)) {
                    GpuFailure(L"GPU perimeter upload recovery", result, request.deviceGeneration);
                    continue;
                }
                entry->render.failed = request;
                ++counters_.renderErrors;
                continue;
            }
            entry->render.prepared = request;
            entry->render.prepared->clipRelativeToOuter = window_sweaters::host::TrackedRect(publication->clipRelativeToOuter);
            entry->state.appliedRect = window_sweaters::host::TrackedRect(publication->physicalTargetBounds);
            ++counters_.uploads;
            ++counters_.draws;
            ++counters_.geometryUpdates;
            // Revalidate again before making the newly attached visual visible.
            if (Validate(action)) {
                if (auto current = Find(request.token.target)) {
                    result = PlaceAndShow(request.token.target);
                    if (FAILED(result)) {
                        if (auto remaining = Find(request.token.target)) Hide(*remaining);
                        LogError(L"knit publication placement", result);
                    }
                }
            }
            probeDirty_ = true;
        }
        RetryRenders();
    }

    void RetryRenders() {
        if (!ConfigurationCurrent() || committed_.paused || queueFaulted_ ||
            !recovery_.CanRender(recovery_.Generation())) return;
        const auto status = queue_->Status();
        if (status.pending >= queueLimits_.maxPending) return;
        constexpr std::size_t retryBudget = 8;
        const auto attempts = std::min(retryBudget, renderRetries_.size());
        for (std::size_t index = 0; index < attempts && !stopped_ && !renderRetries_.empty(); ++index) {
            const auto target = renderRetries_.front();
            renderRetries_.pop_front();
            auto entry = Find(target);
            if (!entry) continue;
            entry->render.retryQueued = false;
            if (!entry->render.retry) continue;
            if (!EligibleNow(*entry)) {
                Hide(*entry);
                RetireRenderRetry(*entry);
                continue;
            }
            const Action action{ActionKind::Reorder, target, entry->state.pendingRect,
                entry->state.dpi, entry->state.appearanceRevision};
            if (!Validate(action)) {
                if (auto current = Find(target); current && current->render.retry) {
                    if (!EligibleNow(*current)) RetireRenderRetry(*current);
                    else if (!current->render.retryQueued) {
                        renderRetries_.push_back(target);
                        current->render.retryQueued = true;
                    }
                }
                continue;
            }
            entry = Find(target);
            if (!entry) continue;
            entry->render.retry.reset();
            const HRESULT result = PlaceAndShow(target);
            if (FAILED(result)) {
                if (auto remaining = Find(target)) Hide(*remaining);
                LogError(L"capacity-triggered render retry", result);
            }
            if (stopped_ || !queue_ || queue_->Status().pending >= queueLimits_.maxPending) break;
        }
    }

    HRESULT PlaceAndShow(TargetId identity) {
        auto current = Find(identity);
        if (stopped_ || !current) return S_FALSE;
        if (!EligibleNow(*current) || queueFaulted_) {
            Hide(*current);
            return S_OK;
        }
        HRESULT result = Prepare(identity);
        if (result != S_OK) return result;
        result = MovePrepared(identity);
        if (result != S_OK) return result;
        current = Find(identity);
        if (stopped_ || !current || !EligibleNow(*current) || !current->render.prepared) return S_FALSE;
        auto& entry = *current;
        if (contract_) {
            entry.presentationVisible = true;
            entry.placementPending = false;
            return S_OK;
        }
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
            if (!EligibleNow(entry) || !entry.render.prepared) { Hide(entry); return S_FALSE; }
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
        entry.presentationVisible = true;
        probeDirty_ = true;
        return S_OK;
    }

    HRESULT ApplyBatch(const ActionBatch& actions) {
        bool capacityFreed{};
        bool lifecycleChanged{};
        for (auto action : actions) {
            if (stopped_ && action.kind != ActionKind::Remove) continue;
            Entry* entry = Find(action.target);
            if (action.kind == ActionKind::Create) {
                if (Model().Current(action.target.hwnd) != action.target) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry) {
                    if (entries_.contains(action.target.hwnd)) return E_UNEXPECTED;
                    auto created = std::make_unique<Entry>(action.target);
                    entry = created.get();
                    entries_.emplace(action.target.hwnd, std::move(created));
                    visibilityOrder_.push_back(action.target);
                }
                const WorkToken token{action.target, action.appearanceRevision};
                if (!Synchronize(*entry, token)) {
                    ++counters_.staleActions;
                    continue;
                }
                // Retain a resource-free entry if preparation loses a race to suppression.
                // A later settled Show must still have a lifetime to recover.
                if (!Validate(action)) { ++counters_.staleActions; continue; }
                entry = Find(action.target);
                if (!entry) { ++counters_.staleActions; continue; }
                const HRESULT result = Prepare(action.target);
                if (FAILED(result)) return result;
                PublishApplications();
            } else if (!entry) {
                ++counters_.staleActions;
                continue;
            } else if (action.kind == ActionKind::Remove) {
                const HRESULT cancellation = queue_ && !stopped_ ? queue_->Cancel(action.target) : S_OK;
                if (FAILED(cancellation)) LogError(L"exact target render cancellation", cancellation);
                if (entry->overlay.Window()) ++counters_.removes;
                std::erase(renderRetries_, action.target);
                std::erase(visibilityOrder_, action.target);
                if (visibility_ && entry->visibilityRegistered) visibility_->UnregisterOverlay(entry->overlay.Window());
                entries_.erase(action.target.hwnd);
                metadataGate_.Forget(action.target.hwnd);
                PublishApplications();
                capacityFreed = true;
            } else if (action.kind == ActionKind::Hide) {
                lifecycleChanged=true;
                const WorkToken token{action.target, action.appearanceRevision};
                if (!Synchronize(*entry, token)) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry->state.requestedVisible) {
                    Hide(*entry);
                    RetireRenderRetry(*entry);
                }
            } else if (action.kind == ActionKind::Move) {
                const WorkToken token{action.target, action.appearanceRevision};
                if (!Synchronize(*entry, token)) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry->state.requestedVisible || committed_.paused || entry->state.NeedsDraw()) Hide(*entry);
                if (!entry->state.requestedVisible) RetireRenderRetry(*entry);
                if (action.rebuildAppearance && entry->state.requestedVisible && !committed_.paused) {
                    if (!Validate(action)) { ++counters_.staleActions; continue; }
                    entry = Find(action.target);
                    if (!entry) continue;
                    const HRESULT result = PlaceAndShow(action.target);
                    if (FAILED(result)) return result;
                } else if (entry->state.requestedVisible && !committed_.paused && entry->render.prepared) {
                    const auto input = CurrentGeometry(*entry);
                    if (input) {
                        const HRESULT result = MovePrepared(action.target);
                        if (FAILED(result)) return result;
                        if (result == S_FALSE && !stopped_) {
                            if (auto remaining = Find(action.target)) remaining->placementPending = true;
                        }
                    }
                }
            } else {
                if (action.kind==ActionKind::Show) lifecycleChanged=true;
                if (!Validate(action)) {
                    // The old work token is rejected. Retry current immutable input, not its result.
                    if (auto pending = Find(action.target)) pending->placementPending = true;
                    ++counters_.staleActions;
                    continue;
                }
                entry = Find(action.target); // Revalidation can remove or replace a lifetime.
                if (!entry) { ++counters_.staleActions; continue; }
                const HRESULT result = PlaceAndShow(action.target);
                if (FAILED(result)) return result;
            }
            probeDirty_ = true;
        }
        if (lifecycleChanged) PublishApplications();
        if (capacityFreed && !stopped_) RetryRenders();
        return S_OK;
    }

    void RetryPlacement() {
        if (stopped_ || !recovery_.CanRender(recovery_.Generation())) return;
        std::vector<Action> retry;
        for (const auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            if (entry->placementPending && entry->state.requestedVisible
                && EligibleNow(*entry)) {
                retry.push_back({ActionKind::Reorder, entry->state.target, entry->state.pendingRect,
                    entry->state.dpi, entry->state.appearanceRevision, false, entry->state.topmost});
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
        const auto recoveryStop = recovery_.Stop();
        recoveryRetry_.reset();
        renderRetries_.clear();
        visibilityOrder_.clear();
        // Stop invalidates COM authority immediately. Keep the object alive until
        // Host destruction, after any reentrant desktop operation has unwound.
        if (visibility_) visibility_->Stop();
        if (window_) { KillTimer(window_, ProbeTimer); KillTimer(window_, ConfigurationTimer); KillTimer(window_, RecoveryTimer); }
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            Hide(*entry);
            entry->render.Invalidate();
            entry->placementPending = false;
        }
        configuration_.Quit();
        if (metadataWorker_) {
            for (auto const& retirement:metadataWorker_->Stop()) metadataGate_.Retire(retirement);
            metadataWorker_.reset();
        }
        if (queue_) {
            queue_->Invalidate(renderRevision_, recoveryStop.generation);
            const HRESULT result = queue_->Stop();
            if (FAILED(result)) LogError(L"render queue shutdown", result);
            finalQueueStats_ = queue_->Status();
        }
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
        queue_.reset();
        if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE, &tray_); trayAdded_ = false; }
        if (window_) { DestroyWindow(window_); window_ = nullptr; }
    }

    HINSTANCE instance_;
    const Options& options_;
    window_sweaters::integration::ConfigurationHost& configuration_;
    window_sweaters::contracts::Configuration committed_;
    window_sweaters::knitting::TileSpec appearance_;
    // Queue revision advances only for shared appearance changes. Target epochs
    // retire app-policy work without canceling another application's preparation.
    std::uint64_t renderRevision_{};
    window_sweaters::applications::CompletionGate metadataGate_;
    std::unique_ptr<window_sweaters::applications::MetadataWorker> metadataWorker_;
    ContractState* contract_{};
    HWND window_{};
    GraphicsDevice graphics_;
    window_sweaters::rendering::GraphicsRecovery recovery_;
    std::optional<window_sweaters::rendering::RecoveryRetry> recoveryRetry_;
    std::uint64_t recoveryAttempts_{};
    std::uint64_t injectedGraphicsFailures_{};
    std::unique_ptr<window_sweaters::visibility::NativeVisibility> visibility_;
    window_sweaters::visibility::PollGate visibilityPoll_;
    std::deque<TargetId> visibilityOrder_;
    std::unique_ptr<WindowTracker> tracker_;
    std::unique_ptr<window_sweaters::rendering::RenderQueue> queue_;
    window_sweaters::rendering::RenderQueueLimits queueLimits_;
    std::deque<TargetId> renderRetries_;
    std::unordered_map<WindowHandle, std::unique_ptr<Entry>> entries_;
    TrackerStats finalStats_;
    window_sweaters::rendering::RenderQueueStatus finalQueueStats_;
    Counters counters_;
    NOTIFYICONDATAW tray_{};
    bool trayAdded_{};
    bool probeDirty_{true};
    bool stopped_{};
    bool queueFaulted_{};
    int exitCode_{};
    std::uint64_t settingsLaunchAttempts_{};
    DWORD observedSettingsPid_{};
    bool observedSettingsExited_{};
};

void Host::RunConsumerContracts() {
    class DesktopManager final : public IVirtualDesktopManager {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (!object) return E_POINTER;
            *object = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, __uuidof(IVirtualDesktopManager))) return E_NOINTERFACE;
            *object = this; AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
        ULONG STDMETHODCALLTYPE Release() override { return --references; }
        HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND, BOOL* current) override { *current = TRUE; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND, GUID* desktop) override {
            if (reenter) { auto callback = std::move(reenter); callback(); }
            *desktop = GUID{1, 0, 0, {}};
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND, REFGUID) override { ++moves; return E_UNEXPECTED; }
        ULONG references{1};
        unsigned moves{};
        std::function<void()> reenter;
    } desktopManager;
    std::size_t consumerChecks=0;
    auto require = [&](bool condition, const char* message) {
        ++consumerChecks;
        if (!condition) throw std::runtime_error(message);
    };
    ComApartment apartment;
    require(SUCCEEDED(apartment.Result()), "Consumer contract COM initialization");
    ContractState contract;
    contract.policy.SetNotificationsReady(true);
    contract.policy.SetSession(window_sweaters::visibility::SessionState::Interactive);
    contract.policy.SetPower(window_sweaters::visibility::PowerState::Awake);
    contract.policy.SetDisplay(window_sweaters::visibility::DisplayState::On);
    Options options;
    window_sweaters::integration::ConfigurationHost configuration;
    Host host(GetModuleHandleW(nullptr), options, configuration);
    host.contract_ = &contract;
    WNDCLASSW registration{};
    registration.lpfnWndProc = WindowProc;
    registration.hInstance = host.instance_;
    registration.lpszClassName = HostClass;
    require(RegisterClassW(&registration) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "Consumer contract hidden receiver class");
    host.window_ = CreateWindowExW(0, HostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
        nullptr, host.instance_, &host);
    require(host.window_ != nullptr && SUCCEEDED(host.graphics_.Initialize()),
        "Consumer contract native receiver and graphics");
    // A ready slot held until the receiver consumes it forces a real >32 pending
    // burst, without replacing or blocking the production renderer with a stub.
    host.queueLimits_.maxReady = 1;
    require(SUCCEEDED(window_sweaters::rendering::RenderQueue::Create(
        host.window_, host.queueLimits_, host.queue_)) &&
        SUCCEEDED(host.queue_->Start(host.committed_.revision, DeviceGeneration)),
        "Consumer contract production render queue");
    std::unordered_map<WindowHandle, AliveWindow> windows;
    auto add = [&](WindowHandle handle) {
        AliveWindow window;
        window.hwnd = handle;
        window.identity = {GetCurrentProcessId(), 1234, handle};
        window.className = L"OrdinaryApp";
        window.rect = {100, 100, 401, 301};
        window.dpi = 96;
        contract.geometry.emplace(handle, window_sweaters::geometry::WindowFacts{
            {100, 100, 401, 301}, {0, 0, 1920, 1080}, {0, 0, 1920, 1040},
            96, 1, false, window_sweaters::geometry::CornerPreference::Round,
            window_sweaters::geometry::CornerEligibility::StandardFrame});
        windows.emplace(handle, window);
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(window, Dirty::Lifecycle, 0))),
            "Actual consumer initial Create/Show batch");
    };
    auto pump = [&](const auto& complete) {
        const auto deadline = GetTickCount64() + 30000;
        while (!complete() && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, host.window_,
                window_sweaters::rendering::RenderQueue::ReadyMessage,
                window_sweaters::rendering::RenderQueue::ReadyMessage, PM_REMOVE))
                DispatchMessageW(&message);
        }
        require(complete(), "Actual consumer completes current appearance within deadline");
    };
    auto prepared = [&] {
        for (const auto& [handle, entry] : host.entries_) {
            (void)handle;
            if (!entry->render.prepared || entry->render.pending || entry->render.retry ||
                entry->render.failed || entry->render.prepared->configRevision != host.renderRevision_ ||
                entry->render.prepared->deviceGeneration != host.recovery_.Generation())
                return false;
        }
        return true;
    };
    add(10001);
    pump(prepared);
    const auto firstUploads = host.counters_.uploads;
    auto change = [&](bool paused, float width) {
        auto snapshot = host.committed_;
        ++snapshot.revision;
        snapshot.paused = paused;
        snapshot.border_width_dip = width;
        host.ApplyConfiguration(snapshot);
        for (const auto& [handle, window] : windows) {
            (void)handle;
            const auto actions = contract.model.Observe(window, Dirty::Appearance, GetTickCount64());
            require(actions.size() == 1 && actions[0].kind == ActionKind::Move &&
                actions[0].rebuildAppearance, "Stationary shown tracker emits only rebuilding Move");
            require(SUCCEEDED(host.ApplyBatch(actions)), "Actual consumer rebuilding Move");
        }
    };
    change(false, 24.0f);
    pump(prepared);
    require(host.counters_.uploads == firstUploads + 1 &&
        host.entries_.at(10001)->render.prepared->frame.bandDip == 24.0f &&
        host.entries_.at(10001)->overlay.Bounds().right - host.entries_.at(10001)->overlay.Bounds().left == 349,
        "Stationary width change republishes actual four-piece perimeter at new width");
    const auto beforePause = host.counters_.uploads;
    change(true, 24.0f);
    require(!host.entries_.at(10001)->render.pending && !host.entries_.at(10001)->render.prepared &&
        host.counters_.uploads == beforePause, "Pause hides and invalidates without preparing yarn");
    change(false, 24.0f);
    pump(prepared);
    require(host.counters_.uploads == beforePause + 1, "Stationary Resume republishes from rebuilding Move");

    const auto beforeMove = host.queue_->Status();
    auto& moved = windows.at(10001);
    moved.rect = {120, 130, 421, 331};
    contract.geometry.at(10001).physicalTargetBounds = {120, 130, 421, 331};
    require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(moved, Dirty::Geometry, GetTickCount64()))),
        "Actual consumer pure-position Move");
    const auto afterMove = host.queue_->Status();
    require(beforeMove.renderRequests == afterMove.renderRequests &&
        beforeMove.renderPreparations == afterMove.renderPreparations &&
        beforeMove.tileGenerations == afterMove.tileGenerations &&
        host.counters_.uploads == beforePause + 1, "Pure movement never regenerates or republishes yarn");

    for (WindowHandle handle = 10002; handle <= 10065; ++handle) add(handle);
    require(host.queue_->Status().quotaRejected > 0, "Visible 64-target burst exceeds real pending32 quota");
    const auto hiddenTarget = host.renderRetries_.back();
    windows.at(hiddenTarget.hwnd).minimized = true;
    const auto hiddenActions = contract.model.Observe(windows.at(hiddenTarget.hwnd), Dirty::Lifecycle, GetTickCount64());
    require(SUCCEEDED(host.ApplyBatch(hiddenActions)), "Quota-deferred target temporarily suppressed");
    const auto hiddenEntry = host.Find(hiddenTarget);
    require(hiddenEntry && !hiddenEntry->render.retry && !hiddenEntry->render.retryQueued,
        "Suppression retires capacity gate, so restore does not depend on another completion");
    auto othersPrepared = [&] {
        for (const auto& [handle, entry] : host.entries_) {
            if (handle == hiddenTarget.hwnd) continue;
            if (!entry->render.prepared || entry->render.pending || entry->render.retry) return false;
        }
        return true;
    };
    pump(othersPrepared);
    require(host.queue_->Status().pending == 0 && host.queue_->Status().active == 0 &&
        host.queue_->Status().ready == 0, "Deferred target minimized until queue has no future ready notifications");
    windows.at(hiddenTarget.hwnd).minimized = false;
    const auto restored = contract.model.Observe(windows.at(hiddenTarget.hwnd), Dirty::Lifecycle, GetTickCount64());
    require(SUCCEEDED(host.ApplyBatch(restored)), "Quota-deferred target restore after other capacity notifications drain");
    pump(prepared);
    require(host.entries_.size() == 65 && host.counters_.uploads == beforePause + 65,
        "Every valid current target rejected by burst capacity eventually publishes");
    const auto idle = host.queue_->Status();
    host.RetryRenders();
    host.RetryRenders();
    require(host.queue_->Status().renderRequests == idle.renderRequests,
        "Consumed retry gates leave no static polling or regeneration");

    // Exercise the last retry losing visibility inside Validate, when there
    // will be no future ready message to revisit its former FIFO entry.
    auto& last = *host.entries_.at(10001);
    last.render.retry = last.render.prepared;
    last.render.prepared.reset();
    last.render.retryQueued = true;
    host.renderRetries_.push_back(last.state.target);
    contract.suppressDuringValidation = last.state.target;
    host.RetryRenders();
    require(!last.render.retry && !last.render.retryQueued && host.renderRetries_.empty(),
        "Recursive retry validation suppression does not strand a dormant exact-input gate");
    require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Lifecycle, GetTickCount64()))),
        "Restore last suppressed retry without another ready capacity signal");
    pump(prepared);

    for (const auto& [handle, window] : windows) {
        if (handle != 10001)
            require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(DestroyedWindow{window.hwnd},
                Dirty::Lifecycle, GetTickCount64()))), "Retire burst lifetimes before catalogue contracts");
    }
    auto appearanceChange = [&](window_sweaters::contracts::Configuration snapshot) {
        snapshot.revision = host.committed_.revision + 1;
        host.ApplyConfiguration(snapshot);
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(windows.at(10001),
            Dirty::Appearance, GetTickCount64()))), "Actual Host committed appearance transaction");
        pump(prepared);
    };
    using namespace window_sweaters;
    for (std::size_t index = 0; index < designs::ws_design_theme_count(); ++index) {
        const auto theme = designs::ws_design_theme_at(index);
        auto snapshot = host.committed_;
        snapshot.design_selection = contracts::DesignSelection::original;
        snapshot.design_id = theme->id;
        snapshot.yarn_argb = theme->base_argb;
        snapshot.material_style = contracts::MaterialStyle::raised;
        snapshot.stitch_rows = 4.5f;
        appearanceChange(snapshot);
        const auto& tile = host.entries_.at(10001)->render.prepared->tile;
        const auto sourceChart = designs::ws_design_chart_by_name(theme->chart_name);
        const auto sourceMaterial = (sourceChart->flags & designs::ChartSculptedYarn)
            ? knitting::Material::Raised : knitting::Material::Plain;
        require(tile.chart && tile.chart->semanticId == theme->id && tile.baseArgb == theme->base_argb &&
            tile.material == sourceMaterial && tile.gauge.rows == 4.5f,
            "Each committed semantic catalogue design reaches actual native worker publication");
    }
    for (const auto stitch : {contracts::StitchStyle::stockinette, contracts::StitchStyle::rib,
        contracts::StitchStyle::garter}) {
        for (const auto material : {contracts::MaterialStyle::plain, contracts::MaterialStyle::raised}) {
            auto snapshot = host.committed_;
            snapshot.design_selection = contracts::DesignSelection::plain;
            snapshot.design_id.clear();
            snapshot.stitch_style = stitch;
            snapshot.material_style = material;
            snapshot.yarn_argb = 0xffa25f39u;
            snapshot.stitch_rows = 3.5f;
            appearanceChange(snapshot);
            const auto& tile = host.entries_.at(10001)->render.prepared->tile;
            require(!tile.chart && tile.baseArgb == 0xffa25f39u && tile.gauge.rows == 3.5f &&
                tile.stitch == static_cast<knitting::Stitch>(stitch) &&
                tile.material == static_cast<knitting::Material>(material),
                "Committed yarn/stitch/material/row count reaches actual native publication");
        }
    }
    auto byApp = host.committed_;
    byApp.design_selection = contracts::DesignSelection::by_app;
    appearanceChange(byApp);
    require(!host.entries_.at(10001)->render.prepared->tile.chart &&
        host.entries_.at(10001)->render.prepared->tile.baseArgb == byApp.yarn_argb,
        "ByApp actual Host publication is honest saved plain-yarn fallback, not Finder");
    contract.requireMetadata=true;
    auto appMetadata=[](wchar_t const* path) {
        return std::make_shared<applications::MetadataResult const>(
            applications::ResolveMetadata(applications::NoPackagedIdentity{},applications::ExecutablePath{path}));
    };
    auto appA=appMetadata(L"C:\\PrivateFixtures\\One\\app.exe");
    auto appB=appMetadata(L"C:\\PrivateFixtures\\Two\\app.exe");
    host.entries_.at(10001)->metadata=appA;
    add(20001); add(20002);
    require(!host.entries_.at(20001)->overlay.Window() && !host.entries_.at(20002)->render.pending,
        "Pending owner metadata never allocates a visible default border");
    host.entries_.at(20001)->metadata=appA; host.entries_.at(20002)->metadata=appB;
    require(SUCCEEDED(host.PlaceAndShow(host.entries_.at(20002)->state.target)),
        "Known eligible second application submits real rendering");
    pump([&]{return host.entries_.at(20002)->render.prepared.has_value();});
    auto unrelated=*host.entries_.at(20002)->render.prepared;
    auto unrelatedOverlay=host.entries_.at(20002)->overlay.Window();
    auto unrelatedEpoch=host.entries_.at(20002)->renderEpoch;
    auto uploadsBeforeDisable=host.counters_.uploads;
    require(SUCCEEDED(host.PlaceAndShow(host.entries_.at(20001)->state.target)) &&
        host.entries_.at(20001)->render.pending.has_value(),"First application has actual in-flight production preparation");
    auto oldRequest=*host.entries_.at(20001)->render.pending;
    applications::EligibilityMutation disable;
    disable.edits.emplace_back(applications::SetApplicationPreference{std::get<applications::AppMetadata>(*appA).Identity(),false});
    auto disabled=host.committed_;
    disabled.eligibility=std::get<applications::EligibilityPreferences>(disabled.eligibility.Apply(disable));
    ++disabled.revision; host.ApplyConfiguration(disabled);
    require(!host.EligibleNow(*host.entries_.at(10001)) && !host.entries_.at(20001)->render.pending &&
        !host.entries_.at(20001)->render.prepared &&
        host.entries_.at(20001)->renderEpoch!=oldRequest.token.appearanceRevision,
        "One identity disables every matching window and retires exact pending epoch");
    require(host.entries_.at(20002)->render.prepared.has_value() &&
        window_sweaters::host::SamePreparation(*host.entries_.at(20002)->render.prepared,unrelated) &&
        host.entries_.at(20002)->overlay.Window()==unrelatedOverlay &&
        host.entries_.at(20002)->renderEpoch==unrelatedEpoch,
        "Application-only mutation retains unrelated GPU pixels, pending contracts and target epoch");
    pump([&]{auto stats=host.queue_->Status(); return !stats.pending && !stats.active && !stats.ready;});
    require(host.counters_.uploads==uploadsBeforeDisable && host.counters_.staleResults>0,
        "Late real render completion after disable cannot upload or show");
    auto off=host.committed_; ++off.revision;
    off.eligibility=std::get<applications::EligibilityPreferences>(off.eligibility.Apply({false,true,{}}));
    host.ApplyConfiguration(off);
    add(20003); host.entries_.at(20003)->metadata=appMetadata(L"C:\\PrivateFixtures\\Future\\app.exe");
    require(host.PlaceAndShow(host.entries_.at(20003)->state.target)==S_OK &&
        !host.entries_.at(20003)->overlay.Window() && !host.entries_.at(20003)->render.pending,
        "All-off denies later-new owners before overlay creation or rendering");
    auto on=host.committed_; ++on.revision;
    on.eligibility=std::get<applications::EligibilityPreferences>(on.eligibility.Apply({true,true,{}}));
    host.ApplyConfiguration(on); host.RetryPlacement(); pump(prepared);
    require(host.entries_.at(20001)->render.prepared->token.appearanceRevision!=oldRequest.token.appearanceRevision,
        "Re-enable uses a fresh per-target epoch without ending its tracking lifetime");
    add(20007); add(20008);
    host.entries_.at(20007)->metadata=appA; host.entries_.at(20008)->metadata=appB;
    require(SUCCEEDED(host.PlaceAndShow(host.entries_.at(20007)->state.target)) &&
        SUCCEEDED(host.PlaceAndShow(host.entries_.at(20008)->state.target)) &&
        host.entries_.at(20007)->render.pending && host.entries_.at(20008)->render.pending,
        "Actual owner-not-yet-polled fixture has queued work for both applications");
    const auto pendingOther=*host.entries_.at(20008)->render.pending;
    const auto preparedOther=*host.entries_.at(20002)->render.prepared;
    const auto beforeUnpolled=host.counters_.uploads;
    auto missedDisabled=host.committed_; ++missedDisabled.revision;
    missedDisabled.eligibility=std::get<applications::EligibilityPreferences>(missedDisabled.eligibility.Apply(disable));
    applications::EligibilityMutation enableA;
    enableA.edits.emplace_back(applications::SetApplicationPreference{std::get<applications::AppMetadata>(*appA).Identity(),true});
    auto missedEnabled=missedDisabled; ++missedEnabled.revision;
    missedEnabled.eligibility=std::get<applications::EligibilityPreferences>(missedEnabled.eligibility.Apply(enableA));
    contract.eligibilityRevisions.insert_or_assign(std::get<applications::AppMetadata>(*appA).Identity(),missedEnabled.revision);
    contract.pendingConfiguration=missedEnabled;
    pump([&] {
        auto status=host.queue_->Status();
        return !status.pending && !status.active && !status.ready && host.entries_.at(20008)->render.prepared;
    });
    require(host.counters_.uploads==beforeUnpolled+1 && !host.entries_.at(20007)->render.prepared &&
        !host.entries_.at(20007)->render.pending &&
        window_sweaters::host::SamePreparation(*host.entries_.at(20008)->render.prepared,pendingOther) &&
        window_sweaters::host::SamePreparation(*host.entries_.at(20002)->render.prepared,preparedOther),
        "Unpolled disable/re-enable fences old own work while preserving unrelated queued and prepared pixels");
    host.RetryPlacement(); pump(prepared);
    for (auto handle:{WindowHandle{20001},WindowHandle{20002},WindowHandle{20003},WindowHandle{20007},WindowHandle{20008}}) {
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()))),
            "Retire private eligibility fixture lifetime");
        windows.erase(handle);
    }
    {
        struct MetadataBarrier {
            std::mutex mutex;
            std::condition_variable wake;
            bool entered{},released{};
            void Block() {
                std::unique_lock lock(mutex);
                entered=true; wake.notify_all();
                if (!wake.wait_for(lock,std::chrono::seconds(10),[&]{return released;}))
                    throw std::runtime_error("Private metadata contract barrier timed out");
            }
            bool Wait() {
                std::unique_lock lock(mutex);
                return wake.wait_for(lock,std::chrono::seconds(10),[&]{return entered;});
            }
            void Release() { std::lock_guard lock(mutex); released=true; wake.notify_all(); }
            ~MetadataBarrier() { Release(); }
        } disabling,reusing;
        std::atomic<bool> firstDisable=true,firstReuse=true;
        std::atomic<unsigned> changingOwnerQueries{};
        const auto ownerThread=GetCurrentThreadId();
        std::atomic<bool> queriedOffOwner=true;
        host.metadataWorker_=std::make_unique<applications::MetadataWorker>(applications::WorkerLimits{},
            [&](applications::TargetToken const& token) {
                if (GetCurrentThreadId()==ownerThread) queriedOffOwner=false;
                if (token.Target().hwnd==20004 && firstDisable.exchange(false)) disabling.Block();
                if (token.Target().hwnd==20005 && firstReuse.exchange(false)) reusing.Block();
                if (token.Target().hwnd==20006)
                    return applications::ResolveMetadata(
                        applications::PackagedAumid{L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"},
                        applications::ExecutablePath{L"C:\\PrivateFixtures\\WindowSweaters.Settings.exe"});
                if (token.Target().hwnd==20009 && ++changingOwnerQueries>1) return *appB;
                return *appA;
            });
        struct MetadataCleanup {
            std::function<void()> stop;
            ~MetadataCleanup() { stop(); }
        } cleanup{[&] {
            disabling.Release(); reusing.Release();
            if (host.metadataWorker_) {
                for (auto const& retirement:host.metadataWorker_->Stop()) host.metadataGate_.Retire(retirement);
                host.metadataWorker_.reset();
            }
        }};
        add(20004);
        host.PollMetadata();
        require(disabling.Wait() && host.entries_.at(20004)->metadataPending &&
            !host.entries_.at(20004)->overlay.Window(),"Host metadata is queried off discovery and opens no default border while pending");
        auto allOff=host.committed_; ++allOff.revision;
        allOff.eligibility=std::get<applications::EligibilityPreferences>(allOff.eligibility.Apply({false,true,{}}));
        host.ApplyConfiguration(allOff); host.PollMetadata(); disabling.Release();
        auto waitMetadata=[&](WindowHandle handle) {
            const auto deadline=GetTickCount64()+10000;
            while (!host.entries_.at(handle)->metadata && GetTickCount64()<deadline) {
                Sleep(5); host.PollMetadata();
            }
            require(host.entries_.at(handle)->metadata!=nullptr,"Current Host metadata request settles within bounded fixture deadline");
        };
        waitMetadata(20004);
        require(!host.entries_.at(20004)->overlay.Window() && !host.entries_.at(20004)->render.pending &&
            !host.EligibleNow(*host.entries_.at(20004)),"Late pre-disable metadata cannot bypass current all-off; renewed metadata also remains disabled");
        add(20005); host.PollMetadata();
        require(reusing.Wait(),"Private metadata lookup reaches gated old HWND/process lifetime");
        const auto oldTarget=host.entries_.at(20005)->state.target;
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(DestroyedWindow{20005},Dirty::Lifecycle,GetTickCount64()))),
            "Destroy pending metadata target retires exact owner admission");
        windows.at(20005).identity.processStart=5678;
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(windows.at(20005),Dirty::Lifecycle,GetTickCount64()))),
            "Rediscover same HWND/PID with changed process birth");
        host.PollMetadata(); reusing.Release(); waitMetadata(20005);
        const auto replacementTarget=host.entries_.at(20005)->state.target;
        require(replacementTarget.processStart==5678 && replacementTarget.generation!=oldTarget.generation &&
            !host.entries_.at(20005)->overlay.Window() && !host.entries_.at(20005)->render.pending &&
            queriedOffOwner && host.metadataWorker_->Statistics().queries>=4,
            "Old metadata completion is rejected across HWND/PID reuse; only current renewed owner settles off UI thread");
        add(20006); host.PollMetadata(); waitMetadata(20006);
        require(host.entries_.at(20006)->state.excluded && !host.entries_.at(20006)->overlay.Window(),
            "Verified packaged Settings process alias remains excluded without reducing its persistent identity to a basename");
        auto ownerEnabled=host.committed_; ++ownerEnabled.revision;
        ownerEnabled.eligibility=std::get<applications::EligibilityPreferences>(ownerEnabled.eligibility.Apply(enableA));
        host.ApplyConfiguration(ownerEnabled);
        add(20009); host.PollMetadata(); waitMetadata(20009);
        pump([&]{return host.entries_.at(20009)->render.prepared.has_value();});
        const auto ownerEpoch=host.entries_.at(20009)->renderEpoch;
        host.entries_.at(20009)->nextMetadataQuery=0;
        host.PollMetadata();
        const auto ownerDeadline=GetTickCount64()+10000;
        while (changingOwnerQueries<2 && GetTickCount64()<ownerDeadline) { Sleep(5); host.PollMetadata(); }
        while (host.entries_.at(20009)->metadataPending && GetTickCount64()<ownerDeadline) { Sleep(5); host.PollMetadata(); }
        require(changingOwnerQueries>=2 && !host.entries_.at(20009)->metadataPending &&
            host.entries_.at(20009)->renderEpoch>ownerEpoch && !host.entries_.at(20009)->render.prepared &&
            !host.EligibleNow(*host.entries_.at(20009)) &&
            std::get<applications::AppMetadata>(*host.entries_.at(20009)->metadata).Identity()==std::get<applications::AppMetadata>(*appB).Identity(),
            "Bounded metadata refresh retires only the target whose same-lifetime supported owner property changes");
        for (auto const& retirement:host.metadataWorker_->Stop()) host.metadataGate_.Retire(retirement);
        host.metadataWorker_.reset();
        for (auto handle:{WindowHandle{20004},WindowHandle{20005},WindowHandle{20006},WindowHandle{20009}}) {
            require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()))),
                "Retire private metadata-worker fixture lifetime");
            windows.erase(handle);
        }
        auto allOn=host.committed_; ++allOn.revision;
        allOn.eligibility=std::get<applications::EligibilityPreferences>(allOn.eligibility.Apply({true,true,{}}));
        host.ApplyConfiguration(allOn); host.RetryPlacement(); pump(prepared);
    }
    contract.requireMetadata=false;
    const auto visibleTarget = host.entries_.at(10001)->state.target;
    const auto visibleToken = *host.entries_.at(10001)->visibilityToken;
    using namespace window_sweaters::visibility;
    contract.recreateOverlay = true;
    require(!host.VisibilityAllows(visibleTarget) && !host.entries_.at(10001)->render.prepared &&
        !host.entries_.at(10001)->visibilityToken && !IsWindowVisible(host.entries_.at(10001)->overlay.Window()),
        "Unsupported off-current own-overlay capability recreates only hidden owned resources and retires publication");
    require(host.PlaceAndShow(visibleTarget) == S_FALSE, "Recreated overlay requires fresh current-desktop probe and preparation");
    pump(prepared);
    const auto recoveredUploads = host.counters_.uploads;
    const auto visibilityRequests = host.queue_->Status().renderRequests;
    host.ApplyVisibilityPolicy(contract.policy.SetSession(SessionState::Locked).snapshot);
    require(!host.VisibilityAllows(visibleTarget) &&
        !host.entries_.at(10001)->visibility.CanPublish(contract.policy.Snapshot(), visibleToken),
        "Actual Host lock policy rejects previous publication token");
    require(SUCCEEDED(host.PlaceAndShow(visibleTarget)) &&
        host.queue_->Status().renderRequests == visibilityRequests, "Locked Host submits no new yarn work");
    host.ApplyVisibilityPolicy(contract.policy.SetSession(SessionState::Interactive).snapshot);
    contract.uncertainDesktop = true;
    require(!host.VisibilityAllows(visibleTarget), "Desktop uncertainty suppresses Host publication on unlock");
    contract.uncertainDesktop = false;
    contract.offDesktop = 10001;
    require(!host.VisibilityAllows(visibleTarget), "Off-desktop Host target cannot publish");
    contract.offDesktop.reset();
    require(host.VisibilityAllows(visibleTarget), "Current-desktop return restores current visibility authority");
    require(SUCCEEDED(host.PlaceAndShow(visibleTarget)) &&
        host.counters_.uploads == recoveredUploads, "Visibility-only recovery reuses prepared GPU pieces");
    host.ApplyVisibilityPolicy(contract.policy.SetPower(PowerState::Suspended).snapshot);
    require(!host.VisibilityAllows(visibleTarget), "Suspend suppresses Host publication");
    host.ApplyVisibilityPolicy(contract.policy.SetPower(PowerState::Awake).snapshot);
    require(!host.VisibilityAllows(visibleTarget), "Resume without fresh display evidence stays suppressed");
    host.ApplyVisibilityPolicy(contract.policy.SetDisplay(DisplayState::On).snapshot);
    require(host.VisibilityAllows(visibleTarget), "Fresh active display permits a new Host visibility probe");
    const auto idleVisibility = host.counters_.uploads;
    host.PollVisibility();
    host.PollVisibility();
    require(host.counters_.uploads == idleVisibility &&
        host.queue_->Status().renderRequests == visibilityRequests, "Stable visibility polls never redraw or regenerate yarn");
    const auto publicationTarget = host.entries_.at(10001)->state.target;
    contract.duringVisibilityProbe = [&] {
        auto& window = windows.at(10001);
        window.rect = {160, 170, 461, 371};
        contract.geometry.at(10001).physicalTargetBounds = {160, 170, 461, 371};
        host.ApplyBatch(contract.model.Observe(window, Dirty::Geometry, GetTickCount64()));
    };
    require(host.MovePrepared(publicationTarget) == S_FALSE &&
        host.entries_.at(10001)->overlay.Bounds().left == 136,
        "Pumping desktop probe moves to fresh B placement, never pre-query A");
    require(host.PlaceAndShow(publicationTarget) == S_OK, "Establish current presentation before stale polling probe");
    const auto stalePollUploads = host.counters_.uploads;
    contract.duringVisibilityProbe = [&] {
        auto& window = windows.at(10001);
        window.rect.left += 20; window.rect.right += 20;
        contract.geometry.at(10001).physicalTargetBounds.left += 20;
        contract.geometry.at(10001).physicalTargetBounds.right += 20;
        host.ApplyBatch(contract.model.Observe(window, Dirty::Geometry, GetTickCount64()));
        require(host.PlaceAndShow(publicationTarget) == S_OK, "Nested current probe completes presentation");
    };
    host.visibilityPoll_ = {};
    host.PollVisibility();
    const auto& afterOlderPoll = *host.entries_.at(10001);
    require(afterOlderPoll.presentationVisible && afterOlderPoll.visibility.Shown() &&
        afterOlderPoll.visibilityToken &&
        afterOlderPoll.visibility.CanPublish(host.VisibilityPolicy(), *afterOlderPoll.visibilityToken) &&
        host.counters_.uploads == stalePollUploads,
        "Rejected outer PollVisibility probe cannot hide nested current publication or diverge presentation state");
    host.visibilityPoll_ = {};
    host.PollVisibility();
    require(host.entries_.at(10001)->presentationVisible && host.counters_.uploads == stalePollUploads,
        "Stable polls preserve newest presentation with no regeneration or external recovery event");
    contract.duringVisibilityProbe = [&] {
        contract.duringVisibilityProbe = [&] {
            auto changed = host.committed_;
            ++changed.revision;
            changed.border_width_dip = 28;
            host.ApplyConfiguration(changed);
            host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Appearance, GetTickCount64()));
        };
    };
    require(host.PlaceAndShow(publicationTarget) == S_FALSE,
        "Pumping final desktop probe rejects old prepared configuration before Show");
    pump(prepared);
    contract.duringVisibilityProbe = [&] {
        contract.duringVisibilityProbe = [&] {
            windows.at(10001).rect.right += 10;
            contract.geometry.at(10001).physicalTargetBounds.right += 10;
            host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Geometry, GetTickCount64()));
        };
    };
    require(host.PlaceAndShow(publicationTarget) == S_FALSE,
        "Pumping final desktop probe rejects same-lifetime resize before Show");
    host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Settlement, GetTickCount64() + 300));
    pump(prepared);
    for (const auto consumer : {0, 1, 2}) {
        const auto lifetime = host.entries_.at(10001)->state.target;
        contract.duringVisibilityProbe = [&] {
            host.ApplyBatch(contract.model.Observe(DestroyedWindow{10001}, Dirty::Lifecycle, GetTickCount64()));
        };
        const auto result = consumer == 0 ? host.Prepare(lifetime)
            : consumer == 1 ? host.MovePrepared(lifetime) : host.PlaceAndShow(lifetime);
        require(result == S_FALSE && !host.Find(lifetime),
            "Actual Prepare/Move/Show consumer rejects pumped Remove without retained Entry/TargetId references");
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Lifecycle, GetTickCount64()));
        pump(prepared);
    }

    using rendering::GraphicsRecovery;
    using rendering::RecoveryState;
    const auto recoveredTarget = host.entries_.at(10001)->state.target;
    const auto obsoleteRequest = *host.entries_.at(10001)->render.prepared;
    const auto cacheBeforeRecovery = host.queue_->Status();
    require(!host.TrayCommand(SimulateGraphicsFailureCommand),
        "Production Host does not expose fixture graphics-failure command");
    require(host.GpuFailure(L"CPU WIC rejection is not a GPU fault", E_INVALIDARG,
        host.recovery_.Generation()) == E_INVALIDARG && host.recovery_.State() == RecoveryState::Ready,
        "Arbitrary CPU/WIC errors cannot start shared GPU recovery");
    require(host.queue_->Submit(obsoleteRequest) == S_OK, "Queue actual old-generation worker input before injected fault");
    host.entries_.at(10001)->render.pending = obsoleteRequest;
    options.testMode = true;
    contract.failDeviceInitializations = 2;
    require(host.TrayCommand(SimulateGraphicsFailureCommand) &&
        host.recovery_.State() == RecoveryState::Recovering &&
        host.recovery_.Generation() == obsoleteRequest.deviceGeneration + 1 &&
        !host.graphics_.Composition() && !host.entries_.at(10001)->overlay.Window() &&
        !host.entries_.at(10001)->render.pending && !host.entries_.at(10001)->render.prepared &&
        !host.entries_.at(10001)->visibilityToken,
        "Injected GPU fault retires old queue gates, registration authority, targets and shared native device");
    require(host.queue_->Submit(obsoleteRequest) == S_FALSE &&
        !host.recovery_.CanRender(obsoleteRequest.deviceGeneration),
        "Actual render queue and publication reject old device generation");
    const auto faultGeneration = host.recovery_.Generation();
    host.GpuFailure(L"coalesced second overlay fault", DXGI_ERROR_DEVICE_RESET, obsoleteRequest.deviceGeneration);
    require(host.recovery_.Generation() == faultGeneration && host.recoveryAttempts_ == 0,
        "Per-window faults coalesce into one shared recovery ticket");
    auto pausedDuringRecovery = host.committed_;
    ++pausedDuringRecovery.revision;
    pausedDuringRecovery.paused = true;
    host.ApplyConfiguration(pausedDuringRecovery);
    host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Appearance, GetTickCount64()));
    add(10066);
    host.ApplyBatch(contract.model.Observe(DestroyedWindow{10066}, Dirty::Lifecycle, GetTickCount64()));
    windows.erase(10066);
    require(host.entries_.size() == 1 && host.entries_.at(10001)->state.target == recoveredTarget,
        "GPU recovery retains tracker identity/configuration and respects destroyed lifetimes");
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        require(host.recoveryRetry_.has_value(), "Bounded shared attempt has exactly one timer ticket");
        host.RecoveryTick(host.recoveryRetry_->due);
    }
    require(host.recovery_.State() == RecoveryState::Ready && host.recoveryAttempts_ == 3 &&
        !host.recoveryRetry_ && host.graphics_.Composition() &&
        !host.entries_.at(10001)->overlay.Window(),
        "Three injected attempt outcomes recreate real D3D/DComp once eligible, while Pause prevents overlay publication");
    auto resumeRecovery = host.committed_;
    resumeRecovery.paused = false;
    appearanceChange(resumeRecovery);
    require(host.entries_.at(10001)->state.target == recoveredTarget &&
        host.entries_.at(10001)->render.prepared->deviceGeneration == faultGeneration &&
        host.queue_->Status().tileGenerations == cacheBeforeRecovery.tileGenerations &&
        host.queue_->Status().cacheHits > cacheBeforeRecovery.cacheHits,
        "Recovered current target republishes new generation using retained CPU tile cache and current saved configuration");
    contract.recreateOverlay = true;
    contract.nextOverlayCreateFailure = DXGI_ERROR_DEVICE_RESET;
    const auto beforeRecreationFault = host.recovery_.Generation();
    require(!host.VisibilityAllows(recoveredTarget) &&
        host.recovery_.State() == RecoveryState::Recovering &&
        host.recovery_.Generation() == beforeRecreationFault + 1 &&
        !host.entries_.at(10001)->overlay.Window() && !host.graphics_.Composition(),
        "Recoverable desktop-overlay recreation failure retires partial own HWND and enters shared recovery");
    require(host.FinishInitialPublication() == S_FALSE,
        "Initial publication wait skips composition after a tracker/recreation GPU fault revoked the device");
    contract.recreateOverlay = false;
    host.RecoveryTick(host.recoveryRetry_->due);
    pump(prepared);
    contract.recreateOverlay = true;
    contract.nextOverlayCreateFailure = E_INVALIDARG;
    const auto beforeInvalidCreation = host.recovery_.Generation();
    require(!host.VisibilityAllows(recoveredTarget) &&
        !host.entries_.at(10001)->overlay.Window() &&
        host.recovery_.State() == RecoveryState::Ready &&
        host.recovery_.Generation() == beforeInvalidCreation,
        "Nonrecoverable recreation failure destroys partial unregistered HWND without GPU recovery");
    contract.recreateOverlay = false;
    host.PlaceAndShow(recoveredTarget);
    pump(prepared);

    {
        ContractState startupContract;
        startupContract.policy.SetNotificationsReady(true);
        startupContract.policy.SetSession(SessionState::Interactive);
        startupContract.policy.SetPower(PowerState::Awake);
        startupContract.policy.SetDisplay(DisplayState::On);
        Host startup(host.instance_, options, configuration);
        startup.contract_ = &startupContract;
        startup.window_ = CreateWindowExW(0, HostClass, L"", WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, host.instance_, &startup);
        require(startup.window_ && SUCCEEDED(startup.graphics_.Initialize()) &&
            SUCCEEDED(rendering::RenderQueue::Create(startup.window_, {}, startup.queue_)) &&
            SUCCEEDED(startup.queue_->Start(0, DeviceGeneration)), "Create native startup fault fixture");
        auto window = windows.at(10001);
        window.hwnd = 22001;
        startupContract.geometry.emplace(window.hwnd, contract.geometry.at(10001));
        startupContract.nextOverlayCreateFailure = D2DERR_RECREATE_TARGET;
        require(SUCCEEDED(startup.ApplyBatch(startupContract.model.Observe(window, Dirty::Lifecycle, 0))) &&
            startup.recovery_.State() == RecoveryState::Recovering &&
            !startup.graphics_.Composition() && startup.FinishInitialPublication() == S_FALSE,
            "Actual startup Create action GPU fault remains recoverable instead of dereferencing null composition");
        startup.RecoveryTick(startup.recoveryRetry_->due);
        require(startup.graphics_.Composition() && startup.recovery_.State() == RecoveryState::Ready,
            "Actual startup fault follows armed native reconstruction");
    }
    for (const bool retryConsumer : {false, true}) {
        ContractState closingContract;
        closingContract.policy.SetNotificationsReady(true);
        closingContract.policy.SetSession(SessionState::Interactive);
        closingContract.policy.SetPower(PowerState::Awake);
        closingContract.policy.SetDisplay(DisplayState::On);
        Host closing(host.instance_, options, configuration);
        closing.contract_ = &closingContract;
        closing.window_ = CreateWindowExW(0, HostClass, L"", WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, host.instance_, &closing);
        require(closing.window_ && SUCCEEDED(closing.graphics_.Initialize()) &&
            SUCCEEDED(rendering::RenderQueue::Create(closing.window_, {}, closing.queue_)) &&
            SUCCEEDED(closing.queue_->Start(0, DeviceGeneration)), "Create isolated consumer shutdown fixture");
        auto window = windows.at(10001);
        window.hwnd = 20001;
        const auto actions = closingContract.model.Observe(window, Dirty::Lifecycle, 0);
        const auto lifetime = actions[0].target;
        closingContract.geometry.emplace(window.hwnd, contract.geometry.at(10001));
        auto owned = std::make_unique<Entry>(lifetime);
        owned->state.Synchronize(closingContract.model, {lifetime, actions[0].appearanceRevision});
        owned->state.appliedRect = owned->state.pendingRect;
        const auto geometry = window_sweaters::geometry::BuildRenderInput(
            closingContract.geometry.at(window.hwnd), {16, 8});
        const auto request = window_sweaters::host::MakeRenderRequest(
            {lifetime, actions[0].appearanceRevision}, 0, DeviceGeneration,
            std::get<window_sweaters::geometry::RenderInput>(geometry));
        owned->render.prepared = request;
        closing.entries_.emplace(window.hwnd, std::move(owned));
        closingContract.duringVisibilityProbe = [&] { closing.Shutdown(); };
        if (retryConsumer) {
            auto& ownedEntry = *closing.entries_.at(window.hwnd);
            ownedEntry.render.retry = request;
            ownedEntry.render.retryQueued = true;
            closing.renderRetries_.push_back(lifetime);
            closing.RetryRenders();
        } else {
            window.rect.left += 20; window.rect.right += 20;
            closingContract.geometry.at(window.hwnd).physicalTargetBounds.left += 20;
            closingContract.geometry.at(window.hwnd).physicalTargetBounds.right += 20;
            require(SUCCEEDED(closing.ApplyBatch(closingContract.model.Observe(window, Dirty::Geometry, 10))),
                "Actual Move batch consumer survives pumping shutdown");
        }
        {
            ContractState unavailableContract;
            Host unavailable(host.instance_, options, configuration);
            unavailable.contract_ = &unavailableContract;
            unavailable.window_ = CreateWindowExW(0, HostClass, L"", WS_POPUP, 0, 0, 0, 0,
                nullptr, nullptr, host.instance_, &unavailable);
            require(unavailable.window_ && SUCCEEDED(unavailable.graphics_.Initialize()), "Create actual permanent-recovery fixture device");
            unavailableContract.failDeviceInitializations = 3;
            unavailable.TrayCommand(SimulateGraphicsFailureCommand);
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                require(unavailable.recoveryRetry_.has_value(), "Permanent recovery remains bounded to three tickets");
                unavailable.RecoveryTick(unavailable.recoveryRetry_->due);
            }
            require(unavailable.recovery_.State() == RecoveryState::Unavailable &&
                !unavailable.recoveryRetry_ && !unavailable.graphics_.Composition() &&
                unavailable.recoveryAttempts_ == 3,
                "Three failed native reconstruction attempts remain unavailable with no timer or retry loop");
        }
        {
            Host stopping(host.instance_, options, configuration);
            stopping.window_ = CreateWindowExW(0, HostClass, L"", WS_POPUP, 0, 0, 0, 0,
                nullptr, nullptr, host.instance_, &stopping);
            require(stopping.window_ && SUCCEEDED(stopping.graphics_.Initialize()), "Create pending recovery stop fixture");
            stopping.TrayCommand(SimulateGraphicsFailureCommand);
            const auto staleTicket = *stopping.recoveryRetry_;
            stopping.Shutdown();
            stopping.RecoveryTick(staleTicket.due);
            require(stopping.recovery_.State() == RecoveryState::Stopped && !stopping.recoveryRetry_ &&
                stopping.recoveryAttempts_ == 0, "Stop revokes in-flight attempt/timer so late ticks cannot recreate resources");
        }
        require(closing.stopped_ && !closing.queue_ && closing.entries_.empty(),
            "Actual Move/Retry consumer never dereferences Entry or queue after pumping Shutdown");
    }
    require(SUCCEEDED(host.graphics_.Composition()->WaitForCommitCompletion()),
        "Consumer contract native publications commit");
    std::puts("PASS: actual Host stationary width/Pause/Resume rebuilding-Move flow and pending32 burst capacity recovery (hidden native receiver)");
    std::puts("PASS: actual Host app-only epochs preserve unrelated pixels; pending metadata, all-off future owners, multi-window toggle and late production completion gates");
    std::printf("PASS: quota suppression/restore including recursive last retry; %zu chart selections and six stitch/material combinations reach actual native publication\n",
        window_sweaters::designs::ws_design_theme_count());
    std::puts("PASS: actual Host visibility gate rejects lock/off-desktop/uncertain/resume-stale tokens and reuses pixels after current-desktop recovery (synthetic facts; no OS desktop/session mutation)");
    std::puts("PASS: actual pumping Prepare/Move/Show/Retry consumers reject removed lifetimes, shutdown, obsolete configuration/size and stale placement");
    std::puts("PASS: injected shared GPU recovery recreates actual D3D/DComp, rejects late generations, preserves target/config/CPU cache, and bounds failure/Stop (injection, not a driver reset)");

    // The last test runs real NativeVisibility against a fake public manager.
    // Its query pumps Shutdown, so the Host must retain the adapter object until
    // both the native operation and this outer consumer scope have unwound.
    host.visibility_ = std::make_unique<NativeVisibility>(host.window_, desktopManager);
    const auto started = host.visibility_->Start();
    require(SUCCEEDED(started.notifications) && SUCCEEDED(started.virtualDesktop),
        "Actual Host top-level receiver starts native visibility registrations");
    const HWND targetWindow = CreateWindowExW(0, HostClass, L"hidden visibility target", WS_POPUP,
        10, 10, 100, 100, nullptr, nullptr, host.instance_, nullptr);
    require(targetWindow != nullptr, "Create invisible native visibility target fixture");
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE,
        "Capture native target process lifetime");
    const window_sweaters::tracking::TargetId nativeTarget{reinterpret_cast<WindowHandle>(targetWindow), GetCurrentProcessId(),
        (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime, 9000};
    auto nativeEntry = std::make_unique<Entry>(nativeTarget);
    nativeEntry->state.requestedVisible = true;
    nativeEntry->state.appearanceRevision = 1;
    nativeEntry->renderEpoch=1;
    nativeEntry->metadata=appMetadata(L"C:\\PrivateFixtures\\Native\\app.exe");
    require(SUCCEEDED(nativeEntry->overlay.Create(host.instance_, host.window_, host.graphics_)) &&
        SUCCEEDED(host.visibility_->RegisterOverlay(nativeEntry->overlay.Window())),
        "Register only actual own hidden overlay with native owner epoch");
    nativeEntry->visibilityRegistered = true;
    host.entries_.emplace(nativeTarget.hwnd, std::move(nativeEntry));
    struct DisplayMessage { GUID id; DWORD length; DWORD value; } display{GUID_SESSION_DISPLAY_STATUS, sizeof(DWORD), 1};
    host.visibility_->HandleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, reinterpret_cast<LPARAM>(&display));
    host.contract_ = nullptr;
    host.committed_=configuration.Snapshot();
    if (host.visibility_->Snapshot().MayWork()) {
        desktopManager.reenter = [&] {
            auto* const adapter = host.visibility_.get();
            host.Shutdown();
            require(host.visibility_.get() == adapter, "Reentrant Host Shutdown retains in-flight native adapter object");
        };
        require(host.MovePrepared(nativeTarget) == S_FALSE && host.stopped_ && host.entries_.empty() &&
            desktopManager.moves == 0, "Reentrant public desktop query cannot resurrect Host or move a foreign window");
    } else {
        std::puts("SKIP: native reentrant Host query; runner session is not interactive");
    }
    DestroyWindow(targetWindow);
    std::puts("PASS: actual native Host receiver/overlay ownership and Stop object-lifetime contract; no shell desktop manager or OS state mutation");
    std::printf("PASS: %zu actual Host consumer assertions, including app eligibility and metadata-worker races\n",consumerChecks);
}
}

namespace window_sweaters::host {
void RunHostConsumerContracts() { Host::RunConsumerContracts(); }
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

        if (!BindFixtureStorage(options)) {
            LogError(L"test mode requires isolated overrides or an owned probe-output parent", E_INVALIDARG);
            return 2;
        }

        ComApartment apartment;
        HRESULT result = apartment.Result();
        if (FAILED(result)) { LogError(L"CoInitializeEx", result); return 5; }
        window_sweaters::integration::ConfigurationHost configuration(options.configuration);
        const auto status = configuration.Start();
        if (status.state == window_sweaters::integration::HostState::already_running) return 4;
        if (status.state != window_sweaters::integration::HostState::running) {
            LogError(L"configuration binding before decoration", HRESULT_FROM_WIN32(status.error));
            return status.state == window_sweaters::integration::HostState::invalid_options ? 2 : 5;
        }
        int exitCode = 0;
        {
            Host host(instance, options, configuration);
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
