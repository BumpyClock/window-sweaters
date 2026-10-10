#include "NativeVisibility.h"

#include <Dwmapi.h>
#include <WtsApi32.h>

#include <cstring>
#include <cwchar>
#include <exception>

namespace window_sweaters::visibility {
namespace {
constexpr wchar_t OverlayClass[] = L"WindowSweaters.Overlay";
constexpr wchar_t OverlayOwnerProperty[] = L"WindowSweaters.Visibility.OverlayOwner";
constexpr Milliseconds ActivationRetryMs = 5000;

HRESULT LastErrorResult() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

GUID NativeDesktopId(const DesktopId& desktop) noexcept {
    GUID result{};
    static_assert(sizeof(result) == sizeof(desktop.bytes));
    std::memcpy(&result, desktop.bytes.data(), sizeof(result));
    return result;
}
}

NativeVisibility::NativeVisibility(HWND receiver) noexcept
    : receiver_(receiver), ownerThreadId_(GetCurrentThreadId()) {}

NativeVisibility::NativeVisibility(HWND receiver, IVirtualDesktopManager& manager) noexcept
    : NativeVisibility(receiver) {
    suppliedManager_ = true;
    manager_ = &manager;
    suppliedDesktopManager_ = &manager;
    stats_.desktopStatus = S_OK;
}

#ifdef WINDOW_SWEATERS_VISIBILITY_TESTING
NativeVisibility::NativeVisibility(HWND receiver, IVirtualDesktopManager& manager,
                                   FactsQuery facts) noexcept
    : NativeVisibility(receiver, manager) {
    factsQuery_ = facts;
}
#endif

NativeVisibility::~NativeVisibility() {
    if (!OnOwnerThread() || activeOperations_ != 0) std::terminate();
    Stop();
}

NativeVisibility::DesktopOperation::DesktopOperation(NativeVisibility& owner) noexcept
    : owner_(owner), lifetime_(owner.lifetime_) {
    ++owner_.activeOperations_;
    manager_ = owner_.manager_;
}

NativeVisibility::DesktopOperation::~DesktopOperation() {
    // Release the operation's reference while shutdown still sees an active call.
    manager_.Reset();
    owner_.FinishOperation();
}

bool NativeVisibility::DesktopOperation::Current() const noexcept {
    return lifetime_ == owner_.lifetime_;
}

bool NativeVisibility::DesktopOperation::Authorized(const ProbeToken& token) const noexcept {
    const auto policy = owner_.policy_.Snapshot();
    return Current() && owner_.started_ && policy.MayWork() && token.sequence != 0
        && token.policyRevision == policy.Revision();
}

void NativeVisibility::FinishOperation() noexcept {
    --activeOperations_;
    ReleaseDeferredCom();
}

void NativeVisibility::ReleaseDeferredCom() noexcept {
    if (activeOperations_ != 0 || releasingCom_) return;
    releasingCom_ = true;
    while (releaseManagerPending_ || uninitializePending_) {
        if (releaseManagerPending_) {
            releaseManagerPending_ = false;
            manager_.Reset();
        }
        if (uninitializePending_) {
            uninitializePending_ = false;
            if (comInitialized_) {
                comInitialized_ = false;
                CoUninitialize();
            }
        }
    }
    releasingCom_ = false;
}

bool NativeVisibility::OnOwnerThread() const noexcept {
    return GetCurrentThreadId() == ownerThreadId_;
}

bool NativeVisibility::ValidReceiver() const noexcept {
    DWORD pid{};
    return receiver_ && IsWindow(receiver_) && !IsWindowVisible(receiver_)
        && GetWindowThreadProcessId(receiver_, &pid) == ownerThreadId_
        && pid == GetCurrentProcessId() && GetParent(receiver_) != HWND_MESSAGE
        && GetAncestor(receiver_, GA_PARENT) == GetDesktopWindow()
        && GetAncestor(receiver_, GA_ROOT) == receiver_
        && !(GetWindowLongPtrW(receiver_, GWL_STYLE) & WS_CHILD);
}

bool NativeVisibility::ValidOverlay(HWND overlay) const noexcept {
    DWORD pid{};
    wchar_t className[64]{};
    constexpr LONG_PTR required = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    return overlay && overlay != receiver_ && IsWindow(overlay)
        && GetWindowThreadProcessId(overlay, &pid) == ownerThreadId_
        && pid == GetCurrentProcessId() && GetParent(overlay) != HWND_MESSAGE
        && GetAncestor(overlay, GA_PARENT) == GetDesktopWindow()
        && GetAncestor(overlay, GA_ROOT) == overlay
        && !(GetWindowLongPtrW(overlay, GWL_STYLE) & WS_CHILD)
        && (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & required) == required
        && GetClassNameW(overlay, className, static_cast<int>(std::size(className))) != 0
        && std::wcscmp(className, OverlayClass) == 0;
}

bool NativeVisibility::RegisteredOverlay(HWND overlay) const noexcept {
    return overlays_.contains(overlay) && ValidOverlay(overlay)
        && GetPropW(overlay, OverlayOwnerProperty) == this;
}

std::optional<std::uint64_t> NativeVisibility::OverlayEpoch(HWND overlay) const noexcept {
    if (!RegisteredOverlay(overlay)) return std::nullopt;
    return overlays_.find(overlay)->second;
}

bool NativeVisibility::CurrentOverlay(HWND overlay, std::uint64_t epoch) const noexcept {
    return OverlayEpoch(overlay) == epoch;
}

HRESULT NativeVisibility::ActivateDesktopManager() noexcept {
    DesktopOperation operation{*this};
    if (suppliedManager_) {
        manager_ = suppliedDesktopManager_;
        return stats_.desktopStatus = manager_ ? S_OK : E_NOINTERFACE;
    }
    if (!comInitialized_) {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result)) return stats_.desktopStatus = result;
        comInitialized_ = true;
    }
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> manager;
    const HRESULT result = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(manager.ReleaseAndGetAddressOf()));
    if (!operation.Current()) return E_PENDING;
    if (SUCCEEDED(result)) manager_ = std::move(manager);
    return stats_.desktopStatus = result;
}

