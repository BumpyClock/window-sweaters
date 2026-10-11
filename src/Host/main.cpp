#include "..\Rendering\Overlay.h"
#include "..\Tracking\WindowTracker.h"
#include "OverlayState.h"
#include "RenderState.h"
#include "Appearance.h"
#include "..\Geometry\NativeGeometry.h"
#include "..\Integration\ConfigurationHost.h"
#include "..\Configuration\Pipe.h"
#include "..\Configuration\Storage.h"
#include "..\Visibility\NativeVisibility.h"
#include "..\Rendering\GraphicsRecovery.h"
#include "..\IconYarn\WindowsIcon.h"

#include <shellapi.h>
#include <sddl.h>
#include <aclapi.h>
#include <array>
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
    L"  --test-mode requires a fixture target or explicit preview selections, with isolated storage/IPC.\n"
    L"  --storage-dir <absolute-path> --endpoint-suffix <safe-name>: test mode only; use both.\n"
    L"  Without overrides, test mode derives storage/IPC from an owned --probe-output parent.\n"
    L"  --probe-output requires a fixture/preview target; writes appearance snapshots and action counters.\n"
    L"  --inspect-preview <HWND>: read-only owner metadata and a lifetime-bound selection token.\n"
    L"  --inspect-fixture <HWND>: read-only exact FixtureOnly startup selection diagnostics.\n"
    L"  --preview-target <HWND:PID:birth:fingerprint> (up to two): isolated explicit real-app preview.\n"
    L"  Preview requires --test-mode, --storage-dir, --endpoint-suffix and --probe-output; no production profile.\n"
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

using PreviewSelection = WindowSelection;

std::optional<std::uint64_t> ParseUnsigned(std::wstring_view text) {
    if (text.empty() || text.size()>20 || text[0]<L'0' || text[0]>L'9') return std::nullopt;
    const std::wstring value(text);
    wchar_t* end{};
    errno=0;
    const bool hex=value.starts_with(L"0x") || value.starts_with(L"0X");
    for (const auto c:text)
        if (c==L' ' || c==L'\t' || c==L'\r' || c==L'\n') return std::nullopt;
    const auto number=std::wcstoull(value.c_str(),&end,hex?16:10);
    return errno!=ERANGE && end!=value.c_str() && *end==L'\0' ?
        std::optional<std::uint64_t>(number) : std::nullopt;
}

std::optional<PreviewSelection> ParsePreviewSelection(std::wstring_view text) {
    std::array<std::uint64_t,4> parts{};
    for (std::size_t i=0;i<parts.size();++i) {
        const auto end=text.find(L':');
        if ((i+1<parts.size())!=(end!=std::wstring_view::npos)) return std::nullopt;
        const auto value=ParseUnsigned(text.substr(0,end));
        if (!value || !*value) return std::nullopt;
        parts[i]=*value;
        if (end!=std::wstring_view::npos) text.remove_prefix(end+1);
    }
    if (parts[0]>std::numeric_limits<UINT_PTR>::max() || parts[1]>UINT32_MAX) return std::nullopt;
    return PreviewSelection{static_cast<WindowHandle>(parts[0]),
        {static_cast<std::uint32_t>(parts[1]),parts[2],parts[3]}};
}

struct Options {
    HWND target{};
    bool testMode{};
    bool help{};
    bool integrationContract{};
    WindowHandle inspectPreview{};
    WindowHandle inspectFixture{};
    std::vector<PreviewSelection> preview;
    std::optional<WindowIdentity> fixtureIdentity;
    std::filesystem::path probeOutput;
    window_sweaters::integration::ConfigurationHostOptions configuration;
};

class ComApartment final {
public:
    explicit ComApartment(DWORD mode=COINIT_APARTMENTTHREADED) noexcept : result_(CoInitializeEx(nullptr, mode)) {}
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
        } else if (std::wcscmp(arguments[index], L"--inspect-preview") == 0 && index+1<count) {
            const auto value=ParseUnsigned(arguments[++index]);
            valid=count==3 && !options.inspectPreview && value && *value &&
                *value<=std::numeric_limits<UINT_PTR>::max();
            if (valid) options.inspectPreview=static_cast<WindowHandle>(*value);
        } else if (std::wcscmp(arguments[index], L"--inspect-fixture") == 0 && index+1<count) {
            const auto value=ParseUnsigned(arguments[++index]);
            valid=count==3 && !options.inspectFixture && value && *value &&
                *value<=std::numeric_limits<UINT_PTR>::max();
            if (valid) options.inspectFixture=static_cast<WindowHandle>(*value);
        } else if (std::wcscmp(arguments[index], L"--preview-target") == 0 && index+1<count) {
            const auto selection=ParsePreviewSelection(arguments[++index]);
            valid=selection && options.preview.size()<2;
            if (valid) {
                for (const auto& previous:options.preview) if (previous.hwnd==selection->hwnd) valid=false;
                if (valid) options.preview.push_back(*selection);
            }
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
    if (!options.preview.empty())
        valid=valid && !options.target && options.testMode && !storage.empty() && !suffix.empty() &&
            !options.probeOutput.empty() && options.probeOutput.is_absolute();
    return valid && (!options.testMode || options.target || !options.preview.empty())
        && (options.probeOutput.empty() || options.target || !options.preview.empty());
}

bool BindFixtureStorage(Options& options) {
    if (!options.testMode || (!options.configuration.storage_directory.empty() && options.preview.empty())) return true;
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
    if (options.configuration.storage_directory.empty()) {
        options.configuration.storage_directory = parent / L"fixture-configuration";
        options.configuration.endpoint_suffix = window_sweaters::configuration::TestEndpointSuffix(
            options.configuration.storage_directory);
    }
    return true;
}

std::optional<PreviewSelection> ObservePreviewSelection(WindowHandle hwnd) {
    const auto observation=WindowTracker::Query(reinterpret_cast<HWND>(hwnd));
    Policy policy;
    policy.hostPid=GetCurrentProcessId();
    const auto eligibility=Evaluate(observation,policy);
    if (eligibility!=Eligibility::Eligible && eligibility!=Eligibility::Suppressed) return std::nullopt;
    const auto& window=std::get<AliveWindow>(observation);
    return PreviewSelection{hwnd,window.identity};
}

int InspectPreview(WindowHandle hwnd) {
    ComApartment apartment(COINIT_MULTITHREADED);
    if (FAILED(apartment.Result())) return 5;
    const auto selection=ObservePreviewSelection(hwnd);
    if (!selection) return 3;
    const auto token=window_sweaters::applications::TargetToken::FromTarget(
        {hwnd,selection->identity.pid,selection->identity.processStart,1},0);
    if (!token) return 3;
    const auto metadata=window_sweaters::applications::QueryWindowOwnerMetadata(*token);
    std::printf("{\"preview_token\":\"%llu:%lu:%llu:%llu\",",
        static_cast<unsigned long long>(hwnd),static_cast<unsigned long>(selection->identity.pid),
        static_cast<unsigned long long>(selection->identity.processStart),
        static_cast<unsigned long long>(selection->identity.observedIdentity));
    if (const auto* unavailable=std::get_if<window_sweaters::applications::MetadataUnavailable>(&metadata)) {
        std::printf("\"metadata_available\":false,\"reason\":%u,\"stage\":%u,\"native_error\":%lu}\n",
            static_cast<unsigned>(unavailable->reason),static_cast<unsigned>(unavailable->stage),
            static_cast<unsigned long>(unavailable->nativeError));
        return 3;
    }
    const auto& identity=std::get<window_sweaters::applications::AppMetadata>(metadata).Identity();
    const auto resolution=window_sweaters::colorways::ResolveAppearance(&identity,{}, {},
        window_sweaters::colorways::QueryWindowsInstallRoots());
    if (const auto* error=std::get_if<window_sweaters::colorways::ResolutionError>(&resolution)) {
        std::printf("\"metadata_available\":true,\"resolution_error\":%u}\n",static_cast<unsigned>(*error));
        return 3;
    }
    const auto& appearance=std::get<window_sweaters::colorways::ResolvedAppearance>(resolution);
    std::printf("\"metadata_available\":true,\"theme_id\":\"%.*s\",\"base_argb\":%lu}\n",
        static_cast<int>(appearance.theme_id ? appearance.theme_id->size() : 0),
        appearance.theme_id ? appearance.theme_id->data() : "",
        static_cast<unsigned long>(appearance.base_argb));
    return 0;
}

struct FixtureSelectionDiagnostics {
    unsigned observation{};
    Eligibility eligibility{Eligibility::Uncertain};
    DWORD error{};
    int titleCharacters{};
    const char* failure{"query"};
    bool ordinary{};
    bool fixtureClass{}, fixtureTitle{};
    NativeQueryFailure query;
};

std::optional<WindowIdentity> SelectFixture(HWND target, FixtureSelectionDiagnostics* diagnostic = nullptr) {
    const auto observation = WindowTracker::Query(target,diagnostic ? &diagnostic->query : nullptr);
    if (diagnostic) {
        diagnostic->observation=static_cast<unsigned>(observation.index());
        diagnostic->error=diagnostic->query.error;
    }
    Policy policy;
    policy.mode = TrackingMode::FixtureOnly;
    policy.fixtureTarget = reinterpret_cast<WindowHandle>(target);
    policy.hostPid = GetCurrentProcessId();
    const auto eligibility = Evaluate(observation, policy);
    if (diagnostic) {
        diagnostic->eligibility=eligibility;
        diagnostic->failure=std::holds_alternative<AliveWindow>(observation) ? "eligibility" : "native_query";
        if (const auto* alive=std::get_if<AliveWindow>(&observation))
            diagnostic->ordinary=alive->ordinaryTopLevel;
    }
    if (eligibility != Eligibility::Eligible && eligibility != Eligibility::Suppressed) return std::nullopt;
    const auto& snapshot = std::get<AliveWindow>(observation);
    if (diagnostic) diagnostic->ordinary=snapshot.ordinaryTopLevel;
    wchar_t title[128]{};
    const auto characters=GetWindowTextW(target, title, static_cast<int>(std::size(title)));
    if (diagnostic) {
        diagnostic->titleCharacters=characters;
        diagnostic->error=characters ? 0 : GetLastError();
        diagnostic->failure=characters ? "fixture_class_or_title" : "title_query";
        diagnostic->fixtureClass=snapshot.className==L"WindowSweaters.TestFixture";
        diagnostic->fixtureTitle=std::wstring_view(title)==L"WindowSweaters Fixture";
    }
    if (!characters || !MatchesFixtureSelection(snapshot, snapshot.identity, title)) return std::nullopt;
    if (diagnostic) diagnostic->failure="none";
    return snapshot.identity;
}

std::string FixtureSelectionJson(WindowHandle target, bool selected,
    const FixtureSelectionDiagnostics& diagnostic) {
    char text[768]{};
    sprintf_s(text,"{\"target_hwnd\":%llu,\"selected\":%s,\"failure\":\"%s\","
        "\"observation\":%u,\"eligibility\":%u,\"native_error\":%lu,\"native_stage\":%u,"
        "\"native_hresult\":%ld,\"title_characters\":%d,\"ordinary\":%s,\"fixture_class\":%s,\"fixture_title\":%s}\n",
        static_cast<unsigned long long>(target),selected ? "true" : "false",diagnostic.failure,
        diagnostic.observation,static_cast<unsigned>(diagnostic.eligibility),
        static_cast<unsigned long>(diagnostic.error),static_cast<unsigned>(diagnostic.query.stage),
        static_cast<long>(diagnostic.query.result),diagnostic.titleCharacters,
        diagnostic.ordinary ? "true" : "false",diagnostic.fixtureClass ? "true" : "false",
        diagnostic.fixtureTitle ? "true" : "false");
    return text;
}

int InspectFixture(WindowHandle target) {
    FixtureSelectionDiagnostics diagnostic;
    const auto selected=SelectFixture(reinterpret_cast<HWND>(target),&diagnostic);
    std::printf("%s",FixtureSelectionJson(target,selected.has_value(),diagnostic).c_str());
    return selected ? 0 : 3;
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
    if (!options.preview.empty()) {
        config.mode = TrackingMode::SelectedOnly;
        config.selectedTargets = options.preview;
        config.maxTargets = MaximumSelectedTargets;
    }
    config.excludedClasses = {HostClass, L"WindowSweaters.Overlay", L"WindowSweaters.Settings",
        L"WindowSweaters.Settings.Activation"};
    config.excludedPids = {GetCurrentProcessId()};
    return config;
}