StartResult NativeVisibility::Start() noexcept {
    if (!OnOwnerThread()) return {RPC_E_WRONG_THREAD, RPC_E_WRONG_THREAD};
    if (started_) return {S_OK, stats_.desktopStatus};
    if (activeOperations_ != 0 || releasingCom_) return {E_PENDING, E_PENDING};
    if (!ValidReceiver()) return {E_INVALIDARG, E_UNEXPECTED};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId_))
        return {LastErrorResult(), E_UNEXPECTED};

    const auto lifetime = lifetime_;
    const HRESULT desktopResult = ActivateDesktopManager();
    if (lifetime != lifetime_) return {E_PENDING, E_PENDING};
    lastActivation_ = GetTickCount64();
    sessionRegistered_ = WTSRegisterSessionNotification(receiver_, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (sessionRegistered_) {
        displayRegistration_ = RegisterPowerSettingNotification(receiver_,
            &GUID_SESSION_DISPLAY_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (displayRegistration_)
            suspendRegistration_ = RegisterSuspendResumeNotification(receiver_, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    if (!sessionRegistered_ || !displayRegistration_ || !suspendRegistration_) {
        stats_.lastRegistrationError = GetLastError();
        const auto error = LastErrorResult();
        ReleaseRegistrations();
        policy_.Reset();
        return {error, desktopResult};
    }
    started_ = true;
    policy_.SetSession(QueryCurrentSession(sessionId_));
    lastSessionQuery_ = GetTickCount64();
    // This code is executing; display state remains unknown until its initial notification.
    policy_.SetPower(PowerState::Awake);
    policy_.SetNotificationsReady(true);
    return {S_OK, desktopResult};
}

void NativeVisibility::ReleaseRegistrations() noexcept {
    if (suspendRegistration_) {
        UnregisterSuspendResumeNotification(suspendRegistration_);
        suspendRegistration_ = nullptr;
    }
    if (displayRegistration_) {
        UnregisterPowerSettingNotification(displayRegistration_);
        displayRegistration_ = nullptr;
    }
    if (sessionRegistered_) {
        WTSUnRegisterSessionNotification(receiver_);
        sessionRegistered_ = false;
    }
}

PolicyChange NativeVisibility::Stop() noexcept {
    if (!OnOwnerThread()) return {policy_.Snapshot(), false, false};
    started_ = false;
    ++lifetime_;
    const auto change = policy_.Reset();
    releaseManagerPending_ = true;
    uninitializePending_ = true;
    ReleaseRegistrations();
    for (const auto& registration : overlays_) {
        const auto overlay = registration.first;
        if (ValidOverlay(overlay) && GetPropW(overlay, OverlayOwnerProperty) == this)
            RemovePropW(overlay, OverlayOwnerProperty);
    }
    overlays_.clear();
    ReleaseDeferredCom();
    lastActivation_.reset();
    lastSessionQuery_.reset();
    return change;
}

SessionState NativeVisibility::QueryCurrentSession(DWORD sessionId) noexcept {
    DWORD actualSession{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &actualSession) || actualSession != sessionId)
        return SessionState::Unknown;
    LPWSTR buffer{};
    DWORD bytes{};
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSSessionInfoEx,
                                     &buffer, &bytes))
        return SessionState::Unknown;
    SessionState result = SessionState::Unknown;
    if (buffer && bytes >= sizeof(WTSINFOEXW)) {
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (info->Level == 1 && info->Data.WTSInfoExLevel1.SessionId == sessionId) {
            const auto& session = info->Data.WTSInfoExLevel1;
            if (session.SessionState != WTSActive) result = SessionState::Disconnected;
            else if (session.SessionFlags == WTS_SESSIONSTATE_LOCK) result = SessionState::Locked;
            else if (session.SessionFlags == WTS_SESSIONSTATE_UNLOCK) result = SessionState::Interactive;
        }
    }
    WTSFreeMemory(buffer);
    return result;
}

MessageResult NativeVisibility::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    const auto previous = policy_.Snapshot();
    if (!OnOwnerThread() || !started_) return {false, {previous, false, false}};
    bool handled{};
    if (message == WM_WTSSESSION_CHANGE) {
        if (static_cast<DWORD_PTR>(lParam) != sessionId_)
            return {false, {previous, false, false}};
        switch (wParam) {
        case WTS_SESSION_LOCK:
            policy_.SetSession(SessionState::Locked);
            handled = true;
            break;
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT:
        case WTS_SESSION_LOGOFF:
        case WTS_SESSION_TERMINATE:
            policy_.SetSession(SessionState::Disconnected);
            handled = true;
            break;
        case WTS_SESSION_UNLOCK:
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT:
        case WTS_SESSION_LOGON:
        case WTS_SESSION_REMOTE_CONTROL:
        case WTS_SESSION_DESKTOP_READY:
            policy_.SetSession(QueryCurrentSession(sessionId_));
            handled = true;
            break;
        default: break;
        }
    } else if (message == WM_POWERBROADCAST) {
        switch (wParam) {
        case PBT_APMSUSPEND:
            policy_.SetPower(PowerState::Suspended);
            handled = true;
            break;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
            policy_.SetPower(PowerState::Awake);
            policy_.SetSession(QueryCurrentSession(sessionId_));
            handled = true;
            break;
        case PBT_POWERSETTINGCHANGE:
            if (lParam) {
                const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                if (IsEqualGUID(setting->PowerSetting, GUID_SESSION_DISPLAY_STATUS)) {
                    DisplayState state = DisplayState::Unknown;
                    if (setting->DataLength == sizeof(DWORD)) {
                        DWORD value{};
                        std::memcpy(&value, setting->Data, sizeof(value));
                        if (value == 0) state = DisplayState::Off;
                        else if (value == 1) state = DisplayState::On;
                        else if (value == 2) state = DisplayState::Dimmed;
                    }
                    policy_.SetDisplay(state);
                    handled = true;
                }
            }
            break;
        default: break;
        }
    }
    const auto snapshot = policy_.Snapshot();
    return {handled, {snapshot, snapshot != previous, snapshot.MayWork() && !previous.MayWork()}};
}