class Host final {
    Host(HINSTANCE instance,const Options& options,
        window_sweaters::integration::ConfigurationHost& configuration,
        window_sweaters::configuration::CommittedState owned)
        : instance_(instance), options_(options), configuration_(configuration),
          committed_(std::move(owned.configuration)),
          imported_(committed_.imports,std::move(owned.catalogue)),
          appearance_(window_sweaters::host::MakeAppearance(committed_,&imported_)),
          installRoots_(window_sweaters::colorways::QueryWindowsInstallRoots()),
          renderRevision_(committed_.revision), metadataGate_(committed_.revision) {}
public:
    Host(HINSTANCE instance, const Options& options,
        window_sweaters::integration::ConfigurationHost& configuration)
        : Host(instance,options,configuration,configuration.OwnedSnapshot()) {}
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
        iconWorker_=std::make_unique<window_sweaters::icon_yarn::PreparationWorker>(
            window_sweaters::icon_yarn::SampleExecutableIcon);
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
        const auto selected = options_.target ? reinterpret_cast<WindowHandle>(options_.target) :
            options_.preview.empty() ? WindowHandle{} : options_.preview.front().hwnd;
        const auto found = entries_.find(selected);
        const Entry* entry = found == entries_.end() ? nullptr : found->second.get();
        const auto* tile = entry && entry->appearance ?
            std::get_if<window_sweaters::knitting::TileSpec>(&entry->appearance->result) : nullptr;
        if (!tile && !window_sweaters::contracts::PerApplication(committed_.design_selection))
            tile = &appearance_;
        const auto* appearanceFailure = entry && entry->appearance ?
            std::get_if<AppearanceFailure>(&entry->appearance->result) : nullptr;
        const auto targetBounds = entry ? entry->state.pendingRect : PhysicalRect{};
        const RECT bounds = entry ? entry->overlay.Bounds() : RECT{};
        const auto stats = tracker_ ? tracker_->Stats() : finalStats_;
        const auto queueStats = queue_ ? queue_->Status() : finalQueueStats_;
        const auto configurationStatus = configuration_.Status();
        const auto visibilityStats = visibility_ ? visibility_->Stats() : window_sweaters::visibility::NativeStats{};
        const auto iconStats = iconWorker_ ? iconWorker_->Statistics() : finalIconStats_;
        output << "{\n"
            << "  \"host_pid\": " << GetCurrentProcessId() << ",\n"
            << "  \"host_hwnd\": " << reinterpret_cast<UINT_PTR>(window_) << ",\n"
            << "  \"overlay_hwnd\": " << reinterpret_cast<UINT_PTR>(entry ? entry->overlay.Window() : nullptr) << ",\n"
            << "  \"target_hwnd\": " << selected << ",\n"
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
            << "  \"chart_id\": \"" << (tile && tile->chart ? tile->chart->semanticId : "")
            << "\", \"by_app_plain_fallback\": "
            << (committed_.design_selection == window_sweaters::contracts::DesignSelection::by_app &&
                tile && !tile->chart ? "true" : "false") << ",\n"
            << "  \"appearance_resolutions\": " << counters_.appearanceResolutions
            << ", \"icon_queries\": " << iconStats.queries << ", \"icon_cache_hits\": " << iconStats.cacheHits
            << ", \"icon_pending_jobs\": " << iconStats.pendingJobs << ", \"icon_subscribers\": " << iconStats.subscribers
            << ", \"icon_cache_entries\": " << iconStats.cacheEntries
            << ", \"icon_accepted\": " << counters_.iconAccepted << ", \"icon_stale\": " << counters_.iconStale
            << ", \"icon_recolors\": " << counters_.iconRecolors << ", \"icon_full\": " << counters_.iconFull
            << ", \"icon_invalid\": " << counters_.iconInvalid
            << ", \"icon_pending_bytes\": " << iconStats.pendingBytes << ", \"icon_cache_bytes\": " << iconStats.cacheBytes
            << ", \"appearance_errors\": " << counters_.appearanceErrors
            << ", \"appearance_error\": " << (appearanceFailure ? appearanceFailure->result : S_OK)
            << ", \"resolution_error\": " << (appearanceFailure && appearanceFailure->resolution ?
                static_cast<int>(*appearanceFailure->resolution) : -1) << ",\n"
            << "  \"render_epoch\": " << (entry ? entry->renderEpoch : 0)
            << ", \"metadata_available\": " << (entry && entry->metadata &&
                std::holds_alternative<window_sweaters::applications::AppMetadata>(*entry->metadata) ? "true" : "false") << ",\n"
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
            << "  \"color_rgb\": [" << (((tile ? tile->baseArgb : committed_.yarn_argb) >> 16) & 255) << ", "
            << (((tile ? tile->baseArgb : committed_.yarn_argb) >> 8) & 255) << ", " << ((tile ? tile->baseArgb : committed_.yarn_argb) & 255)
            << "], \"render_kind\": \"native-knit\",\n"
            << "  \"composition\": \"Four narrow BGRA perimeter surfaces / DirectComposition / content-excluded / layered alpha 255 + transparent + no-redirection\",\n"
            << "  \"preview_mode\": " << (!options_.preview.empty() ? "true" : "false") << ", \"targets\": [";
        bool first=true;
        for (const auto& [hwnd, target] : entries_) {
            const auto* effective=target->appearance ?
                std::get_if<window_sweaters::knitting::TileSpec>(&target->appearance->result) : nullptr;
            const auto* failure=target->appearance ?
                std::get_if<AppearanceFailure>(&target->appearance->result) : nullptr;
            output << (first ? "\n" : ",\n") << "    {\"hwnd\":" << hwnd << ",\"pid\":" << target->state.target.pid
                << ",\"process_start\":" << target->state.target.processStart << ",\"generation\":" << target->state.target.generation
                << ",\"render_epoch\":" << target->renderEpoch << ",\"chart_id\":\""
                << (effective && effective->chart ? effective->chart->semanticId : "") << "\",\"base_argb\":"
                << (effective ? effective->baseArgb : 0) << ",\"stitch\":" << (effective ? static_cast<unsigned>(effective->stitch) : 0)
                << ",\"material\":" << (effective ? static_cast<unsigned>(effective->material) : 0)
                << ",\"metadata_available\":" << (target->metadata &&
                    std::holds_alternative<window_sweaters::applications::AppMetadata>(*target->metadata) ? "true" : "false")
                << ",\"appearance_error\":" << (failure ? failure->result : S_OK)
                << ",\"icon_pending\":" << (target->iconPending ? "true" : "false")
                << ",\"icon_sampled\":" << (target->iconPalette &&
                    std::holds_alternative<window_sweaters::icon_yarn::SampledPalette>(*target->iconPalette) ? "true" : "false")
                << ",\"icon_fallback\":" << (target->iconPalette &&
                    std::holds_alternative<window_sweaters::icon_yarn::Fallback>(*target->iconPalette) ?
                    static_cast<int>(std::get<window_sweaters::icon_yarn::Fallback>(*target->iconPalette).reason) : -1)
                << ",\"overlay_hwnd\":" << reinterpret_cast<UINT_PTR>(target->overlay.Window())
                << ",\"overlay_visible\":" << (target->presentationVisible ? "true" : "false") << "}";
            first=false;
        }
        output << "\n  ]\n}\n";
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
        std::optional<window_sweaters::colorways::ResolutionError> nextAppearanceFailure;
    };
    struct AppearanceFailure {
        HRESULT result{};
        std::optional<window_sweaters::colorways::ResolutionError> resolution;
    };
    struct TargetAppearance {
        std::uint64_t revision{};
        window_sweaters::applications::ImmutableMetadata owner;
        std::variant<window_sweaters::knitting::TileSpec, AppearanceFailure> result;
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
        std::optional<TargetAppearance> appearance;
        bool metadataPending{};
        std::uint64_t nextMetadataQuery{};
        std::uint64_t renderEpoch{};
        std::uint64_t eligibilityRevision{};
        bool eligibilityObserved{};
        std::optional<window_sweaters::icon_yarn::IconInput> iconInput;
        window_sweaters::icon_yarn::ImmutablePalette iconPalette;
        std::uint64_t iconEpoch{1}, nextIconRetry{};
        unsigned iconAttempts{};
        bool iconPending{};
    };
    struct Counters {
        std::uint64_t creates{}, removes{}, geometryUpdates{}, draws{}, pureMoves{};
        std::uint64_t hides{}, shows{}, reorders{}, staleActions{}, placementFailures{};
        std::uint64_t uploads{}, renderErrors{}, staleResults{};
        std::uint64_t appearanceResolutions{}, appearanceErrors{};
        std::uint64_t iconAccepted{}, iconStale{}, iconRecolors{}, iconFull{}, iconInvalid{};
        std::uint64_t visibilityObservations{}, workValidations{}, geometryQueries{}, applicationPublications{};
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
                        host->PollIcons();
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
        const auto snapshot = configuration_.PollOwnedSnapshot(committed_.revision);
        if (!snapshot) return;
        ApplyConfiguration(snapshot->configuration,snapshot->catalogue);
    }

    void ApplyConfiguration(const window_sweaters::contracts::Configuration& snapshot,
        window_sweaters::configuration::CatalogueSnapshot catalogue={}) {
        if (snapshot.imports!=committed_.imports) {
            auto onlyImports=snapshot;
            onlyImports.revision=committed_.revision; onlyImports.imports=committed_.imports;
            if (onlyImports==committed_) {
                auto nextImported=window_sweaters::integration::ImportedAppearance(snapshot.imports,
                    catalogue?catalogue:configuration_.OwnedSnapshot().catalogue,&imported_);
                auto nextAppearance=window_sweaters::host::MakeAppearance(snapshot,&nextImported);
                for (auto& [hwnd,entry]:entries_) {
                    (void)hwnd;
                    bool same=false;
                    if (entry->appearance) {
                        auto old=std::get_if<window_sweaters::knitting::TileSpec>(&entry->appearance->result);
                        auto known=entry->metadata?std::get_if<window_sweaters::applications::AppMetadata>(entry->metadata.get()):nullptr;
                        try {
                            auto automatic=entry->iconInput && entry->iconPalette?
                                window_sweaters::integration::AutomaticProposal(*entry->iconInput,*entry->iconPalette):
                                std::optional<window_sweaters::colorways::AutomaticAppearance>{};
                            auto proposed=known?window_sweaters::host::MakeAppearance(snapshot,known->Identity(),
                                installRoots_,automatic,{},&nextImported):nextAppearance;
                            same=old && SameAutomaticAppearance(*old,proposed);
                        } catch (const std::invalid_argument&) {}
                    }
                    if (same) {
                        const bool identityDependent=window_sweaters::contracts::PerApplication(snapshot.design_selection) ||
                            !nextImported.Personal().empty();
                        entry->appearance->owner=identityDependent?entry->metadata:nullptr;
                        continue;
                    }
                    AdvanceRenderEpoch(*entry); Hide(*entry); entry->render.Invalidate();
                    entry->appearance.reset(); entry->placementPending=entry->state.requestedVisible;
                }
                imported_=std::move(nextImported); committed_=snapshot; appearance_=std::move(nextAppearance);
                metadataGate_.SetConfigurationRevision(committed_.revision);
                for (auto& [hwnd,entry]:entries_) { (void)hwnd; entry->metadataPending=false; }
                PublishApplications(); probeDirty_=true; return;
            }
        }
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
                ForgetIcon(*entry, true);
                AdvanceRenderEpoch(*entry);
                if (!committed_.eligibility.Allows(*entry->metadata)) Hide(*entry);
                else entry->placementPending=entry->state.requestedVisible;
            }
            PublishApplications(); probeDirty_=true; return;
        }
        auto nextImported=imported_;
        if (catalogue) nextImported=window_sweaters::integration::ImportedAppearance(snapshot.imports,catalogue,&imported_);
        auto appearance = window_sweaters::host::MakeAppearance(snapshot,&nextImported);
        renderRetries_.clear();
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            ForgetIcon(*entry);
            Hide(*entry);
            entry->render.Invalidate();
            AdvanceRenderEpoch(*entry);
            entry->appearance.reset();
            entry->placementPending = false;
        }
        committed_ = snapshot;
        imported_=std::move(nextImported);
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
            ForgetIcon(*entry);
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
            ForgetIcon(*entry);
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
        ++counters_.visibilityObservations;
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
        // Desktop COM may pump WM_CLOSE and remove this entry; keep no Entry reference or native authority across it.
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
                const HRESULT result = PlaceAndShow(target, entry->visibilityToken);
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
        ++counters_.workValidations;
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
            if (auto snapshot=configuration_.PollOwnedSnapshot(committed_.revision))
                ApplyConfiguration(snapshot->configuration,snapshot->catalogue);
        }
        return configuration_.Revision()==committed_.revision;
    }

    std::uint64_t EligibilityRevision(window_sweaters::applications::AppIdentity const& identity) const {
        if (!contract_) return configuration_.EligibilityRevision(identity);
        auto found=contract_->eligibilityRevisions.find(identity);
        return found==contract_->eligibilityRevisions.end()?0:found->second;
    }

    bool ResolveTargetAppearance(Entry& entry) {
        using namespace window_sweaters;
        const bool byApp = contracts::PerApplication(committed_.design_selection) || !imported_.Personal().empty();
        const auto* known = entry.metadata ? std::get_if<applications::AppMetadata>(entry.metadata.get()) : nullptr;
        if (byApp && !known) return false;
        if (entry.appearance && entry.appearance->revision == renderRevision_) {
            const auto* bound = entry.appearance->owner ?
                std::get_if<applications::AppMetadata>(entry.appearance->owner.get()) : nullptr;
            if (!byApp || (bound && bound->Identity() == known->Identity())) {
                if (byApp) entry.appearance->owner = entry.metadata;
                return std::holds_alternative<knitting::TileSpec>(entry.appearance->result);
            }
            AdvanceRenderEpoch(entry);
            Hide(entry);
            entry.placementPending = entry.state.requestedVisible;
        }
        ++counters_.appearanceResolutions;
        TargetAppearance appearance{renderRevision_, byApp ? entry.metadata : nullptr, AppearanceFailure{E_FAIL, {}}};
        try {
            if (byApp && contract_ && contract_->nextAppearanceFailure) {
                const auto error = *contract_->nextAppearanceFailure;
                contract_->nextAppearanceFailure.reset();
                appearance.result = host::MakeByAppAppearance(committed_, error);
            } else {
                const auto automatic = entry.iconInput && entry.iconPalette ?
                    integration::AutomaticProposal(*entry.iconInput, *entry.iconPalette) :
                    std::optional<colorways::AutomaticAppearance>{};
                appearance.result = byApp ? host::MakeAppearance(committed_, known->Identity(), installRoots_, automatic,{},&imported_) : appearance_;
            }
        } catch (const host::AppearanceResolutionError& error) {
            appearance.result = AppearanceFailure{E_INVALIDARG, error.Error()};
        } catch (const std::invalid_argument&) {
            appearance.result = AppearanceFailure{E_INVALIDARG, {}};
        } catch (const std::bad_alloc&) {
            appearance.result = AppearanceFailure{E_OUTOFMEMORY, {}};
        }
        entry.appearance = std::move(appearance);
        if (const auto* failure = std::get_if<AppearanceFailure>(&entry.appearance->result)) {
            ++counters_.appearanceErrors;
            LogError(L"target appearance rejected; hidden until owner or appearance configuration changes", failure->result);
            return false;
        }
        return true;
    }

    bool EligibleNow(Entry& entry) {
        if (!ConfigurationCurrent()) return false;
        if (entry.state.excluded || !entry.state.requestedVisible || committed_.paused) return false;
        if (contract_ && !contract_->requireMetadata &&
            !window_sweaters::contracts::PerApplication(committed_.design_selection))
            return ResolveTargetAppearance(entry);
        auto known=entry.metadata?std::get_if<window_sweaters::applications::AppMetadata>(entry.metadata.get()):nullptr;
        if (!known) return false;
        const auto revision=EligibilityRevision(known->Identity());
        if (entry.eligibilityObserved && revision!=entry.eligibilityRevision) {
            ForgetIcon(entry, true);
            AdvanceRenderEpoch(entry);
            Hide(entry);
            entry.placementPending=entry.state.requestedVisible;
        }
        entry.eligibilityObserved=true; entry.eligibilityRevision=revision;
        return ConfigurationCurrent() && committed_.eligibility.Allows(known->Identity()) &&
            ResolveTargetAppearance(entry);
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
        if (stopped_) return;
        ++counters_.applicationPublications;
        if (contract_) return;
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
                    ForgetIcon(*entry, true);
                    AdvanceRenderEpoch(*entry); Hide(*entry); entry->eligibilityObserved=false;
                    entry->appearance.reset();
                } else if (entry->appearance && entry->appearance->owner) {
                    entry->appearance->owner = entry->metadata;
                }
            }
            if (auto known=std::get_if<AppMetadata>(entry->metadata.get())) {
                entry->state.excluded=known->ExecutableAlias() &&
                    HelperImage(known->ExecutableAlias()->c_str(),options_.target!=nullptr);
                if (entry->iconInput && CompareStringOrdinal(entry->iconInput->executablePath.data(),
                    static_cast<int>(entry->iconInput->executablePath.size()), known->ExecutableSource().data(),
                    static_cast<int>(known->ExecutableSource().size()), TRUE) != CSTR_EQUAL) {
                    ForgetIcon(*entry, true);
                    entry->appearance.reset();
                    AdvanceRenderEpoch(*entry); Hide(*entry);
                }
            }
            if (entry->state.excluded || !committed_.eligibility.Allows(*entry->metadata))
                ForgetIcon(*entry, true);
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

    void ForgetIcon(Entry& entry, bool discard = false) {
        iconGate_.Forget(entry.state.target.hwnd);
        if (iconWorker_) iconWorker_->Forget(entry.state.target.hwnd);
        if (entry.iconEpoch == UINT64_MAX) throw std::overflow_error("Icon appearance epoch exhausted");
        ++entry.iconEpoch;
        entry.iconPending = false;
        entry.iconAttempts = 0;
        entry.nextIconRetry = 0;
        if (discard) { entry.iconInput.reset(); entry.iconPalette.reset(); }
    }

    std::optional<window_sweaters::icon_yarn::WorkToken> CurrentIconToken(const Entry& entry) const {
        using namespace window_sweaters;
        const auto* owner = entry.metadata ? std::get_if<applications::AppMetadata>(entry.metadata.get()) : nullptr;
        if (stopped_ || !contracts::PerApplication(committed_.design_selection) || !owner ||
            entry.state.excluded || !entry.state.requestedVisible || committed_.paused ||
            !committed_.eligibility.Allows(owner->Identity()) || !recovery_.CanRender(recovery_.Generation()) ||
            Model().Current(entry.state.target.hwnd) != entry.state.target)
            return {};
        return icon_yarn::WorkToken{entry.state.target.hwnd, entry.state.target.processStart,
            entry.state.target.generation, entry.iconEpoch, recovery_.Generation()};
    }

    static bool SameAutomaticAppearance(const window_sweaters::knitting::TileSpec& a,
        const window_sweaters::knitting::TileSpec& b) {
        if (a.baseArgb != b.baseArgb || a.stitch != b.stitch || a.material != b.material ||
            a.gauge.rows != b.gauge.rows || a.bandDip != b.bandDip) return false;
        if (a.chart == b.chart) return true;
        return a.chart && b.chart && a.chart->semanticId == b.chart->semanticId &&
            a.chart->contentRevision == b.chart->contentRevision &&
            a.chart->width == b.chart->width && a.chart->height == b.chart->height &&
            a.chart->appearance == b.chart->appearance && a.chart->argbCells == b.chart->argbCells;
    }

    void PollIcons() {
        using namespace window_sweaters;
        if (!iconWorker_ || !ConfigurationCurrent()) return;
        for (const auto& completion : iconWorker_->Drain()) {
            const auto found = entries_.find(completion.request.Token().targetId);
            auto* entry = found == entries_.end() ? nullptr : found->second.get();
            const auto current = entry ? CurrentIconToken(*entry) : std::nullopt;
            if (!entry || !entry->iconInput || *entry->iconInput != *completion.request.Input() ||
                !iconGate_.Accept(completion, current)) {
                ++counters_.iconStale;
                if (entry && entry->iconPending && entry->iconEpoch == completion.request.Token().appearanceRevision)
                    ForgetIcon(*entry);
                continue;
            }
            entry->iconPending = false;
            entry->iconPalette = completion.palette;
            ++counters_.iconAccepted;
            if (std::holds_alternative<icon_yarn::SampledPalette>(*completion.palette)) {
                const auto* owner = std::get_if<applications::AppMetadata>(entry->metadata.get());
                try {
                    auto proposed = host::MakeAppearance(committed_, owner->Identity(), installRoots_,
                        integration::AutomaticProposal(*entry->iconInput, *completion.palette),{},&imported_);
                    const auto* old = entry->appearance ?
                        std::get_if<knitting::TileSpec>(&entry->appearance->result) : nullptr;
                    if (!old || !SameAutomaticAppearance(*old, proposed)) {
                        AdvanceRenderEpoch(*entry);
                        Hide(*entry);
                        entry->appearance = TargetAppearance{renderRevision_, entry->metadata, std::move(proposed)};
                        entry->placementPending = entry->state.requestedVisible;
                        ++counters_.iconRecolors;
                    }
                } catch (const std::exception&) {
                    entry->iconPalette = std::make_shared<const icon_yarn::PaletteResult>(
                        icon_yarn::Fallback{icon_yarn::FallbackReason::InvalidResource});
                    ++counters_.iconInvalid;
                    LogError(L"icon proposal rejected; stable provisional appearance retained", E_INVALIDARG);
                }
            }
            probeDirty_ = true;
        }
        std::size_t budget = 8;
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            auto current = CurrentIconToken(*entry);
            if (!current) {
                if (entry->iconPending) ForgetIcon(*entry);
                continue;
            }
            if (!budget || entry->iconPending || entry->iconPalette ||
                entry->iconAttempts >= 4 || GetTickCount64() < entry->nextIconRetry) continue;
            const auto& owner = std::get<applications::AppMetadata>(*entry->metadata);
            if (committed_.design_selection == contracts::DesignSelection::by_app &&
                colorways::MatchWindowsApplication(owner.Identity(), installRoots_)) continue;
            auto input = integration::MakeIconInput(owner.Identity(), owner.ExecutableSource());
            if (const auto* failure = std::get_if<icon_yarn::Fallback>(&input)) {
                entry->iconPalette = std::make_shared<const icon_yarn::PaletteResult>(*failure);
                ++counters_.iconInvalid; probeDirty_ = true;
                LogError(L"icon identity/source rejected; stable provisional yarn retained", E_INVALIDARG);
                continue;
            }
            entry->iconInput = std::move(std::get<icon_yarn::IconInput>(input));
            auto request = iconGate_.Begin(*current, *entry->iconInput);
            ++entry->iconAttempts;
            --budget;
            if (!request) {
                entry->iconPalette = std::make_shared<const icon_yarn::PaletteResult>(
                    icon_yarn::Fallback{icon_yarn::FallbackReason::ResourceLimit});
                ++counters_.iconInvalid; probeDirty_ = true;
                LogError(L"icon request admission rejected; stable provisional yarn retained",
                    HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA));
                continue;
            }
            const auto submitted = iconWorker_->Submit(*request);
            if (submitted == icon_yarn::SubmitStatus::Accepted ||
                submitted == icon_yarn::SubmitStatus::Deduplicated || submitted == icon_yarn::SubmitStatus::Cached) {
                entry->iconPending = true;
            } else {
                iconGate_.Forget(entry->state.target.hwnd);
                if (submitted == icon_yarn::SubmitStatus::Full) {
                    ++counters_.iconFull;
                    entry->nextIconRetry = GetTickCount64() + 250 * entry->iconAttempts;
                    if (entry->iconAttempts == 4)
                        entry->iconPalette = std::make_shared<const icon_yarn::PaletteResult>(
                            icon_yarn::Fallback{icon_yarn::FallbackReason::ResourceLimit});
                } else {
                    entry->iconPalette = std::make_shared<const icon_yarn::PaletteResult>(
                        icon_yarn::Fallback{icon_yarn::FallbackReason::QueryFailed});
                    ++counters_.iconInvalid;
                }
                probeDirty_ = true;
            }
        }
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
        ++counters_.geometryQueries;
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
        return MoveCurrent(*current, CurrentGeometry(*current));
    }

    // Caller owns the probe and validation; no pumping boundary may separate them from this move.
    HRESULT MoveCurrent(Entry& entry, const std::optional<window_sweaters::geometry::RenderInput>& fresh) {
        if (!EligibleNow(entry) || !fresh || !entry.visibilityToken ||
            !entry.visibility.CanPublish(VisibilityPolicy(), *entry.visibilityToken)) {
            Hide(entry);
            return S_FALSE;
        }
        const auto& input = *fresh;
        if (!entry.render.prepared || !Matches(*entry.render.prepared, entry, input)) {
            Hide(entry);
            return S_FALSE;
        }
        const auto rect = window_sweaters::host::TrackedRect(input.physicalTargetBounds);
        const RECT outer = NativeRect(input.physicalOuterBounds);
        const auto& previous = entry.overlay.Bounds();
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

    HRESULT Prepare(TargetId target, std::optional<window_sweaters::geometry::RenderInput>* matched = nullptr,
        const std::optional<window_sweaters::visibility::ProbeToken>& probed = {}) {
        auto current = Find(target);
        if (stopped_ || !current || !recovery_.CanRender(recovery_.Generation())) return S_FALSE;
        auto& entry = *current;
        if (!EligibleNow(entry)) { Hide(entry); return S_FALSE; }
        const bool created = !entry.overlay.Window();
        if (created) {
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
        const bool reuse = probed && !created && entry.visibilityToken == probed &&
            entry.visibility.CanPublish(VisibilityPolicy(), *probed);
        if (!reuse && !VisibilityAllows(target)) return S_FALSE;
        current = Find(target);
        if (stopped_ || !current || !queue_) return S_FALSE;
        const Action action{ActionKind::Move, target, current->state.pendingRect, current->state.dpi,
            current->state.appearanceRevision};
        if (!Validate(action)) return S_FALSE;
        current = Find(target);
        if (stopped_ || !current || !queue_) return S_FALSE;
        return PrepareCurrent(*current, matched);
    }

    HRESULT PrepareCurrent(Entry& entry, std::optional<window_sweaters::geometry::RenderInput>* matched = nullptr) {
        if (!EligibleNow(entry) || !recovery_.CanRender(recovery_.Generation()) || !entry.visibilityToken ||
            !entry.visibility.CanPublish(VisibilityPolicy(), *entry.visibilityToken)) {
            Hide(entry);
            return S_FALSE;
        }
        const auto input = CurrentGeometry(entry);
        if (!input) return S_FALSE;
        if (entry.render.prepared && Matches(*entry.render.prepared, entry, *input)) {
            if (matched) matched->emplace(*input);
            return S_OK;
        }
        Hide(entry);
        if (entry.render.pending && Matches(*entry.render.pending, entry, *input)) return S_FALSE;
        if (entry.render.failed && Matches(*entry.render.failed, entry, *input)) return S_FALSE;
        if (entry.render.retry && Matches(*entry.render.retry, entry, *input)) return S_FALSE;
        const auto request = window_sweaters::host::MakeRenderRequest(
            {entry.state.target, entry.renderEpoch}, renderRevision_, recovery_.Generation(), *input,
            std::get<window_sweaters::knitting::TileSpec>(entry.appearance->result));
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
            result = ShowCurrent(request.token.target, publication);
            if (FAILED(result)) {
                if (auto remaining = Find(request.token.target)) Hide(*remaining);
                LogError(L"knit publication placement", result);
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

    HRESULT PlaceAndShow(TargetId identity, const std::optional<window_sweaters::visibility::ProbeToken>& probed = {}) {
        auto current = Find(identity);
        if (stopped_ || !current) return S_FALSE;
        if (!EligibleNow(*current) || queueFaulted_) {
            Hide(*current);
            return current->appearance && std::holds_alternative<AppearanceFailure>(current->appearance->result)
                ? S_FALSE : S_OK;
        }
        std::optional<window_sweaters::geometry::RenderInput> matched;
        const HRESULT result = Prepare(identity, &matched, probed);
        return result == S_OK ? ShowCurrent(identity, matched) : result;
    }

    HRESULT ShowCurrent(TargetId identity, const std::optional<window_sweaters::geometry::RenderInput>& fresh) {
        auto current = Find(identity);
        if (stopped_ || !current) return S_FALSE;
        if (committed_.paused || !current->state.requestedVisible || queueFaulted_) { Hide(*current); return S_FALSE; }
        HRESULT result = MoveCurrent(*current, fresh);
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
        bool publish{};
        HRESULT failure = S_OK;
        for (auto action : actions) {
            if (stopped_ && action.kind != ActionKind::Remove) continue;
            Entry* entry = Find(action.target);
            if (action.kind == ActionKind::Create) {
                if (Model().Current(action.target.hwnd) != action.target) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry) {
                    if (entries_.contains(action.target.hwnd)) { failure = E_UNEXPECTED; break; }
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
                if (!Validate(action)) { ++counters_.staleActions; continue; }
                entry = Find(action.target);
                if (!entry) { ++counters_.staleActions; continue; }
                const HRESULT result = Prepare(action.target);
                if (FAILED(result)) { failure = result; break; }
                publish = true;
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
                ForgetIcon(*entry, true);
                entries_.erase(action.target.hwnd);
                metadataGate_.Forget(action.target.hwnd);
                publish = true;
                capacityFreed = true;
            } else if (action.kind == ActionKind::Hide) {
                publish=true;
                const WorkToken token{action.target, action.appearanceRevision};
                if (!Synchronize(*entry, token)) {
                    ++counters_.staleActions;
                    continue;
                }
                if (!entry->state.requestedVisible) {
                    if (entry->iconPending) ForgetIcon(*entry);
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
                    if (FAILED(result)) { failure = result; break; }
                } else if (entry->state.requestedVisible && !committed_.paused && entry->render.prepared) {
                    const HRESULT result = MovePrepared(action.target);
                    if (FAILED(result)) { failure = result; break; }
                    if (result == S_FALSE && !stopped_) {
                        if (auto remaining = Find(action.target)) remaining->placementPending = true;
                    }
                }
            } else {
                if (action.kind==ActionKind::Show) publish=true;
                if (!Validate(action)) {
                    if (auto pending = Find(action.target)) pending->placementPending = true;
                    ++counters_.staleActions;
                    continue;
                }
                entry = Find(action.target);
                if (!entry) { ++counters_.staleActions; continue; }
                const HRESULT result = PlaceAndShow(action.target);
                if (FAILED(result)) { failure = result; break; }
            }
            probeDirty_ = true;
        }
        if (publish) PublishApplications();
        if (FAILED(failure)) return failure;
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
        // Stop invalidates COM authority; keep the object alive until Host destruction.
        if (visibility_) visibility_->Stop();
        if (window_) { KillTimer(window_, ProbeTimer); KillTimer(window_, ConfigurationTimer); KillTimer(window_, RecoveryTimer); }
        for (auto& [hwnd, entry] : entries_) {
            (void)hwnd;
            Hide(*entry);
            entry->render.Invalidate();
            entry->placementPending = false;
        }
        configuration_.Quit();
        iconGate_.Clear();
        if (iconWorker_) {
            iconWorker_->Stop();
            finalIconStats_ = iconWorker_->Statistics();
            iconWorker_.reset();
        }
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
    window_sweaters::integration::ImportedAppearance imported_;
    window_sweaters::knitting::TileSpec appearance_;
    const window_sweaters::colorways::WindowsInstallRoots installRoots_;
    std::uint64_t renderRevision_{};
    window_sweaters::applications::CompletionGate metadataGate_;
    std::unique_ptr<window_sweaters::applications::MetadataWorker> metadataWorker_;
    window_sweaters::icon_yarn::CompletionGate iconGate_;
    std::unique_ptr<window_sweaters::icon_yarn::PreparationWorker> iconWorker_;
    window_sweaters::icon_yarn::PreparationStatistics finalIconStats_;
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
    const auto selection=ParsePreviewSelection(L"123:45:678:90");
    require(selection && selection->hwnd==123 && selection->identity==WindowIdentity{45,678,90},
        "Explicit preview selection parses complete process birth and native window fingerprint");
    for (const auto token:{L"123",L"123:45:678",L"123:45:678:90:1",L"0:45:678:90",L"123:0:678:90",
        L"123:45:0:90",L"123:45:678:0",L"123:4294967296:678:90",L"123:45:18446744073709551616:90",
        L"123:45:-678:90",L"123:45:678: 90"})
        require(!ParsePreviewSelection(token),"Explicit real-app preview rejects partial, zero, overflow and malformed lifetime tokens");
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
    FixtureSelectionDiagnostics invalidSelection;
    require(!SelectFixture(reinterpret_cast<HWND>(1),&invalidSelection) &&
        invalidSelection.query.stage==NativeQueryStage::Window &&
        invalidSelection.query.error==ERROR_INVALID_WINDOW_HANDLE &&
        invalidSelection.eligibility==Eligibility::Destroyed &&
        invalidSelection.failure==std::string_view("native_query"),
        "Exact fixture selector reports native invalid-window stage without creating a target");
    host.queueLimits_.maxReady = 1;
    require(SUCCEEDED(window_sweaters::rendering::RenderQueue::Create(
        host.window_, host.queueLimits_, host.queue_)) &&
        SUCCEEDED(host.queue_->Start(host.committed_.revision, DeviceGeneration)),
        "Consumer contract production render queue");
    {
        using namespace window_sweaters;
        const auto directory=std::filesystem::current_path()/L"out"/L"issue11"/
            (L"startup-consumer-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        struct Cleanup {
            std::filesystem::path directory;
            ~Cleanup() {std::error_code error; std::filesystem::remove_all(directory,error);}
        } cleanup{directory};
        auto path=directory/L"startup.png";
        knitting::SavePng({2,2,8,{0xffe02152u,0xff37d0a6u,0,0xffe02152u}},path.c_str());
        auto file=imports::ImportPngFile(path.native());
        require(std::holds_alternative<imports::FileImport>(file),"Running-server startup fixture validates its owned PNG");
        auto built=imports::BuildCandidate({},imports::AddChart{"startup-literal",std::get<imports::FileImport>(std::move(file)).chart});
        require(std::holds_alternative<imports::ImportCatalogue>(built),"Running-server startup fixture owns a nonempty catalogue");
        auto catalogue=std::make_shared<const imports::ImportCatalogue>(std::get<imports::ImportCatalogue>(std::move(built)));
        contracts::Configuration initial; initial.revision=7;
        configuration::DescribeCatalogue(initial.imports,*catalogue);
        require(configuration::Storage(directory).Save(initial,catalogue)==ERROR_SUCCESS,
            "Running-server startup fixture persists its initial revision and catalogue");
        integration::ConfigurationHost liveConfiguration({true,directory,
            L"startup-consumer-"+std::to_wstring(GetCurrentProcessId()),{}});
        require(liveConfiguration.Start().state==integration::HostState::running,
            "Actual sole-writer ConfigurationHost starts its native server before consumer construction");
        auto captured=liveConfiguration.OwnedSnapshot();
        require(liveConfiguration.SubmitLocal(false,24.f,captured.configuration.revision).state==
            integration::LocalSubmissionState::queued,"Actual server save interleaves after the one owned startup capture");
        const auto deadline=GetTickCount64()+10000;
        while (liveConfiguration.Revision()==captured.configuration.revision && GetTickCount64()<deadline) Sleep(1);
        auto completed=liveConfiguration.DrainLocalCompletions();
        require(completed.completions.size()==1 && completed.completions[0].result.status==contracts::Status::saved &&
            completed.current.revision==8,"Actual worker durably advances configuration during consumer startup");
        Host startup(host.instance_,options,liveConfiguration,std::move(captured));
        const auto target=TargetId{19600,GetCurrentProcessId(),1234,1};
        auto oldToken=applications::TargetToken::FromTarget(target,startup.committed_.revision);
        auto request=startup.metadataGate_.Begin(*oldToken);
        require(startup.committed_.revision==7 && startup.renderRevision_==7 && request &&
            startup.imported_.Find(catalogue->Charts()[0].Id(),1),
            "Startup appearance, catalogue, render revision and metadata gate derive from one coherent owned snapshot");
        startup.window_=CreateWindowExW(0,HostClass,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,startup.instance_,&startup);
        require(startup.window_ && SUCCEEDED(rendering::RenderQueue::Create(
            startup.window_,startup.queueLimits_,startup.queue_)) &&
            SUCCEEDED(startup.queue_->Start(startup.renderRevision_,DeviceGeneration)),
            "Startup consumer has its own hidden native receiver and render queue");
        require(startup.ConfigurationCurrent() && startup.committed_.revision==8 && startup.renderRevision_==8,
            "Real-server startup poll advances all consumer configuration epochs without another mutation");
        auto currentToken=applications::TargetToken::FromTarget(target,startup.committed_.revision);
        auto currentRequest=startup.metadataGate_.Begin(*currentToken);
        auto metadata=std::make_shared<const applications::MetadataResult>(applications::ResolveMetadata(
            applications::NoPackagedIdentity{},applications::ExecutablePath{L"C:\\PrivateFixtures\\startup.exe"}));
        require(currentRequest && !startup.metadataGate_.Accept({*request,metadata},currentToken) &&
            startup.metadataGate_.Accept({*currentRequest,metadata},currentToken),
            "Actual running-server startup interleaving retires old metadata and accepts current revision immediately");
        Host fresh(host.instance_,options,liveConfiguration);
        auto freshToken=applications::TargetToken::FromTarget(target,fresh.committed_.revision);
        require(fresh.committed_.revision==8 && fresh.renderRevision_==8 && fresh.metadataGate_.Begin(*freshToken),
            "Public Host constructor obtains one current owned snapshot from the actual running server");
        startup.Shutdown(); liveConfiguration.Quit();
        std::puts("PASS: actual running-server startup save interleaving keeps one owned configuration/catalogue/render/metadata snapshot");
    }
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
        if (!complete()) {
            for (const auto& [handle, entry] : host.entries_)
                std::printf("Consumer deadline: target=%llu mode=%u epoch=%llu prepared=%d pending=%d retry=%d metadata=%d appearance=%d\n",
                    static_cast<unsigned long long>(handle), static_cast<unsigned>(host.committed_.design_selection),
                    static_cast<unsigned long long>(entry->renderEpoch), entry->render.prepared.has_value(),
                    entry->render.pending.has_value(), entry->render.retry.has_value(), entry->metadata != nullptr,
                    entry->appearance.has_value());
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
    const auto beforeDrain = host.counters_;
    pump(prepared);
    require(host.entries_.at(10001)->presentationVisible && host.counters_.uploads == beforeDrain.uploads + 1 &&
        host.counters_.workValidations == beforeDrain.workValidations + 2 &&
        host.counters_.visibilityObservations == beforeDrain.visibilityObservations + 1 &&
        host.counters_.geometryQueries == beforeDrain.geometryQueries + 2,
        "Drained result shows with its post-probe validation and geometry; only the pre-probe gate repeats them");
    require(host.counters_.uploads == firstUploads + 1 &&
        host.entries_.at(10001)->render.prepared->frame.bandDip == 24.0f &&
        host.entries_.at(10001)->overlay.Bounds().right - host.entries_.at(10001)->overlay.Bounds().left == 373,
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
    const auto beforePureMove = host.counters_;
    require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(moved, Dirty::Geometry, GetTickCount64()))) &&
        host.counters_.pureMoves == beforePureMove.pureMoves + 1 &&
        host.counters_.workValidations == beforePureMove.workValidations + 1 &&
        host.counters_.visibilityObservations == beforePureMove.visibilityObservations + 1 &&
        host.counters_.geometryQueries == beforePureMove.geometryQueries + 1,
        "Actual consumer pure-position Move reads geometry once, after its probe and validation");
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

    ActionBatch retired;
    for (const auto& [handle, window] : windows) {
        if (handle != 10001)
            for (const auto& action : contract.model.Observe(DestroyedWindow{window.hwnd}, Dirty::Lifecycle, GetTickCount64()))
                retired.push_back(action);
    }
    const auto publications = host.counters_.applicationPublications;
    require(SUCCEEDED(host.ApplyBatch(retired)) && host.entries_.size() == 1 &&
        host.counters_.applicationPublications == publications + 1,
        "Retiring 64 burst lifetimes in one batch publishes running applications once");
    add(10002);
    const auto staleLifetime = host.entries_.at(10001)->state.target;
    auto partial = contract.model.Observe(DestroyedWindow{10002}, Dirty::Lifecycle, GetTickCount64());
    auto recovery = contract.model.Observe(DestroyedWindow{10001}, Dirty::Lifecycle, GetTickCount64());
    const auto recreated = contract.model.Observe(windows.at(10001), Dirty::Lifecycle, GetTickCount64());
    partial.insert(partial.end(), recreated.begin(), recreated.end());
    recovery.insert(recovery.end(), recreated.begin(), recreated.end());
    const auto beforePartial = host.counters_.applicationPublications;
    require(host.ApplyBatch(partial) == E_UNEXPECTED && !host.entries_.contains(10002) &&
        host.counters_.applicationPublications == beforePartial + 1,
        "Batch failing after a Remove still publishes the partial lifecycle change once");
    require(SUCCEEDED(host.ApplyBatch(recovery)) && host.entries_.size() == 1 &&
        host.entries_.at(10001)->state.target != staleLifetime, "Replayed Remove/Create recovers the reused HWND");
    pump(prepared);
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
    auto appMetadata=[](std::wstring const& path) {
        return std::make_shared<applications::MetadataResult const>(
            applications::ResolveMetadata(applications::NoPackagedIdentity{},applications::ExecutablePath{path}));
    };
    host.entries_.at(10001)->metadata=appMetadata(L"C:\\Tools\\Unknown.exe");
    auto byApp = host.committed_;
    byApp.design_selection = contracts::DesignSelection::by_app;
    appearanceChange(byApp);
    require(!host.entries_.at(10001)->render.prepared->tile.chart &&
        host.entries_.at(10001)->render.prepared->tile.baseArgb == 0xff7c9885u,
        "ByApp actual Host publication uses identity-stable Wool fallback, never a Finder example or saved manual yarn");
    contract.requireMetadata=true;
    {
        const auto previousConfiguration=host.committed_;
        auto mappedConfiguration=previousConfiguration;
        mappedConfiguration.border_width_dip=16.f;
        appearanceChange(mappedConfiguration);
        const auto chromeOwner=appMetadata(host.installRoots_.program_files + L"\\Google\\Chrome\\Application\\chrome.exe");
        const auto teamsOwner=std::make_shared<applications::MetadataResult const>(applications::ResolveMetadata(
            applications::PackagedAumid{L"MSTeams_8wekyb3d8bbwe!MSTeams"},
            applications::ExecutablePath{L"C:\\Packages\\Teams\\ms-teams.exe"}));
        const auto unknownOwner=appMetadata(L"C:\\Tools\\Unknown.exe");
        add(19001); add(19002); add(19003);
        require(!host.entries_.at(19001)->overlay.Window() && !host.entries_.at(19002)->render.pending,
            "ByApp mapped owners wait for accepted identity, never provisional manual yarn");
        host.entries_.at(19001)->metadata=chromeOwner;
        host.entries_.at(19002)->metadata=teamsOwner;
        host.entries_.at(19003)->metadata=unknownOwner;
        for (auto handle:{WindowHandle{19001},WindowHandle{19002},WindowHandle{19003}})
            require(SUCCEEDED(host.PlaceAndShow(host.entries_.at(handle)->state.target)), "Mapped/unmatched owner submits real Host worker input");
        pump(prepared);
        auto mappedTiles=[&] {
            const auto& chromeTile=host.entries_.at(19001)->render.prepared->tile;
            const auto& teamsTile=host.entries_.at(19002)->render.prepared->tile;
            require(chromeTile.chart && chromeTile.chart->semanticId=="google-chrome" &&
                chromeTile.baseArgb==0xfff4f0e6u && chromeTile.chart->appearance.flags==56 &&
                chromeTile.chart->argbCells[4]==0xffd8675bu && chromeTile.chart->argbCells[20]==0xff6ba776u &&
                chromeTile.chart->argbCells[100]==0xff4285f4u &&
                chromeTile.stitch==knitting::Stitch::Stockinette && chromeTile.material==knitting::Material::Raised,
                "Exact mapped executable reaches actual Host publication with literal Chrome palette and finishing");
            require(teamsTile.chart && teamsTile.chart->semanticId=="microsoft-teams" &&
                teamsTile.baseArgb==0xff9283c1u && teamsTile.chart->appearance.flags==48 &&
                teamsTile.chart->argbCells[0]==0xfff4efeeu && teamsTile.chart->argbCells[1]==0 &&
                teamsTile.stitch==knitting::Stitch::Stockinette && teamsTile.material==knitting::Material::Raised,
                "Exact packaged owner reaches actual Host publication with literal Teams palette and material");
        };
        mappedTiles();
        Options selectedOptions;
        selectedOptions.preview.push_back({19001,windows.at(19001).identity});
        const auto selectedConfig=MakeTrackerConfig(selectedOptions);
        Policy selectedPolicy;
        selectedPolicy.mode=selectedConfig.mode;
        selectedPolicy.selectedTargets=selectedConfig.selectedTargets;
        selectedPolicy.maxTargets=selectedConfig.maxTargets;
        TrackingModel selectedModel(selectedPolicy);
        auto suppressedSelection=windows.at(19001);
        suppressedSelection.minimized=true;
        require(selectedModel.Observe(suppressedSelection,Dirty::Lifecycle,1).empty() &&
            selectedModel.Current(19001).has_value(),
            "Host preview configuration discovers/seals suppressed selection before first Host Create");
        require(selectedModel.Observe(windows.at(19002),Dirty::Lifecycle,2).empty() && selectedModel.Size()==1,
            "Host preview policy rejects unselected owners before tracker target admission");
        selectedModel.Observe(DestroyedWindow{19001},Dirty::Lifecycle,3);
        suppressedSelection.minimized=false;
        require(selectedModel.Observe(suppressedSelection,Dirty::Lifecycle,4,4).empty() &&
            !selectedModel.Current(19001),
            "Destroyed initially suppressed preview cannot authorize same-HWND/process/fingerprint restored replacement");
        auto updateAll=[&](contracts::Configuration snapshot,bool wait) {
            snapshot.revision=host.committed_.revision+1;
            host.ApplyConfiguration(snapshot);
            for (auto handle:{WindowHandle{10001},WindowHandle{19001},WindowHandle{19002},WindowHandle{19003}})
                require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(windows.at(handle),Dirty::Appearance,GetTickCount64()))),
                    "All current private owner fixtures observe one committed appearance transaction");
            if (wait) pump(prepared);
        };
        auto manual=host.committed_;
        manual.design_selection=contracts::DesignSelection::plain;
        manual.yarn_argb=0xff123456u;
        manual.stitch_style=contracts::StitchStyle::rib;
        manual.material_style=contracts::MaterialStyle::plain;
        updateAll(manual,true);
        for (auto handle:{WindowHandle{19001},WindowHandle{19002},WindowHandle{19003}}) {
            const auto& tile=host.entries_.at(handle)->render.prepared->tile;
            require(!tile.chart && tile.baseArgb==0xff123456u && tile.stitch==knitting::Stitch::Rib &&
                tile.material==knitting::Material::Plain && tile.gauge.rows==3.5f,
                "Manual plain overrides differing owners without modifying persisted yarn/stitch/material/gauge");
        }
        manual=host.committed_;
        manual.design_selection=contracts::DesignSelection::original;
        manual.design_id="messages";
        updateAll(manual,true);
        for (auto handle:{WindowHandle{19001},WindowHandle{19002},WindowHandle{19003}}) {
            const auto& tile=host.entries_.at(handle)->render.prepared->tile;
            require(tile.chart && tile.chart->semanticId=="messages" && tile.chart->appearance.flags==36 &&
                tile.baseArgb==0xff123456u && tile.material==knitting::Material::Plain &&
                tile.stitch==knitting::Stitch::Stockinette,
                "Manual authored selection retains saved base yarn and its own non-sculpted finishing across owners");
        }
        manual=host.committed_; manual.design_selection=contracts::DesignSelection::plain; manual.design_id.clear();
        updateAll(manual,false);
        const auto queuedManual=*host.entries_.at(19001)->render.pending;
        auto automatic=host.committed_; automatic.design_selection=contracts::DesignSelection::by_app;
        updateAll(automatic,true);
        require(host.entries_.at(19001)->render.prepared->configRevision!=queuedManual.configRevision &&
            host.committed_.stitch_style==contracts::StitchStyle::rib &&
            host.committed_.material_style==contracts::MaterialStyle::plain && host.committed_.yarn_argb==0xff123456u,
            "Mode change while real work is queued rejects old mode without rewriting saved manual controls");
        mappedTiles();
        const auto stable=*host.entries_.at(19003)->render.prepared;
        require(!stable.tile.chart && stable.tile.baseArgb==0xff7c9885u &&
            stable.tile.stitch==knitting::Stitch::Rib && stable.tile.material==knitting::Material::Plain,
            "Unmatched ByApp uses exact stable fallback with saved plain stitch/material");
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(DestroyedWindow{19003},Dirty::Lifecycle,GetTickCount64()))),
            "Retire unknown owner lifetime before relaunch");
        windows.at(19003).identity.processStart=5678;
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(windows.at(19003),Dirty::Lifecycle,GetTickCount64()))),
            "Recreate unknown executable with new process birth");
        host.entries_.at(19003)->metadata=appMetadata(L"c:\\TOOLS\\unknown.EXE");
        host.PlaceAndShow(host.entries_.at(19003)->state.target); pump(prepared);
        require(host.entries_.at(19003)->render.prepared->tile.baseArgb==0xff7c9885u &&
            host.entries_.at(19003)->state.target.generation!=stable.token.target.generation,
            "Relaunched unknown identity keeps exact yarn while old HWND/process epoch stays retired");
        const auto unrelated=*host.entries_.at(19002)->render.prepared;
        const auto unrelatedEpoch=host.entries_.at(19002)->renderEpoch;
        const auto resolutionsBefore=host.counters_.appearanceResolutions;
        auto observationOnly=host.committed_; ++observationOnly.revision;
        host.ApplyConfiguration(observationOnly);
        require(host.counters_.appearanceResolutions==resolutionsBefore &&
            host.entries_.at(19002)->renderEpoch==unrelatedEpoch &&
            host::SamePreparation(*host.entries_.at(19002)->render.prepared,unrelated),
            "Observation/eligibility-only revision does not recolor or retire unrelated prepared inputs");

        host.metadataWorker_=std::make_unique<applications::MetadataWorker>(applications::WorkerLimits{},
            [&](applications::TargetToken const& token) -> applications::MetadataResult {
                return token.Target().hwnd==19001 || token.Target().hwnd==19002 ? *teamsOwner : *unknownOwner;
            });
        struct WorkerStop {
            Host& host;
            ~WorkerStop() {
                if (host.metadataWorker_) {
                    for (const auto& request:host.metadataWorker_->Stop()) host.metadataGate_.Retire(request);
                    host.metadataWorker_.reset();
                }
            }
        } workerStop{host};
        auto& changing=*host.entries_.at(19001);
        host.AdvanceRenderEpoch(changing); host.Hide(changing);
        require(SUCCEEDED(host.PlaceAndShow(changing.state.target)) && changing.render.pending.has_value(),
            "Mapped owner has an actual asynchronous Chrome preparation before owner metadata changes");
        const auto obsolete=*changing.render.pending;
        const auto uploadsBefore=host.counters_.uploads;
        changing.nextMetadataQuery=0;
        host.PollMetadata();
        const auto deadline=GetTickCount64()+10000;
        while ((!changing.metadata || std::get<applications::AppMetadata>(*changing.metadata).Identity()!=
            std::get<applications::AppMetadata>(*teamsOwner).Identity()) && GetTickCount64()<deadline) {
            Sleep(5); host.PollMetadata();
        }
        require(changing.renderEpoch>obsolete.token.appearanceRevision &&
            std::get<applications::AppMetadata>(*changing.metadata).Identity()==std::get<applications::AppMetadata>(*teamsOwner).Identity() &&
            host.entries_.at(19002)->renderEpoch==unrelatedEpoch &&
            host::SamePreparation(*host.entries_.at(19002)->render.prepared,unrelated),
            "Accepted same-lifetime async owner change retires only affected target, not unrelated owner");
        pump(prepared);
        require(changing.render.prepared->tile.chart->semanticId=="microsoft-teams" &&
            changing.render.prepared->tile.baseArgb==0xff9283c1u && host.counters_.uploads==uploadsBefore+1,
            "Late old-owner job cannot upload; current accepted owner alone publishes its exact authored theme");
        const auto refreshed=*changing.render.prepared;
        const auto stableEpoch=changing.renderEpoch;
        const auto stableResolutions=host.counters_.appearanceResolutions;
        const auto stableRequests=host.queue_->Status().renderRequests;
        changing.nextMetadataQuery=0;
        host.PollMetadata();
        while (changing.metadataPending && GetTickCount64()<deadline) { Sleep(5); host.PollMetadata(); }
        require(!changing.metadataPending && changing.renderEpoch==stableEpoch &&
            host.counters_.appearanceResolutions==stableResolutions && host.queue_->Status().renderRequests==stableRequests &&
            host::SamePreparation(*changing.render.prepared,refreshed),
            "Same-identity metadata refresh reuses its effective appearance and immutable preparation without churn");
        for (const auto& request:host.metadataWorker_->Stop()) host.metadataGate_.Retire(request);
        host.metadataWorker_.reset();
        changing.appearance.reset(); host.AdvanceRenderEpoch(changing); host.Hide(changing);
        contract.nextAppearanceFailure=colorways::ResolutionError::UnknownChart;
        const auto errorsBefore=host.counters_.appearanceErrors;
        require(host.PlaceAndShow(changing.state.target)==S_FALSE && changing.appearance &&
            std::get<AppearanceFailure>(changing.appearance->result).resolution==colorways::ResolutionError::UnknownChart &&
            !changing.render.pending && !changing.presentationVisible,
            "Typed resolver failure is retained and suppressed, never a successful plain/null appearance");
        for (int retry=0;retry<4;++retry) host.PlaceAndShow(changing.state.target);
        require(host.counters_.appearanceErrors==errorsBefore+1 &&
            host.entries_.at(19002)->renderEpoch==unrelatedEpoch,
            "Permanent resolution failure diagnostics/retries are bounded to the effective owner/config binding");
        changing.metadata=chromeOwner;
        host.PlaceAndShow(changing.state.target); pump(prepared);
        require(changing.render.prepared->tile.chart->semanticId=="google-chrome",
            "New accepted owner binding recovers a prior resolution failure without stale fallback");
        for (auto handle:{WindowHandle{19001},WindowHandle{19002},WindowHandle{19003}}) {
            host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()));
            windows.erase(handle);
        }
        appearanceChange(previousConfiguration);
        std::puts("PASS: actual ByApp exe/package authored palettes, stable unknown relaunch, manual override/mode races, target-only async owner epochs and bounded typed resolution failures");
    }
    {
        const auto previousConfiguration=host.committed_;
        auto iconConfiguration=previousConfiguration;
        iconConfiguration.border_width_dip=16.f;
        appearanceChange(iconConfiguration);
        wchar_t self[32768]{};
        require(GetModuleFileNameW(nullptr,self,32768)!=0,"Native icon consumer locates its sibling fixture");
        auto iconFixture=std::filesystem::path(self).parent_path()/L"IconYarnTests.exe";
        if (!std::filesystem::is_regular_file(iconFixture)) {
#ifdef NDEBUG
            iconFixture=std::filesystem::current_path()/L"tests"/L"IconYarn"/L"build"/L"x64"/L"Release"/L"IconYarnTests.exe";
#else
            iconFixture=std::filesystem::current_path()/L"tests"/L"IconYarn"/L"build"/L"x64"/L"Debug"/L"IconYarnTests.exe";
#endif
        }
        require(std::filesystem::is_regular_file(iconFixture),
            "Build IconYarnTests before Host contracts: native PE fixture is required, not an optional model pass");
        add(19501); add(19502);
        const auto nativeOwner=appMetadata(iconFixture.wstring());
        const auto mappedOwner=appMetadata(host.installRoots_.program_files+L"\\Google\\Chrome\\Application\\chrome.exe");
        host.entries_.at(19501)->metadata=nativeOwner;
        host.entries_.at(19502)->metadata=mappedOwner;
        for (const auto handle:{19501,19502}) host.PlaceAndShow(host.entries_.at(handle)->state.target);
        pump(prepared);
        const auto mappedEpoch=host.entries_.at(19502)->renderEpoch;
        const auto mappedPreparation=*host.entries_.at(19502)->render.prepared;
        host.iconWorker_=std::make_unique<icon_yarn::PreparationWorker>(icon_yarn::SampleExecutableIcon);
        struct StopIcons {
            Host& host;
            ~StopIcons() {
                if (host.iconWorker_) { host.iconWorker_->Stop(); host.iconWorker_.reset(); }
                host.iconGate_.Clear();
            }
        } stopIcons{host};
        auto pollUntil=[&](const auto& predicate) {
            const auto deadline=GetTickCount64()+10000;
            while (!predicate() && GetTickCount64()<deadline) { host.PollIcons(); Sleep(5); }
            require(predicate(),"Actual Host icon consumer reaches bounded completion");
        };
        pollUntil([&]{return host.entries_.at(19501)->iconPalette!=nullptr;});
        pump(prepared);
        const auto& native=*host.entries_.at(19501);
        require(std::get<icon_yarn::SampledPalette>(*native.iconPalette)==
            icon_yarn::SampledPalette{0xffc04b4bu,0xfff6f0deu} &&
            native.render.prepared->tile.baseArgb==0xffc04b4bu && native.render.prepared->tile.chart &&
            native.render.prepared->tile.chart->semanticId.starts_with("automatic-"),
            "Actual Host bounded PE resource/WIC worker proposal reaches native generated tile and GPU publication");
        require(host.entries_.at(19502)->renderEpoch==mappedEpoch &&
            host::SamePreparation(*host.entries_.at(19502)->render.prepared,mappedPreparation) &&
            !host.entries_.at(19502)->iconInput && !host.entries_.at(19502)->iconPending,
            "By App mapped authored owner is never sampled or invalidated by an unrelated native icon completion");
        const auto nativeSalt=native.iconInput->canonicalAppIdentity;
        const auto nativeQueryCount=host.iconWorker_->Statistics().queries;
        const auto nativeGeneration=native.state.target.generation;
        host.ApplyBatch(contract.model.Observe(DestroyedWindow{19501},Dirty::Lifecycle,GetTickCount64()));
        windows.at(19501).identity.processStart=9876;
        host.ApplyBatch(contract.model.Observe(windows.at(19501),Dirty::Lifecycle,GetTickCount64()));
        host.entries_.at(19501)->metadata=nativeOwner;
        host.PlaceAndShow(host.entries_.at(19501)->state.target);
        pollUntil([&]{return host.entries_.at(19501)->iconPalette!=nullptr;}); pump(prepared);
        require(host.entries_.at(19501)->state.target.generation!=nativeGeneration &&
            host.entries_.at(19501)->iconInput->canonicalAppIdentity==nativeSalt &&
            host.entries_.at(19501)->render.prepared->tile.baseArgb==0xffc04b4bu &&
            host.iconWorker_->Statistics().queries==nativeQueryCount && host.iconWorker_->Statistics().cacheHits>0,
            "Native same-executable relaunch has a new target birth, identical salt/pixels and cached sampled colors");
        host.iconWorker_->Stop(); host.iconWorker_.reset(); host.iconGate_.Clear();
        {
            icon_yarn::PreparationLimits limits;
            limits.pendingBytes=1;
            host.iconWorker_=std::make_unique<icon_yarn::PreparationWorker>(
                [](const icon_yarn::IconInput&,std::stop_token,std::chrono::steady_clock::time_point) ->
                    icon_yarn::PaletteResult { return icon_yarn::SampledPalette{0xff000000u,0xffffffffu}; },limits);
            add(19503);
            host.entries_.at(19503)->metadata=appMetadata(L"C:\\Private\\FullAdmission.exe");
            host.PlaceAndShow(host.entries_.at(19503)->state.target); pump(prepared);
            const auto stable=*host.entries_.at(19503)->render.prepared;
            for (unsigned retry=0;retry<8;++retry) {
                host.entries_.at(19503)->nextIconRetry=0;
                host.PollIcons();
            }
            require(host.entries_.at(19503)->iconAttempts==4 && !host.entries_.at(19503)->iconPending &&
                std::get<icon_yarn::Fallback>(*host.entries_.at(19503)->iconPalette).reason==icon_yarn::FallbackReason::ResourceLimit &&
                !host.iconGate_.Size() && !host.iconWorker_->Statistics().queries &&
                host::SamePreparation(*host.entries_.at(19503)->render.prepared,stable),
                "Actual Host Full admission explicitly retires four bounded retries and retains unchanged provisional pixels, never a false palette");
            host.iconWorker_->Stop();
            add(19504);
            host.entries_.at(19504)->metadata=appMetadata(L"C:\\Private\\StoppedAdmission.exe");
            host.PlaceAndShow(host.entries_.at(19504)->state.target); pump(prepared);
            host.PollIcons();
            require(host.entries_.at(19504)->iconPalette && !host.entries_.at(19504)->iconPending &&
                std::holds_alternative<icon_yarn::Fallback>(*host.entries_.at(19504)->iconPalette) &&
                !host.iconGate_.Size() && !host.iconWorker_->Statistics().queries,
                "Actual Host Stopped admission retires its gate and publishes only typed unavailable sampling state");
            host.iconWorker_.reset();
            for (const auto handle:{WindowHandle{19503},WindowHandle{19504}}) {
                host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()));
                windows.erase(handle);
            }
        }
        struct IconLatch {
            std::mutex mutex;
            std::condition_variable wake;
            bool entered{}, released{};
            void Block(std::stop_token stop) {
                std::unique_lock lock(mutex); entered=true; wake.notify_all();
                std::stop_callback callback(stop,[&]{wake.notify_all();});
                wake.wait(lock,[&]{return released || stop.stop_requested();});
            }
            bool Wait() {
                std::unique_lock lock(mutex);
                return wake.wait_for(lock,std::chrono::seconds(10),[&]{return entered;});
            }
            void Release() { std::lock_guard lock(mutex); released=true; wake.notify_all(); }
        };
        auto modeAll=[&](contracts::DesignSelection mode) {
            auto snapshot=host.committed_; ++snapshot.revision;
            snapshot.design_selection=mode; snapshot.design_id.clear();
            host.ApplyConfiguration(snapshot);
            for (const auto& [handle,entry]:host.entries_) {
                (void)entry;
                host.ApplyBatch(contract.model.Observe(windows.at(handle),Dirty::Appearance,GetTickCount64()));
            }
        };
        for (unsigned scenario=0;scenario<4;++scenario) {
            IconLatch latch;
            icon_yarn::PreparationLimits limits; limits.cacheEntries=1;
            host.iconWorker_=std::make_unique<icon_yarn::PreparationWorker>(
                [&](const icon_yarn::IconInput& input,std::stop_token stop,
                    std::chrono::steady_clock::time_point) -> icon_yarn::PaletteResult {
                    if (input.executablePath.find(L"Slow")!=std::wstring::npos) latch.Block(stop);
                    if (stop.stop_requested()) return icon_yarn::Fallback{icon_yarn::FallbackReason::Cancelled};
                    return input.executablePath.find(L"Slow")!=std::wstring::npos ?
                        icon_yarn::SampledPalette{0xff719d79u,0xfff6f0deu} :
                        icon_yarn::SampledPalette{0xff769cc0u,0xfff6f0deu};
                },limits);
            struct ReleaseIcons {
                IconLatch& latch;
                Host& host;
                ~ReleaseIcons() {
                    latch.Release();
                    if (host.iconWorker_) { host.iconWorker_->Stop(); host.iconWorker_.reset(); }
                    host.iconGate_.Clear();
                }
            } releaseIcons{latch,host};
            modeAll(contracts::DesignSelection::shared_zigzag);
            add(19510); add(19511);
            const auto slowOwner=appMetadata(L"C:\\Private\\Slow"+std::to_wstring(scenario)+L".exe");
            const auto fastOwner=appMetadata(L"C:\\Private\\Fast"+std::to_wstring(scenario)+L".exe");
            host.entries_.at(19510)->metadata=slowOwner;
            host.entries_.at(19511)->metadata=fastOwner;
            host.PlaceAndShow(host.entries_.at(19510)->state.target);
            host.PlaceAndShow(host.entries_.at(19511)->state.target);
            pump(prepared);
            host.PollIcons();
            require(latch.Wait(),"Actual icon worker reaches delayed owner source");
            pollUntil([&]{return host.entries_.at(19511)->iconPalette!=nullptr;});
            pump(prepared);
            require(host.entries_.at(19510)->iconPending && !host.entries_.at(19510)->iconPalette &&
                host.entries_.at(19511)->render.prepared->tile.baseArgb==0xff769cc0u &&
                host.entries_.at(19511)->render.prepared->tile.chart->semanticId=="automatic-zigzag" &&
                host.entries_.at(19501)->render.prepared->tile.baseArgb==0xffc04b4bu,
                "Out-of-order owner completion publishes only its per-app shared Zigzag; slow source keeps provisional yarn");
            if (scenario==0)
                require(host.entries_.at(19502)->render.prepared->tile.baseArgb==0xff769cc0u &&
                    host.entries_.at(19502)->render.prepared->tile.chart->semanticId=="automatic-zigzag",
                    "Shared Zigzag samples mapped app color instead of retaining its authored By App palette");
            const auto slowEpoch=host.entries_.at(19510)->renderEpoch;
            const auto recolors=host.counters_.iconRecolors;
            const auto stableQueries=host.iconWorker_->Statistics().queries;
            if (scenario==0) {
                const auto pendingEpoch=host.entries_.at(19510)->iconEpoch;
                applications::EligibilityMutation unrelated;
                unrelated.edits.emplace_back(applications::SetApplicationPreference{
                    *applications::AppIdentity::FromExecutablePath(L"C:\\Private\\SavedNonRunning.exe"),false});
                auto observation=host.committed_; ++observation.revision;
                observation.eligibility=std::get<applications::EligibilityPreferences>(observation.eligibility.Apply(unrelated));
                host.ApplyConfiguration(observation);
                require(host.entries_.at(19510)->iconPending && host.entries_.at(19510)->iconEpoch==pendingEpoch &&
                    host.iconWorker_->Statistics().queries==stableQueries,
                    "Unrelated saved eligibility revision does not invalidate or resubmit an in-flight owner's icon work");
                applications::EligibilityMutation disable;
                disable.edits.emplace_back(applications::SetApplicationPreference{
                    std::get<applications::AppMetadata>(*slowOwner).Identity(),false});
                auto snapshot=host.committed_; ++snapshot.revision;
                snapshot.eligibility=std::get<applications::EligibilityPreferences>(snapshot.eligibility.Apply(disable));
                host.ApplyConfiguration(snapshot);
            } else if (scenario==1) {
                host.ApplyBatch(contract.model.Observe(DestroyedWindow{19510},Dirty::Lifecycle,GetTickCount64()));
            } else if (scenario==2) {
                modeAll(contracts::DesignSelection::plain);
            } else {
                const auto oldGeneration=host.recovery_.Generation();
                require(host.GpuFailure(L"icon consumer device retirement",DXGI_ERROR_DEVICE_RESET,oldGeneration)==S_FALSE &&
                    host.recovery_.Generation()>oldGeneration,
                    "Actual graphics failure retires pending icon device generation before recovery");
            }
            latch.Release();
            pollUntil([&]{return host.iconWorker_->Statistics().pendingJobs==0;});
            host.PollIcons();
            require(host.counters_.iconRecolors==recolors &&
                (!host.entries_.contains(19510) || !host.entries_.at(19510)->iconPalette ||
                    !std::holds_alternative<icon_yarn::SampledPalette>(*host.entries_.at(19510)->iconPalette)),
                "Late icon after exclusion/destruction/manual mode/device retirement cannot recolor or resurrect owner decoration");
            if (scenario==0)
                require(!host.entries_.at(19510)->presentationVisible &&
                    host.entries_.at(19510)->renderEpoch>slowEpoch,
                    "Exclusion suppresses actual overlay and retires only affected owner's effective epoch");
            if (scenario==3) {
                host.ApplyBatch(contract.model.Observe(DestroyedWindow{19510},Dirty::Lifecycle,GetTickCount64()));
                if (host.recoveryRetry_) host.RecoveryTick(host.recoveryRetry_->due);
            }
            require(host.recovery_.CanRender(host.recovery_.Generation()),"Icon race fixture restores real shared graphics before later contracts");
            for (unsigned repeat=0;repeat<8;++repeat) host.PollIcons();
            require(host.iconWorker_->Statistics().queries==stableQueries &&
                host.iconWorker_->Statistics().cacheEntries<=1,
                "Positive/negative per-owner results survive cache pressure without eviction-resubmission feedback");
            for (const auto handle:{WindowHandle{19510},WindowHandle{19511}}) {
                if (host.entries_.contains(handle))
                    host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()));
                windows.erase(handle);
            }
        }
        for (const auto handle:{WindowHandle{19501},WindowHandle{19502}}) {
            host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()));
            windows.erase(handle);
        }
        appearanceChange(previousConfiguration);
        std::puts("PASS: actual hidden Host native PE/WIC-to-GPU icon yarn, cached relaunch, shared per-app colors, out-of-order completion, exclusion/destruction/mode/device fences and no eviction feedback; tracker lifetimes are scripted, not desktop verification");
    }
    {
        const auto previous=host.committed_;
        for (auto mode:{contracts::DesignSelection::plain,contracts::DesignSelection::original}) {
            const WindowHandle personalHandle=mode==contracts::DesignSelection::plain?19621:19623;
            const WindowHandle unrelatedHandle=personalHandle+1;
            auto manual=previous; manual.design_selection=mode;
            manual.design_id=mode==contracts::DesignSelection::original?"messages":"";
            manual.yarn_argb=0xff315273u;
            appearanceChange(manual);
            auto owner=appMetadata(L"C:\\PrivateFixtures\\cache-personal.exe");
            auto unrelatedOwner=appMetadata(L"C:\\PrivateFixtures\\cache-other.exe");
            add(personalHandle); add(unrelatedHandle);
            host.entries_.at(personalHandle)->metadata=owner; host.entries_.at(unrelatedHandle)->metadata=unrelatedOwner;
            host.PlaceAndShow(host.entries_.at(personalHandle)->state.target);
            host.PlaceAndShow(host.entries_.at(unrelatedHandle)->state.target); pump(prepared);
            const auto unchanged=*host.entries_.at(unrelatedHandle)->render.prepared;
            const auto epoch=host.entries_.at(unrelatedHandle)->renderEpoch;
            const auto overlay=host.entries_.at(unrelatedHandle)->overlay.Window();
            std::array metadata{std::get<applications::AppMetadata>(*owner)};
            auto candidate=configuration::PrepareImport(host.committed_.imports,
                std::make_shared<const imports::ImportCatalogue>(),
                contracts::SetRulesText{"cache-personal = #214365\n"},metadata);
            require(std::holds_alternative<configuration::ImportCandidate>(candidate),
                "First personal binding is planned for exact identity in global Plain/Original mode");
            auto imported=std::get<configuration::ImportCandidate>(std::move(candidate));
            auto snapshot=host.committed_; snapshot.imports=imported.preferences; ++snapshot.revision;
            host.ApplyConfiguration(snapshot,imported.catalogue);
            host.PlaceAndShow(host.entries_.at(personalHandle)->state.target); pump(prepared);
            const auto before=host.counters_;
            const auto queueBefore=host.queue_->Status();
            auto& unrelatedMove=windows.at(unrelatedHandle);
            unrelatedMove.rect.left+=9; unrelatedMove.rect.right+=9;
            contract.geometry.at(unrelatedHandle).physicalTargetBounds={unrelatedMove.rect.left,unrelatedMove.rect.top,
                unrelatedMove.rect.right,unrelatedMove.rect.bottom};
            require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(unrelatedMove,Dirty::Geometry,GetTickCount64()))),
                "Unrelated real Move follows first personal binding in global Plain/Original mode");
            host.entries_.at(unrelatedHandle)->metadata=appMetadata(L"c:\\PRIVATEFIXTURES\\cache-other.EXE");
            require(host.ResolveTargetAppearance(*host.entries_.at(unrelatedHandle)),
                "Unrelated same-identity metadata refresh retains cached appearance authority");
            snapshot=host.committed_; ++snapshot.revision; contract.pendingConfiguration=snapshot;
            require(host.ConfigurationCurrent() && host.ResolveTargetAppearance(*host.entries_.at(unrelatedHandle)),
                "Unrelated observation poll retains identity-aware cache after first binding");
            auto queueAfter=host.queue_->Status();
            require(host.entries_.at(unrelatedHandle)->renderEpoch==epoch && host.entries_.at(unrelatedHandle)->overlay.Window()==overlay &&
                host.entries_.at(unrelatedHandle)->presentationVisible && host.entries_.at(unrelatedHandle)->render.prepared &&
                host::SamePreparation(*host.entries_.at(unrelatedHandle)->render.prepared,unchanged) &&
                host.counters_.uploads==before.uploads &&
                host.counters_.appearanceResolutions==before.appearanceResolutions &&
                queueAfter.renderRequests==queueBefore.renderRequests && queueAfter.tileGenerations==queueBefore.tileGenerations,
                "First personal binding reconciles unchanged Plain/Original cache ownership without unrelated flicker, epoch or draw");
            for (auto handle:{personalHandle,unrelatedHandle}) {
                host.ApplyBatch(contract.model.Observe(DestroyedWindow{handle},Dirty::Lifecycle,GetTickCount64()));
                windows.erase(handle);
            }
            snapshot=manual; snapshot.revision=host.committed_.revision+1;
            host.ApplyConfiguration(snapshot,std::make_shared<const imports::ImportCatalogue>()); pump(prepared);
        }
        appearanceChange(previous);
    }
    {
        const auto beforeImports=host.committed_;
        auto owner=appMetadata(L"C:\\PrivateFixtures\\Imports\\personal.exe");
        auto other=appMetadata(L"C:\\PrivateFixtures\\Imports\\other.exe");
        add(19601); add(19602);
        host.entries_.at(19601)->metadata=owner; host.entries_.at(19602)->metadata=other;
        host.PlaceAndShow(host.entries_.at(19601)->state.target);
        host.PlaceAndShow(host.entries_.at(19602)->state.target); pump(prepared);
        const auto unchanged=*host.entries_.at(19602)->render.prepared;
        const auto unchangedEpoch=host.entries_.at(19602)->renderEpoch;
        const auto unchangedWindow=host.entries_.at(19602)->overlay.Window();
        const auto scratch=std::filesystem::current_path()/L"out"/L"issue11"/L"consumer-imports";
        std::filesystem::create_directories(scratch);
        auto path=scratch/L"literal.png";
        struct Cleanup { std::filesystem::path path; ~Cleanup() {std::error_code error; std::filesystem::remove_all(path,error);} } cleanup{scratch};
        auto read=[&](std::uint32_t argb) {
            knitting::RasterImage image{2,2,8,{argb,0xff37d0a6u,0,argb}};
            knitting::SavePng(image,path.c_str());
            auto imported=imports::ImportPngFile(path.native());
            require(std::holds_alternative<imports::FileImport>(imported),"Hidden actual Host fixture validates PNG through pinned local reader");
            return std::get<imports::FileImport>(std::move(imported)).chart;
        };
        configuration::CatalogueSnapshot catalogue=std::make_shared<const imports::ImportCatalogue>();
        auto built=imports::BuildCandidate(*catalogue,imports::AddChart{"consumer-literal",read(0xffe02152u)});
        require(std::holds_alternative<imports::ImportCatalogue>(built),"Hidden Host owns validated imported catalogue candidate");
        catalogue=std::make_shared<const imports::ImportCatalogue>(std::get<imports::ImportCatalogue>(std::move(built)));
        auto importedConfig=beforeImports;
        configuration::DescribeCatalogue(importedConfig.imports,*catalogue);
        std::array metadata{std::get<applications::AppMetadata>(*owner)};
        auto rules=configuration::PrepareImport(importedConfig.imports,catalogue,
            contracts::SetRulesText{"personal = #123456 consumer-literal\n"},metadata);
        require(std::holds_alternative<configuration::ImportCandidate>(rules),"Hidden Host import planner binds exact metadata identity");
        auto candidate=std::get<configuration::ImportCandidate>(std::move(rules));
        catalogue=candidate.catalogue; importedConfig.imports=candidate.preferences;
        importedConfig.revision=host.committed_.revision+1;
        host.ApplyConfiguration(importedConfig,catalogue);
        require(host.entries_.at(19602)->renderEpoch==unchangedEpoch &&
            host.entries_.at(19602)->overlay.Window()==unchangedWindow &&
            host::SamePreparation(*host.entries_.at(19602)->render.prepared,unchanged),
            "Imported per-identity rule invalidates only affected actual entries, not unrelated tiles or overlay");
        host.PlaceAndShow(host.entries_.at(19601)->state.target); pump(prepared);
        const auto id=catalogue->Charts()[0].Id();
        const auto& first=host.entries_.at(19601)->render.prepared->tile;
        require(first.baseArgb==0xff123456u && first.chart && first.chart->semanticId==id &&
            first.chart->contentRevision==1 && first.chart->argbCells==
                std::vector<std::uint32_t>{0xffe02152u,0xff37d0a6u,0,0xffe02152u},
            "Actual hidden Host GPU publication uses personal base plus exact imported cells and alpha sentinel");
        auto replace=[&](std::uint32_t color) {
            auto changed=imports::BuildCandidate(*catalogue,imports::ReplaceChart{id,read(color),{}});
            require(std::holds_alternative<imports::ImportCatalogue>(changed),"Hidden Host replacement candidate preserves stable ID");
            catalogue=std::make_shared<const imports::ImportCatalogue>(std::get<imports::ImportCatalogue>(std::move(changed)));
            auto snapshot=host.committed_; configuration::DescribeCatalogue(snapshot.imports,*catalogue);
            ++snapshot.revision; host.ApplyConfiguration(snapshot,catalogue);
            host.PlaceAndShow(host.entries_.at(19601)->state.target);
        };
        replace(0xff1357a9u);
        require(host.entries_.at(19601)->render.pending.has_value(),"Replacement queues actual async imported GPU preparation");
        auto old=*host.entries_.at(19601)->render.pending;
        replace(0xffa95173u); pump(prepared);
        auto const& current=host.entries_.at(19601)->render.prepared;
        require(current->token.appearanceRevision!=old.token.appearanceRevision &&
            current->tile.chart->contentRevision==3 && current->tile.chart->argbCells[0]==0xffa95173u &&
            host.entries_.at(19602)->renderEpoch==unchangedEpoch &&
            host::SamePreparation(*host.entries_.at(19602)->render.prepared,unchanged),
            "Per-asset revision fences reject in-flight older PNG replacement and preserve unrelated actual preparation");
        host.ApplyBatch(contract.model.Observe(DestroyedWindow{19601},Dirty::Lifecycle,GetTickCount64()));
        auto late=host.committed_; ++late.revision; host.ApplyConfiguration(late,catalogue);
        require(!host.entries_.contains(19601),"Late imported catalogue observation cannot recreate a destroyed owner");
        host.ApplyBatch(contract.model.Observe(DestroyedWindow{19602},Dirty::Lifecycle,GetTickCount64()));
        windows.erase(19601); windows.erase(19602);
        auto reset=beforeImports; reset.revision=host.committed_.revision+1;
        host.ApplyConfiguration(reset,std::make_shared<const imports::ImportCatalogue>());
        pump(prepared);
        std::puts("PASS: actual hidden Host personal PNG generation, literal RGB, alpha sentinel, targeted identity/asset invalidation and in-flight replacement/destruction fences");
    }
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
        host.entries_.at(10001)->overlay.Bounds().left == 124,
        "Pumping desktop probe moves to fresh B placement, never pre-query A");
    {
        auto& window = windows.at(10001);
        const auto offset = host.entries_.at(10001)->overlay.Bounds().left - window.rect.left;
        auto shift = [&](LONG dx) {
            window.rect.left += dx; window.rect.right += dx;
            contract.geometry.at(10001).physicalTargetBounds.left += dx;
            contract.geometry.at(10001).physicalTargetBounds.right += dx;
        };
        contract.duringVisibilityProbe = [&] {
            shift(30);
            host.ApplyBatch(contract.model.Observe(window, Dirty::Geometry, GetTickCount64()));
        };
        shift(15);
        require(SUCCEEDED(host.ApplyBatch(contract.model.Observe(window, Dirty::Geometry, GetTickCount64()))) &&
            !contract.duringVisibilityProbe && host.entries_.at(10001)->overlay.Bounds().left == window.rect.left + offset,
            "Batch pure Move reads geometry only after its pumping probe, never the pre-probe position");
    }
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
    auto unshow = [&] {
        contract.offDesktop = 10001;
        require(!host.VisibilityAllows(host.entries_.at(10001)->state.target) &&
            !host.entries_.at(10001)->visibility.Shown() && !host.entries_.at(10001)->presentationVisible,
            "Off-desktop probe retires shown visibility before poll reactivation");
        contract.offDesktop.reset();
        host.visibilityPoll_ = {};
    };
    unshow();
    {
        const auto before = host.counters_;
        host.PollVisibility();
        require(host.entries_.size() == 1 && host.entries_.at(10001)->presentationVisible &&
            host.counters_.visibilityObservations == before.visibilityObservations + 1 &&
            host.counters_.workValidations == before.workValidations + 1 &&
            host.counters_.geometryQueries == before.geometryQueries + 1 && host.counters_.uploads == before.uploads,
            "Poll reactivation reuses its accepted probe; post-probe validation and one geometry read remain");
    }
    for (const bool tracked : {false, true}) {
        unshow();
        contract.duringVisibilityProbe = [&] {
            windows.at(10001).rect.right += 10;
            contract.geometry.at(10001).physicalTargetBounds.right += 10;
            if (tracked) host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Geometry, GetTickCount64()));
        };
        const auto uploads = host.counters_.uploads;
        host.PollVisibility();
        require(!contract.duringVisibilityProbe && !host.entries_.at(10001)->presentationVisible &&
            host.counters_.uploads == uploads, "Poll reactivation reads geometry after its pumping probe; resize never shows old pieces");
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Geometry, GetTickCount64()));
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Settlement, GetTickCount64() + 300));
        host.PlaceAndShow(publicationTarget);
        pump(prepared);
    }
    for (const auto kind : {ActionKind::Show, ActionKind::Reorder}) {
        const auto before = host.counters_;
        const auto& shown = *host.entries_.at(10001);
        require(SUCCEEDED(host.ApplyBatch({{kind, shown.state.target, shown.state.pendingRect, shown.state.dpi,
            shown.state.appearanceRevision}})) && host.entries_.at(10001)->presentationVisible &&
            host.counters_.workValidations == before.workValidations + 2 &&
            host.counters_.visibilityObservations == before.visibilityObservations + 1 &&
            host.counters_.geometryQueries == before.geometryQueries + 1 && host.counters_.uploads == before.uploads,
            "Prepared Show/Reorder shares one probe and one geometry read; outer and post-probe validation remain");
    }
    contract.duringVisibilityProbe = [&] {
        auto changed = host.committed_;
        ++changed.revision;
        changed.border_width_dip = 28;
        host.ApplyConfiguration(changed);
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Appearance, GetTickCount64()));
    };
    require(host.PlaceAndShow(publicationTarget) == S_FALSE && !host.entries_.at(10001)->presentationVisible,
        "Pumping shared desktop probe rejects old prepared configuration before Show");
    pump(prepared);
    contract.duringVisibilityProbe = [&] {
        windows.at(10001).rect.right += 10;
        contract.geometry.at(10001).physicalTargetBounds.right += 10;
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Geometry, GetTickCount64()));
    };
    require(host.PlaceAndShow(publicationTarget) == S_FALSE && !host.entries_.at(10001)->presentationVisible,
        "Pumping shared desktop probe rejects same-lifetime resize before Show");
    host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Settlement, GetTickCount64() + 300));
    pump(prepared);
    auto resubmit = [&] {
        host.entries_.at(10001)->render.prepared.reset();
        require(host.Prepare(host.entries_.at(10001)->state.target) == S_FALSE &&
            host.entries_.at(10001)->render.pending.has_value(), "Submit actual worker input before a pumped drain");
    };
    auto drainUntilProbe = [&] {
        const auto deadline = GetTickCount64() + 30000;
        while (contract.duringVisibilityProbe && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            if (PeekMessageW(&message, host.window_, window_sweaters::rendering::RenderQueue::ReadyMessage,
                window_sweaters::rendering::RenderQueue::ReadyMessage, PM_REMOVE)) DispatchMessageW(&message);
        }
    };
    resubmit();
    contract.duringVisibilityProbe = [&] {
        windows.at(10001).rect.right += 10;
        contract.geometry.at(10001).physicalTargetBounds.right += 10;
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Geometry, GetTickCount64()));
    };
    {
        const auto before = host.counters_;
        drainUntilProbe();
        require(!contract.duringVisibilityProbe && !host.entries_.at(10001)->presentationVisible &&
            host.counters_.uploads == before.uploads && host.counters_.staleResults == before.staleResults + 1,
            "Drained result reads geometry after its pumping probe; resize never uploads or shows old input");
    }
    host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Settlement, GetTickCount64() + 300));
    host.PlaceAndShow(publicationTarget);
    pump(prepared);
    for (const auto consumer : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        const bool reuse = consumer == 3 || (consumer > 3 && consumer % 2);
        const auto lifetime = host.entries_.at(10001)->state.target;
        if (consumer == 6 || consumer == 7) unshow();
        if (consumer >= 8) resubmit();
        contract.duringVisibilityProbe = [&] {
            host.ApplyBatch(contract.model.Observe(DestroyedWindow{10001}, Dirty::Lifecycle, GetTickCount64()));
            if (reuse) host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Lifecycle, GetTickCount64()));
        };
        HRESULT result = S_FALSE;
        if (consumer == 0) result = host.Prepare(lifetime);
        else if (consumer == 1) result = host.MovePrepared(lifetime);
        else if (consumer <= 3) result = host.PlaceAndShow(lifetime);
        else if (consumer <= 5) {
            auto& window = windows.at(10001);
            window.rect.left += 5; window.rect.right += 5;
            contract.geometry.at(10001).physicalTargetBounds.left += 5;
            contract.geometry.at(10001).physicalTargetBounds.right += 5;
            if (FAILED(host.ApplyBatch(contract.model.Observe(window, Dirty::Geometry, GetTickCount64())))) result = E_FAIL;
        } else if (consumer <= 7) host.PollVisibility();
        else drainUntilProbe();
        require(result == S_FALSE && !contract.duringVisibilityProbe && !host.Find(lifetime) && (!reuse ||
            (host.entries_.at(10001)->state.target != lifetime && !host.entries_.at(10001)->presentationVisible)),
            "Actual Prepare/Move/Show/batch Move/poll/drain consumer rejects pumped Remove or HWND reuse without retained Entry/TargetId references");
        host.ApplyBatch(contract.model.Observe(windows.at(10001), Dirty::Lifecycle, GetTickCount64()));
        host.entries_.at(10001)->metadata=appA;
        host.PlaceAndShow(host.entries_.at(10001)->state.target);
        pump(prepared);
    }

    using rendering::GraphicsRecovery;
    using rendering::RecoveryState;
    const auto recoveredTarget = host.entries_.at(10001)->state.target;
    const auto obsoleteRequest = *host.entries_.at(10001)->render.prepared;
    const auto cacheBeforeRecovery = host.queue_->Status();
    const auto recoveryAttemptsBeforeFault = host.recoveryAttempts_;
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
    require(host.recovery_.Generation() == faultGeneration && host.recoveryAttempts_ == recoveryAttemptsBeforeFault,
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
    require(host.recovery_.State() == RecoveryState::Ready && host.recoveryAttempts_ == recoveryAttemptsBeforeFault+3 &&
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
        if (options.inspectFixture) return InspectFixture(options.inspectFixture);
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
        if (options.inspectPreview) return InspectPreview(options.inspectPreview);
        for (const auto& selected:options.preview) {
            const auto current=ObservePreviewSelection(selected.hwnd);
            if (!current || current->identity!=selected.identity) {
                LogError(L"select exact captured preview window lifetime", E_INVALIDARG);
                return 3;
            }
        }
        if (options.target) {
            FixtureSelectionDiagnostics diagnostic;
            options.fixtureIdentity = SelectFixture(options.target,&diagnostic);
            if (!options.fixtureIdentity) {
                const auto text=FixtureSelectionJson(reinterpret_cast<WindowHandle>(options.target),false,diagnostic);
                std::fprintf(stderr,"%s",text.c_str());
                if (options.testMode && !options.probeOutput.empty() && BindFixtureStorage(options)) {
                    const auto path=std::filesystem::path(options.probeOutput.wstring()+L".selection.json");
                    std::ofstream output(path,std::ios::binary|std::ios::trunc);
                    output<<text;
                    if (!output.good()) LogError(L"fixture startup selection diagnostic write",E_FAIL);
                }
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