PolicyChange NativeVisibility::SetPaused(bool paused) noexcept {
    if (!OnOwnerThread()) return {policy_.Snapshot(), false, false};
    return policy_.SetPaused(paused);
}

PolicyChange NativeVisibility::RefreshUncertainSession(Milliseconds now) noexcept {
    const auto snapshot = policy_.Snapshot();
    if (!OnOwnerThread() || !started_ || snapshot.Paused()
        || snapshot.Power() != PowerState::Awake || snapshot.Session() != SessionState::Unknown)
        return {snapshot, false, false};
    if (lastSessionQuery_ && now >= *lastSessionQuery_ && now - *lastSessionQuery_ < ActivationRetryMs)
        return {snapshot, false, false};
    lastSessionQuery_ = now;
    return policy_.SetSession(QueryCurrentSession(sessionId_));
}

HRESULT NativeVisibility::RetryVirtualDesktop(Milliseconds now) noexcept {
    if (!OnOwnerThread()) return RPC_E_WRONG_THREAD;
    if (!started_ || !policy_.Snapshot().MayWork()) return E_PENDING;
    if (activeOperations_ != 0 || releasingCom_) return E_PENDING;
    if (manager_) return S_OK;
    if (suppliedManager_) return E_NOINTERFACE;
    if (lastActivation_ && now >= *lastActivation_ && now - *lastActivation_ < ActivationRetryMs)
        return stats_.desktopStatus;
    lastActivation_ = now;
    return ActivateDesktopManager();
}

HRESULT NativeVisibility::RegisterOverlay(HWND overlay) {
    if (!OnOwnerThread()) return RPC_E_WRONG_THREAD;
    if (!started_ || !ValidOverlay(overlay)) return E_INVALIDARG;
    if (RegisteredOverlay(overlay)) return S_OK;
    if (nextOverlayEpoch_ == 0) return E_UNEXPECTED;
    if ((!overlays_.contains(overlay) && overlays_.size() >= MaxOverlays)
        || GetPropW(overlay, OverlayOwnerProperty))
        return E_ACCESSDENIED;
    overlays_.insert_or_assign(overlay, nextOverlayEpoch_++);
    if (!SetPropW(overlay, OverlayOwnerProperty, this)) {
        overlays_.erase(overlay);
        return LastErrorResult();
    }
    return S_OK;
}

void NativeVisibility::UnregisterOverlay(HWND overlay) noexcept {
    if (!OnOwnerThread()) return;
    if (overlays_.erase(overlay) && ValidOverlay(overlay)
        && GetPropW(overlay, OverlayOwnerProperty) == this)
        RemovePropW(overlay, OverlayOwnerProperty);
}

HRESULT NativeVisibility::ValidateTarget(const TargetId& target) const noexcept {
    if (!target.hwnd || !target.pid || !target.processStart || !target.generation) return E_INVALIDARG;
    const auto hwnd = reinterpret_cast<HWND>(target.hwnd);
    DWORD pid{};
    if (!IsWindow(hwnd) || !GetWindowThreadProcessId(hwnd, &pid))
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (pid != target.pid || GetAncestor(hwnd, GA_ROOT) != hwnd
        || GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()
        || GetParent(hwnd) == HWND_MESSAGE || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD))
        return E_ACCESSDENIED;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (!process) return LastErrorResult();
    FILETIME start{}, exit{}, kernel{}, user{};
    const bool queried = GetProcessTimes(process, &start, &exit, &kernel, &user) != FALSE;
    const HRESULT error = queried ? S_OK : LastErrorResult();
    CloseHandle(process);
    if (FAILED(error)) return error;
    const auto created = (static_cast<std::uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime;
    return created == target.processStart ? S_OK : E_ACCESSDENIED;
}

HRESULT NativeVisibility::QueryDesktop(const DesktopOperation& operation, const ProbeToken& token,
                                      HWND window, DesktopId& desktop, bool& current,
                                      HWND overlay, std::optional<std::uint64_t> epoch) noexcept {
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    auto* const manager = operation.Manager();
    if (!manager) return FAILED(stats_.desktopStatus) ? stats_.desktopStatus : E_NOINTERFACE;
    GUID first{}, second{};
    BOOL before{}, after{};
    HRESULT result = manager->GetWindowDesktopId(window, &first);
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(window, &before);
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    if (SUCCEEDED(result)) result = manager->GetWindowDesktopId(window, &second);
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(window, &after);
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    if (FAILED(result)) return result;
    if (!IsEqualGUID(first, second) || before != after || IsEqualGUID(first, GUID_NULL))
        return E_PENDING;
    std::memcpy(desktop.bytes.data(), &first, sizeof(first));
    current = before != FALSE;
    return S_OK;
}

bool NativeVisibility::QueryAuthorized(const DesktopOperation& operation, const ProbeToken& token,
                                      HWND overlay, std::optional<std::uint64_t> epoch) const noexcept {
    return operation.Authorized(token) && (!overlay || (epoch && CurrentOverlay(overlay, *epoch)));
}

HRESULT NativeVisibility::QueryCurrentDesktop(const DesktopOperation& operation, const ProbeToken& token,
                                             HWND window, bool& current, HWND overlay,
                                             std::optional<std::uint64_t> epoch) noexcept {
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    auto* const manager = operation.Manager();
    if (!manager) return FAILED(stats_.desktopStatus) ? stats_.desktopStatus : E_NOINTERFACE;
    BOOL before{}, after{};
    HRESULT result = manager->IsWindowOnCurrentVirtualDesktop(window, &before);
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(window, &after);
    if (!QueryAuthorized(operation, token, overlay, epoch)) return E_PENDING;
    if (FAILED(result)) return result;
    if (before != after) return E_PENDING;
    current = before != FALSE;
    return S_OK;
}

HRESULT NativeVisibility::QueryFacts(HWND target, WindowSnapshot& facts) const noexcept {
#ifdef WINDOW_SWEATERS_VISIBILITY_TESTING
    if (factsQuery_) return factsQuery_(target, facts);
#endif
    facts.visible = IsWindowVisible(target) != FALSE;
    facts.minimized = IsIconic(target) != FALSE;
    facts.maximized = IsZoomed(target) != FALSE;
    DWORD cloaked{};
    HRESULT result = DwmGetWindowAttribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (FAILED(result)) return result;
    facts.cloaked = cloaked != 0;
    RECT bounds{};
    result = DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
    if (FAILED(result)) return result;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return E_PENDING;
    const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return LastErrorResult();
    facts.fullscreen = bounds.left <= info.rcMonitor.left && bounds.top <= info.rcMonitor.top
        && bounds.right >= info.rcMonitor.right && bounds.bottom >= info.rcMonitor.bottom;
    return S_OK;
}

void NativeVisibility::RecordFailure(HRESULT error) noexcept {
    ++stats_.queryFailures;
    stats_.lastQueryError = error;
    if (!suppliedManager_ && (error == RPC_E_DISCONNECTED || error == CO_E_OBJNOTCONNECTED
        || error == RPC_E_SERVER_DIED || error == RPC_E_SERVER_DIED_DNE
        || error == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE))) {
        releaseManagerPending_ = true;
        stats_.desktopStatus = error;
        ReleaseDeferredCom();
    }
}

Observation NativeVisibility::Observe(const ProbeToken& token, HWND overlay) noexcept {
    if (!OnOwnerThread()) return {token, std::nullopt, RPC_E_WRONG_THREAD};
    DesktopOperation operation{*this};
    ++stats_.queries;
    auto failure = [this, &token](HRESULT error) {
        RecordFailure(error);
        return Observation{token, std::nullopt, error};
    };
    if (!operation.Authorized(token)) return failure(E_PENDING);
    const auto overlayEpoch = OverlayEpoch(overlay);
    if (overlay && !overlayEpoch) return failure(E_ACCESSDENIED);
    HRESULT result = ValidateTarget(token.target);
    if (FAILED(result)) return failure(result);
    const auto target = reinterpret_cast<HWND>(token.target.hwnd);
    if (target == overlay || overlays_.contains(target)) return failure(E_ACCESSDENIED);
    WindowSnapshot window;
    result = QueryDesktop(operation, token, target, window.desktop, window.onCurrentDesktop,
                          overlay, overlayEpoch);
    if (SUCCEEDED(result)) result = QueryFacts(target, window);
    if (FAILED(result)) return failure(result);
    bool overlayCurrent{};
    if (!overlay) window.overlay = OverlayAssociation::Missing;
    else {
        if (!CurrentOverlay(overlay, *overlayEpoch)) return failure(E_PENDING);
        result = QueryCurrentDesktop(operation, token, overlay, overlayCurrent, overlay, overlayEpoch);
        if (FAILED(result)) return failure(result);
        if (window.onCurrentDesktop && overlayCurrent) {
            window.overlay = OverlayAssociation::CurrentDesktopVerified;
        } else if (window.onCurrentDesktop) {
            DesktopId overlayDesktop;
            bool guidCurrent{};
            result = QueryDesktop(operation, token, overlay, overlayDesktop, guidCurrent, overlay, overlayEpoch);
            if (result == TYPE_E_ELEMENTNOTFOUND) {
                window.overlayDesktopCapability = OverlayDesktopCapability::Unsupported;
                window.overlay = OverlayAssociation::RecreateRequired;
            } else if (FAILED(result)) return failure(result);
            else {
                if (guidCurrent != overlayCurrent) return failure(E_PENDING);
                window.overlayDesktop = overlayDesktop;
                window.overlayDesktopCapability = OverlayDesktopCapability::Available;
                // Contradictory same-GUID/off-current results cannot authorize a move.
                if (overlayDesktop == window.desktop) return failure(E_PENDING);
                window.overlay = OverlayAssociation::Mismatched;
            }
        }
    }
    DesktopId confirmed;
    bool confirmedCurrent{};
    result = QueryDesktop(operation, token, target, confirmed, confirmedCurrent, overlay, overlayEpoch);
    if (SUCCEEDED(result)) result = ValidateTarget(token.target);
    if (FAILED(result)) return failure(result);
    if (confirmed != window.desktop || confirmedCurrent != window.onCurrentDesktop)
        return failure(E_PENDING);
    if (overlay) {
        bool confirmedOverlayCurrent{};
        result = QueryCurrentDesktop(operation, token, overlay, confirmedOverlayCurrent, overlay, overlayEpoch);
        if (FAILED(result)) return failure(result);
        if (confirmedOverlayCurrent != overlayCurrent) return failure(E_PENDING);
    }
    result = QueryFacts(target, window);
    if (SUCCEEDED(result)) result = ValidateTarget(token.target);
    if (FAILED(result)) return failure(result);
    if (!operation.Authorized(token)) return failure(E_PENDING);
    if (overlay && !CurrentOverlay(overlay, *overlayEpoch)) return failure(E_PENDING);
    stats_.lastQueryError = S_OK;
    return {token, window, S_OK};
}

HRESULT NativeVisibility::AssociateOverlay(HWND overlay, const ProbeToken& token,
                                          const DesktopId& desktop) noexcept {
    if (!OnOwnerThread()) return RPC_E_WRONG_THREAD;
    DesktopOperation operation{*this};
    if (!operation.Authorized(token)) return E_PENDING;
    const auto overlayEpoch = OverlayEpoch(overlay);
    if (!overlayEpoch || IsWindowVisible(overlay) || !desktop.IsValid()
        || token.target.hwnd == reinterpret_cast<std::uintptr_t>(overlay))
        return E_ACCESSDENIED;
    HRESULT result = ValidateTarget(token.target);
    if (FAILED(result)) return result;
    const auto target = reinterpret_cast<HWND>(token.target.hwnd);
    if (overlays_.contains(target)) return E_ACCESSDENIED;
    const auto initial = Observe(token, overlay);
    if (!initial.window) return initial.nativeError;
    const auto& window = *initial.window;
    if (window.desktop != desktop || !window.onCurrentDesktop || !window.visible || window.minimized
        || window.cloaked || window.maximized || window.fullscreen)
        return E_PENDING;
    if (!QueryAuthorized(operation, token, overlay, overlayEpoch)) return E_PENDING;
    if (window.overlay == OverlayAssociation::RecreateRequired) return TYPE_E_ELEMENTNOTFOUND;
    if (window.overlay == OverlayAssociation::CurrentDesktopVerified) return S_OK;
    if (window.overlay == OverlayAssociation::Mismatched && window.overlayDesktop
        && window.overlayDesktopCapability == OverlayDesktopCapability::Available) {
        result = ValidateTarget(token.target);
        if (FAILED(result)) return result;
        DesktopId beforeMove;
        bool beforeMoveCurrent{};
        result = QueryDesktop(operation, token, target, beforeMove, beforeMoveCurrent, overlay, overlayEpoch);
        if (FAILED(result)) return result;
        if (beforeMove != desktop || !beforeMoveCurrent) return E_PENDING;
        WindowSnapshot facts;
        result = QueryFacts(target, facts);
        if (FAILED(result)) return result;
        if (!facts.visible || facts.minimized || facts.cloaked || facts.maximized || facts.fullscreen)
            return E_PENDING;
        if (!CurrentOverlay(overlay, *overlayEpoch)) return E_PENDING;
        if (IsWindowVisible(overlay)) return E_ACCESSDENIED;
        // No foreign HWND is passed to the sole mutating virtual-desktop API.
        const GUID id = NativeDesktopId(desktop);
        if (!QueryAuthorized(operation, token, overlay, overlayEpoch)) return E_PENDING;
        result = operation.Manager()->MoveWindowToDesktop(overlay, id);
        if (SUCCEEDED(result)) ++stats_.overlayMoves;
        if (!operation.Authorized(token)) return E_PENDING;
        if (!CurrentOverlay(overlay, *overlayEpoch)) return E_PENDING;
        if (FAILED(result)) {
            RecordFailure(result);
            return result;
        }
    } else return E_PENDING;
    const auto confirmed = Observe(token, overlay);
    if (!confirmed.window) return confirmed.nativeError;
    const auto& finalWindow = *confirmed.window;
    if (finalWindow.desktop != desktop || !finalWindow.onCurrentDesktop
        || finalWindow.overlay != OverlayAssociation::CurrentDesktopVerified || !finalWindow.visible
        || finalWindow.minimized || finalWindow.cloaked || finalWindow.maximized || finalWindow.fullscreen)
        return E_PENDING;
    if (!operation.Authorized(token)) return E_PENDING;
    if (!CurrentOverlay(overlay, *overlayEpoch)) return E_PENDING;
    return S_OK;
}

} // namespace window_sweaters::visibility
