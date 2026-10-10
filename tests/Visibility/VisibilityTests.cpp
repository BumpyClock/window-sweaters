#include "..\..\src\Visibility\NativeVisibility.h"

#include <WtsApi32.h>

#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace window_sweaters::visibility;

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

DesktopId Desktop(std::uint8_t value = 1) {
    DesktopId result;
    result.bytes[0] = value;
    return result;
}

TargetId Target(std::uint64_t generation = 1) {
    return {100, 200, 300, generation};
}

VisibilityPolicy ActivePolicy() {
    VisibilityPolicy policy;
    policy.SetNotificationsReady(true);
    policy.SetSession(SessionState::Interactive);
    policy.SetPower(PowerState::Awake);
    policy.SetDisplay(DisplayState::On);
    return policy;
}

WindowSnapshot Ordinary() {
    return {Desktop(), true, true, false, false, false, false, OverlayAssociation::Matching};
}

Transition Probe(TargetVisibility& target, const PolicySnapshot& policy,
                 std::optional<WindowSnapshot> window) {
    const auto token = target.BeginProbe(policy);
    return target.Reconcile(policy, {token, window, window ? 0 : static_cast<std::int32_t>(E_FAIL)});
}

void StartupIsNotAssumedUnlocked() {
    VisibilityPolicy policy;
    Check(!policy.Snapshot().MayWork(), "Default policy suppresses");
    policy.SetNotificationsReady(true);
    Check(policy.Snapshot().Reason() == Suppression::SessionUnknown, "Initial lock state unknown");
    policy.SetPower(PowerState::Awake);
    policy.SetDisplay(DisplayState::On);
    Check(!policy.Snapshot().MayWork(), "Awake display does not prove session unlocked");
    const auto activated = policy.SetSession(SessionState::Interactive);
    Check(activated.changed && activated.reconcile && activated.snapshot.MayWork(),
          "Public observed unlock enables reconciliation");
    Check(!policy.SetSession(SessionState::Interactive).changed, "Duplicate unlock idempotent");
    Check(!policy.SetDisplay(DisplayState::On).reconcile, "Duplicate display event idempotent");
    policy.SetSession(SessionState::Locked);
    Check(policy.Snapshot().Reason() == Suppression::Locked, "Locked session suppressed");
    policy.SetSession(SessionState::Disconnected);
    Check(policy.Snapshot().Reason() == Suppression::Disconnected, "Disconnected session suppressed");
    policy.SetSession(SessionState::Unknown);
    Check(!policy.Snapshot().MayWork(), "Session query failure suppresses");
}

void DesktopAndAssociationTransitions() {
    auto policy = ActivePolicy();
    const auto snapshot = policy.Snapshot();
    TargetVisibility target{Target()};
    target.ApplyPolicy(snapshot);
    auto window = Ordinary();
    auto transition = Probe(target, snapshot, window);
    Check(transition.accepted && transition.show && !transition.hide && !transition.associate,
          "Matched desktop shows once");
    Check(target.CanPublish(snapshot, transition.token), "Current visible work accepted");
    transition = Probe(target, snapshot, window);
    Check(!transition.show && !transition.hide && !transition.associate, "Stable poll produces no redraw");
    window.onCurrentDesktop = false;
    transition = Probe(target, snapshot, window);
    Check(transition.hide && !transition.show && !transition.associate
          && transition.reason == Suppression::OffDesktop, "Desktop switch hides but does not move target");
    Check(target.Target() == Target(), "Off-desktop retains target identity");
    Check(!Probe(target, snapshot, window).hide, "Duplicate off-desktop transition");
    window.desktop = Desktop(2);
    window.onCurrentDesktop = true;
    window.overlay = OverlayAssociation::Mismatched;
    transition = Probe(target, snapshot, window);
    Check(transition.associate && !transition.show && transition.desktop == Desktop(2),
          "Return on changed desktop needs overlay association before show");
    Check(target.CanAssociate(snapshot, transition.token, Desktop(2)),
          "Current association can execute on owner thread");
    const auto completed = target.CompleteAssociation(snapshot, transition.token, Desktop(2), true);
    Check(completed.show && target.Shown(), "Verified overlay-only association restores once");
    Check(!target.CompleteAssociation(snapshot, transition.token, Desktop(2), true).accepted,
          "Duplicate association cannot show twice");
    window.overlay = OverlayAssociation::Mismatched;
    transition = Probe(target, snapshot, window);
    Check(transition.hide && transition.associate && !transition.show,
          "Residual mismatched overlay hidden before moving");
    Check(!target.CompleteAssociation(snapshot, transition.token, Desktop(3), true).accepted,
          "Wrong desktop completion ignored");
    const auto failed = target.CompleteAssociation(snapshot, transition.token, Desktop(2), false);
    Check(failed.reason == Suppression::QueryUncertain && !target.Shown(),
          "Association error not a safe fallback");
    window.overlay = OverlayAssociation::Matching;
    Check(Probe(target, snapshot, window).show, "Later valid probe restores same target");
}

void ConservativeSuppressionAndRecovery() {
    auto policy = ActivePolicy();
    const auto snapshot = policy.Snapshot();
    TargetVisibility target{Target()};
    target.ApplyPolicy(snapshot);
    const auto normal = Ordinary();
    Check(Probe(target, snapshot, normal).show, "Initial show");
    auto transition = Probe(target, snapshot, std::nullopt);
    Check(transition.hide && transition.reason == Suppression::QueryUncertain,
          "Failed query hides, no lifetime removal");
    Check(target.Target() == Target(), "Failure preserves generation");
    Check(!Probe(target, snapshot, std::nullopt).hide, "Repeated failure idempotent");
    Check(Probe(target, snapshot, normal).show, "Query recovery restores");
    struct Case { WindowSnapshot window; Suppression reason; };
    auto hidden = normal; hidden.visible = false;
    auto minimized = normal; minimized.minimized = true;
    auto cloaked = normal; cloaked.cloaked = true;
    auto maximized = normal; maximized.maximized = true;
    auto fullscreen = normal; fullscreen.fullscreen = true;
    auto uncertain = normal; uncertain.overlay = OverlayAssociation::Uncertain;
    auto invalidDesktop = normal; invalidDesktop.desktop = {};
    const std::array cases{
        Case{hidden, Suppression::Hidden}, Case{minimized, Suppression::Minimized},
        Case{cloaked, Suppression::Cloaked}, Case{maximized, Suppression::Maximized},
        Case{fullscreen, Suppression::Fullscreen}, Case{uncertain, Suppression::QueryUncertain},
        Case{invalidDesktop, Suppression::QueryUncertain}
    };
    for (const auto& item : cases) {
        transition = Probe(target, snapshot, item.window);
        Check(transition.hide && !transition.show && !transition.associate
              && transition.reason == item.reason, "Suppression gate");
        Check(Probe(target, snapshot, normal).show, "Suppression recovery");
    }
}

void LateWorkAndReusedLifetime() {
    auto policy = ActivePolicy();
    auto snapshot = policy.Snapshot();
    TargetVisibility target{Target()};
    target.ApplyPolicy(snapshot);
    const auto first = target.BeginProbe(snapshot);
    const auto second = target.BeginProbe(snapshot);
    Check(!target.Reconcile(snapshot, {first, Ordinary(), 0}).accepted, "Reordered old probe ignored");
    Check(target.Reconcile(snapshot, {second, Ordinary(), 0}).show, "Latest probe shows");
    Check(!target.CanPublish(snapshot, first) && target.CanPublish(snapshot, second),
          "Late rendering work rejected");
    TargetVisibility replacement{Target(2)};
    replacement.ApplyPolicy(snapshot);
    replacement.BeginProbe(snapshot);
    Check(!replacement.Reconcile(snapshot, {second, Ordinary(), 0}).accepted,
          "Reused HWND generation cannot resurrect old target");
    const auto stalePolicy = snapshot;
    policy.SetPaused(true);
    snapshot = policy.Snapshot();
    Check(target.ApplyPolicy(snapshot).hide, "Pause immediately hides before any probe");
    Check(!target.ApplyPolicy(stalePolicy).accepted, "Older snapshot cannot reverse current policy");
    Check(!target.Reconcile(snapshot, {second, Ordinary(), 0}).accepted,
          "Old-policy result cannot unpause decoration");
    Check(!target.CanPublish(snapshot, second), "Paused work rejected");
    Check(!target.ApplyPolicy(snapshot).hide, "Duplicate pause does not hide again");
    policy.SetPaused(false);
    snapshot = policy.Snapshot();
    target.ApplyPolicy(snapshot);
    Check(!target.Shown(), "Unpause requires fresh query");
    Check(!target.Reconcile(snapshot, {second, Ordinary(), 0}).accepted,
          "Unpause cannot accept pre-pause result");
    Check(Probe(target, snapshot, Ordinary()).show, "Unpause query restores");
    Check(stalePolicy.MayWork() && !stalePolicy.Paused(), "Old immutable snapshot remains unchanged");

    auto unassociated = Ordinary();
    unassociated.overlay = OverlayAssociation::Missing;
    const auto association = Probe(target, snapshot, unassociated);
    policy.SetSession(SessionState::Locked);
    snapshot = policy.Snapshot();
    target.ApplyPolicy(snapshot);
    Check(!target.CanAssociate(snapshot, association.token, Desktop()),
          "Late association must not execute native movement");
    Check(!target.CompleteAssociation(snapshot, association.token, Desktop(), true).accepted,
          "Late association cannot resurrect a locked target");
}

void LockSleepResumeAndBoundedPolling() {
    auto policy = ActivePolicy();
    auto snapshot = policy.Snapshot();
    TargetVisibility target{Target()};
    target.ApplyPolicy(snapshot);
    Probe(target, snapshot, Ordinary());
    PollGate poll;
    Check(poll.Due(100, snapshot), "Initial active reconciliation due");
    Check(!poll.Due(100, snapshot) && !poll.Due(349, snapshot), "Duplicate and early polls bounded");
    Check(poll.Due(350, snapshot), "Bounded public API polling due");
    const auto suspend = policy.SetPower(PowerState::Suspended);
    snapshot = suspend.snapshot;
    Check(target.ApplyPolicy(snapshot).hide && !snapshot.MayWork(), "Suspend immediately suppresses");
    Check(!poll.Due(100000, snapshot), "No inactive desktop work during suspend");
    Check(!policy.SetPower(PowerState::Suspended).changed, "Duplicate suspend idempotent");
    policy.SetSession(SessionState::Locked);
    const auto resumedLocked = policy.SetPower(PowerState::Awake);
    Check(!resumedLocked.reconcile && !resumedLocked.snapshot.MayWork(),
          "Resume must not infer unlocked session");
    Check(!poll.Due(100001, resumedLocked.snapshot), "Locked resume does not poll");
    const auto unlock = policy.SetSession(SessionState::Interactive);
    Check(!unlock.snapshot.MayWork(), "Unlock after sleep still needs fresh display state");
    const auto awakeDisplay = policy.SetDisplay(DisplayState::On);
    Check(awakeDisplay.reconcile, "Known awake unlocked display requests reconciliation");
    snapshot = awakeDisplay.snapshot;
    target.ApplyPolicy(snapshot);
    Check(poll.Due(100001, snapshot), "Unlock bypasses old polling deadline once");
    Check(!target.Shown() && Probe(target, snapshot, Ordinary()).show, "Unlock fresh query restores");
    Check(!policy.SetPower(PowerState::Awake).changed, "Duplicate resume idempotent");
    policy.SetDisplay(DisplayState::Off);
    snapshot = policy.Snapshot();
    Check(target.ApplyPolicy(snapshot).hide && !poll.Due(200000, snapshot), "Display-off suspends work");
    const auto displayOn = policy.SetDisplay(DisplayState::On);
    Check(displayOn.reconcile && poll.Due(200001, displayOn.snapshot), "Display-on reconciles");
    policy.SetDisplay(DisplayState::Unknown);
    Check(!policy.Snapshot().MayWork(), "Unknown display suppresses");
    policy.SetPaused(true);
    policy.Reset();
    Check(policy.Snapshot().Paused() && !policy.Snapshot().NotificationsReady(),
          "Shutdown preserves pause configuration, removes notification authority");
    Check(PollBudget == 32 && MaxOverlays == 512, "Integration work bounds");
}

class FakeDesktopManager final : public IVirtualDesktopManager {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IVirtualDesktopManager))) {
            *object = static_cast<IVirtualDesktopManager*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        if (insideQuery) releasedInsideQuery = true;
        return --references;
    }
    HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(HWND window, BOOL* current) override {
        ++membershipQueries;
        Reenter();
        if (FAILED(membershipError)) return membershipError;
        const auto found = currentByWindow.find(window);
        *current = found == currentByWindow.end() ? onCurrent : found->second;
        if (changeMembership) onCurrent = !onCurrent;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND window, GUID* id) override {
        ++desktopQueries;
        ++desktopQueriesByWindow[window];
        Reenter();
        if (FAILED(desktopError)) return desktopError;
        const auto error = desktopErrorsByWindow.find(window);
        if (error != desktopErrorsByWindow.end()) return error->second;
        if (nullDesktop) *id = GUID_NULL;
        else {
            auto found = desktopByWindow.find(window);
            *id = found == desktopByWindow.end() ? GUID{1, 0, 0, {}} : found->second;
            if (changeDesktop) id->Data1 += desktopQueries;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND window, REFGUID id) override {
        ++moves;
        lastMoved = window;
        if (onMove) onMove();
        desktopByWindow[window] = id;
        currentByWindow[window] = TRUE;
        return S_OK;
    }
    std::atomic<ULONG> references{1};
    HRESULT desktopError{S_OK};
    HRESULT membershipError{S_OK};
    BOOL onCurrent{TRUE};
    bool changeDesktop{};
    bool changeMembership{};
    bool nullDesktop{};
    std::uint32_t desktopQueries{};
    std::uint32_t membershipQueries{};
    std::uint32_t moves{};
    HWND lastMoved{};
    std::unordered_map<HWND, GUID> desktopByWindow;
    std::unordered_map<HWND, BOOL> currentByWindow;
    std::unordered_map<HWND, HRESULT> desktopErrorsByWindow;
    std::unordered_map<HWND, std::uint32_t> desktopQueriesByWindow;
    std::function<void()> onQuery;
    std::function<void()> onMove;
    bool insideQuery{};
    bool releasedInsideQuery{};
    std::uint32_t queries{};

private:
    void Reenter() {
        ++queries;
        insideQuery = true;
        if (onQuery) onQuery();
        insideQuery = false;
    }
};

class HiddenWindow final {
public:
    explicit HiddenWindow(const wchar_t* className = L"VisibilityTests.Hidden",
                          DWORD extendedStyle = 0, HWND parent = nullptr) {
        WNDCLASSW klass{};
        klass.lpfnWndProc = DefWindowProcW;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = className;
        if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Hidden fixture class registration");
        window = CreateWindowExW(extendedStyle, className, L"", WS_POPUP,
                                 0, 0, 64, 64, parent, nullptr, klass.hInstance, nullptr);
        Check(window != nullptr, "Hidden fixture creation");
        Check(!IsWindowVisible(window), "Fixture never displayed");
    }
    ~HiddenWindow() { if (window) DestroyWindow(window); }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
    HWND window{};
};

TargetId NativeTarget(HWND hwnd) {
    FILETIME start{}, exit{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &start, &exit, &kernel, &user) != FALSE,
          "Process creation identity");
    return {reinterpret_cast<std::uintptr_t>(hwnd), GetCurrentProcessId(),
        (static_cast<std::uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime, 1};
}

void SetDisplayOn(NativeVisibility& native) {
    struct Setting {
        GUID PowerSetting;
        DWORD DataLength;
        DWORD Data;
    } setting{GUID_SESSION_DISPLAY_STATUS, sizeof(DWORD), 1};
    const auto result = native.HandleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE,
                                            reinterpret_cast<LPARAM>(&setting));
    Check(result.handled && native.Snapshot().Display() == DisplayState::On, "Display notification");
}

void NativeReceiverAndOwnedOverlayValidation() {
    HiddenWindow receiver;
    HiddenWindow messageOnly{L"VisibilityTests.Hidden", 0, HWND_MESSAGE};
    FakeDesktopManager manager;
    {
        NativeVisibility invalid{nullptr, manager};
        Check(invalid.Start().notifications == E_INVALIDARG, "Null receiver rejected");
    }
    {
        NativeVisibility invalid{messageOnly.window, manager};
        Check(invalid.Start().notifications == E_INVALIDARG, "Message-only receiver rejected");
    }
    NativeVisibility native{receiver.window, manager};
    Check(native.Snapshot().Session() == SessionState::Unknown, "Native initial state never unlocked guess");
    const auto started = native.Start();
    Check(SUCCEEDED(started.notifications) && SUCCEEDED(started.virtualDesktop), "Notification registration");
    SetDisplayOn(native);
    Check(native.Start().notifications == S_OK, "Duplicate start does not duplicate registrations");
    Check(FAILED(native.RegisterOverlay(nullptr)), "Null overlay rejected");
    Check(FAILED(native.RegisterOverlay(receiver.window)), "Receiver cannot be marked overlay");
    HiddenWindow arbitrary;
    Check(FAILED(native.RegisterOverlay(arbitrary.window)), "Arbitrary same-process window rejected");
    Check(FAILED(native.RegisterOverlay(GetDesktopWindow())), "Foreign desktop HWND rejected");
    HiddenWindow wrongStyle{L"WindowSweaters.Overlay"};
    Check(FAILED(native.RegisterOverlay(wrongStyle.window)), "Wrong overlay styles rejected");
    constexpr DWORD overlayStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    HiddenWindow overlay{L"WindowSweaters.Overlay", overlayStyle};
    Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Our hidden overlay registers");
    Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Overlay registration idempotent");
    DWORD sessionId{};
    Check(ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) != FALSE, "Current session ID");
    const auto beforeWrongSession = native.Snapshot();
    Check(!native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK,
                                static_cast<LPARAM>(sessionId + 1)).handled
          && native.Snapshot() == beforeWrongSession, "Foreign session lock ignored");
    const auto locked = native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, sessionId);
    Check(locked.handled && native.Snapshot().Session() == SessionState::Locked, "Lock message suppresses");
    Check(!native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, sessionId).policy.changed,
          "Native duplicate lock idempotent");
    native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, sessionId);
    Check(native.Snapshot().Session() == NativeVisibility::QueryCurrentSession(sessionId),
          "Unlock uses public current-session observation, not default unlocked");
    const auto suspended = native.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    Check(suspended.handled && suspended.policy.changed
          && native.Snapshot().Power() == PowerState::Suspended, "Suspend notification");
    Check(!native.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0).policy.changed,
          "Duplicate suspend notification");
    native.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    Check(native.Snapshot().Power() == PowerState::Awake
          && native.Snapshot().Session() == NativeVisibility::QueryCurrentSession(sessionId),
          "Resume reobserves session, never assumes unlocked");
    Check(native.Snapshot().Display() == DisplayState::Unknown && !native.Snapshot().MayWork(),
          "Automatic resume cannot reuse pre-suspend display state");
    Check(!native.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0).policy.changed,
          "Automatic/user resume duplicate");
    struct MalformedSetting {
        GUID PowerSetting;
        DWORD DataLength;
        DWORD Data;
    } malformed{GUID_SESSION_DISPLAY_STATUS, 1, 1};
    const auto malformedDisplay = native.HandleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE,
                                                       reinterpret_cast<LPARAM>(&malformed));
    Check(malformedDisplay.handled && native.Snapshot().Display() == DisplayState::Unknown,
          "Malformed display state suppresses instead of guessing");
    Check(!native.HandleMessage(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, 0).handled,
          "Null power payload ignored");
    SetDisplayOn(native);
    HRESULT wrongThread{};
    std::thread other{[&] { wrongThread = native.RegisterOverlay(overlay.window); }};
    other.join();
    Check(wrongThread == RPC_E_WRONG_THREAD, "Wrong-thread registration rejected");
    auto target = NativeTarget(arbitrary.window);
    ProbeToken token{target, native.Snapshot().Revision(), 1};
    Check(FAILED(native.AssociateOverlay(arbitrary.window, token, Desktop())),
          "Unregistered same-process HWND cannot move");
    Check(FAILED(native.AssociateOverlay(GetDesktopWindow(), token, Desktop())),
          "Foreign overlay HWND cannot move");
    Check(FAILED(native.AssociateOverlay(nullptr, token, Desktop())), "Null move rejected");
    Check(manager.moves == 0, "Invalid overlays never reach even fake moving API");
    const HWND destroyed = overlay.window;
    Check(DestroyWindow(overlay.window) != FALSE, "Destroy hidden test overlay");
    overlay.window = nullptr;
    Check(FAILED(native.AssociateOverlay(destroyed, token, Desktop())), "Destroyed own overlay rejected");
    native.UnregisterOverlay(destroyed);
    const auto stopped = native.Stop();
    Check(stopped.changed && !stopped.snapshot.NotificationsReady(), "Stop unregisters authority");
    Check(!native.Stop().changed, "Duplicate stop idempotent");
    Check(!native.RefreshUncertainSession(GetTickCount64()).changed,
          "Shutdown does not perform session recovery work");
    Check(!native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, sessionId).handled,
          "Late session message after shutdown ignored");
}

void NativeFakeQueriesNeverMoveUserDesktops() {
    HiddenWindow receiver;
    HiddenWindow targetWindow;
    HiddenWindow overlay{L"WindowSweaters.Overlay",
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT};
    FakeDesktopManager manager;
    NativeVisibility native{receiver.window, manager};
    Check(SUCCEEDED(native.Start().notifications), "Native query fixture registrations");
    Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Query fixture overlay registration");
    SetDisplayOn(native);
    DWORD sessionId{};
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    if (native.Snapshot().MayWork()) {
        const auto target = NativeTarget(targetWindow.window);
        const ProbeToken token{target, native.Snapshot().Revision(), 1};
        manager.desktopError = E_NOINTERFACE;
        auto observed = native.Observe(token, overlay.window);
        Check(!observed.window && observed.nativeError == E_NOINTERFACE,
              "Documented API absence not successful fallback");
        manager.desktopError = S_OK;
        manager.membershipError = E_FAIL;
        observed = native.Observe(token, overlay.window);
        Check(!observed.window && observed.nativeError == E_FAIL, "Membership query failure suppresses");
        manager.membershipError = S_OK;
        manager.changeDesktop = true;
        observed = native.Observe(token, overlay.window);
        Check(!observed.window && observed.nativeError == E_PENDING, "Desktop changed during query suppresses");
        manager.changeDesktop = false;
        manager.changeMembership = true;
        observed = native.Observe(token, overlay.window);
        Check(!observed.window && observed.nativeError == E_PENDING, "Current desktop drift suppresses");
        manager.changeMembership = false;
        manager.onCurrent = TRUE;
        manager.nullDesktop = true;
        observed = native.Observe(token, overlay.window);
        Check(!observed.window && observed.nativeError == E_PENDING, "Null desktop identity uncertain");
        manager.nullDesktop = false;
        auto stale = token;
        stale.target.processStart += 1;
        observed = native.Observe(stale, overlay.window);
        Check(!observed.window && observed.nativeError == E_ACCESSDENIED, "Wrong process lifetime rejected");
        stale = token;
        stale.target.pid += 1;
        observed = native.Observe(stale, overlay.window);
        Check(!observed.window && observed.nativeError == E_ACCESSDENIED, "Foreign ownership mismatch rejected");
        stale = token;
        stale.target.hwnd = 0;
        observed = native.Observe(stale, overlay.window);
        Check(!observed.window && observed.nativeError == E_INVALIDARG, "Null target HWND rejected");
        stale = token;
        stale.policyRevision += 1;
        observed = native.Observe(stale, overlay.window);
        Check(!observed.window && observed.nativeError == E_PENDING, "Stale native policy result rejected");
        Check(FAILED(native.AssociateOverlay(overlay.window, token, Desktop())),
              "Hidden target does not justify moving overlay");
    } else {
        std::cout << "SKIP active native query checks: current session not observed interactive\n";
    }
    Check(manager.moves == 0 && native.Stats().overlayMoves == 0,
          "No native test moves windows between real or simulated user desktops");
    native.UnregisterOverlay(overlay.window);
    native.Stop();
    Check(manager.references == 2, "Adapter retains injected interface until destruction");
}

HRESULT SyntheticVisibleFacts(HWND, WindowSnapshot& facts) noexcept {
    facts.visible = true;
    facts.minimized = false;
    facts.cloaked = false;
    facts.maximized = false;
    facts.fullscreen = false;
    return S_OK;
}

void ReentrantNativeDesktopOperations() {
    enum class Mutation { Stop, Pause, Lock, PauseAndResume };
    const std::array mutations{Mutation::Stop, Mutation::Pause, Mutation::Lock, Mutation::PauseAndResume};
    DWORD sessionId{};
    Check(ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) != FALSE, "Reentrancy session ID");
    for (const bool association : {false, true}) {
        for (const auto mutation : mutations) {
            const std::uint32_t lastQuery = association ? 32 : 12;
            for (std::uint32_t query = 1; query <= lastQuery; ++query) {
                HiddenWindow receiver;
                HiddenWindow targetWindow;
                HiddenWindow overlay{L"WindowSweaters.Overlay",
                    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT};
                FakeDesktopManager manager;
                NativeVisibility native{receiver.window, manager, SyntheticVisibleFacts};
                Check(SUCCEEDED(native.Start().notifications), "Reentrant fixture registration");
                SetDisplayOn(native);
                if (!native.Snapshot().MayWork()) {
                    std::cout << "SKIP reentrant native checks: session not observed interactive\n";
                    return;
                }
                Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Reentrant overlay registration");
                const ProbeToken token{NativeTarget(targetWindow.window), native.Snapshot().Revision(), 1};
                if (association) {
                    manager.desktopByWindow[overlay.window] = GUID{2, 0, 0, {}};
                    manager.currentByWindow[overlay.window] = FALSE;
                }
                bool invoked{};
                bool immediateSuppression{};
                bool restartRejected{};
                ULONG referencesAtStop{};
                manager.onQuery = [&] {
                    if (manager.queries != query) return;
                    invoked = true;
                    switch (mutation) {
                    case Mutation::Stop:
                        native.Stop();
                        immediateSuppression = !native.Snapshot().NotificationsReady();
                        referencesAtStop = manager.references;
                        restartRejected = native.Start().notifications == E_PENDING;
                        break;
                    case Mutation::Pause:
                        native.SetPaused(true);
                        immediateSuppression = !native.Snapshot().MayWork();
                        break;
                    case Mutation::Lock:
                        native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, sessionId);
                        immediateSuppression = !native.Snapshot().MayWork();
                        break;
                    case Mutation::PauseAndResume:
                        native.SetPaused(true);
                        native.SetPaused(false);
                        immediateSuppression = native.Snapshot().MayWork()
                            && native.Snapshot().Revision() != token.policyRevision;
                        break;
                    }
                };
                if (association) {
                    const auto result = native.AssociateOverlay(overlay.window, token, Desktop());
                    Check(result == E_PENDING, "Reentrant policy invalidation prevents native association");
                } else {
                    const auto result = native.Observe(token, overlay.window);
                    Check(!result.window && result.nativeError == E_PENDING,
                          "Reentrant invalidation drops native observation");
                }
                if (!invoked || !immediateSuppression) {
                    std::cerr << "Reentrancy case association=" << association
                              << " mutation=" << static_cast<int>(mutation)
                              << " query=" << query << " reached=" << manager.queries << '\n';
                }
                Check(invoked && immediateSuppression, "Query callback immediately invalidates authority");
                Check(manager.moves == (association && query > 20 ? 1u : 0u),
                      "Reentrant query cannot authorize a stale desktop move");
                Check(!manager.releasedInsideQuery, "Stop does not release COM while a query is on the stack");
                if (mutation == Mutation::Stop) {
                    const ULONG expectedReferences = association && (query <= 16 || query > 20) ? 5u : 4u;
                    Check(referencesAtStop == expectedReferences && manager.references == 2,
                          "Stop retains operation COM reference and releases manager only after unwind");
                    Check(restartRejected, "Reentrant restart cannot resurrect active-operation authority");
                    Check(SUCCEEDED(native.Start().notifications), "Restart allowed after old operation unwinds");
                }
                native.Stop();
            }
        }
    }

    HiddenWindow receiver;
    HiddenWindow targetWindow;
    HiddenWindow overlay{L"WindowSweaters.Overlay",
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT};
    FakeDesktopManager manager;
    NativeVisibility native{receiver.window, manager, SyntheticVisibleFacts};
    Check(SUCCEEDED(native.Start().notifications), "Reentrant move fixture registration");
    SetDisplayOn(native);
    Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Reentrant move overlay registration");
    manager.desktopByWindow[overlay.window] = GUID{2, 0, 0, {}};
    manager.currentByWindow[overlay.window] = FALSE;
    const ProbeToken token{NativeTarget(targetWindow.window), native.Snapshot().Revision(), 1};
    manager.onMove = [&] { native.Stop(); };
    Check(native.AssociateOverlay(overlay.window, token, Desktop()) == E_PENDING,
          "Stop during already-authorized fake move drops completion");
    Check(manager.moves == 1 && manager.lastMoved == overlay.window && manager.references == 2,
          "Only fake owned-overlay move executes; its COM reference drains after reentrant stop");
}

void ReentrantOverlayRegistrationLifetime() {
    enum class Mutation { Unregister, Reregister, Destroy };
    for (const bool association : {false, true}) {
        for (const auto mutation : {Mutation::Unregister, Mutation::Reregister, Mutation::Destroy}) {
            const std::uint32_t lastQuery = association ? 32 : 12;
            for (std::uint32_t query = 1; query <= lastQuery; ++query) {
                HiddenWindow receiver;
                HiddenWindow targetWindow;
                HiddenWindow overlay{L"WindowSweaters.Overlay",
                    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT};
                FakeDesktopManager manager;
                NativeVisibility native{receiver.window, manager, SyntheticVisibleFacts};
                Check(SUCCEEDED(native.Start().notifications), "Overlay lifetime fixture registration");
                SetDisplayOn(native);
                if (!native.Snapshot().MayWork()) {
                    std::cout << "SKIP overlay lifetime checks: session not observed interactive\n";
                    return;
                }
                Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Overlay lifetime registration");
                const auto hwnd = overlay.window;
                const auto revision = native.Snapshot().Revision();
                const ProbeToken token{NativeTarget(targetWindow.window), revision, 1};
                if (association) {
                    manager.desktopByWindow[hwnd] = GUID{2, 0, 0, {}};
                    manager.currentByWindow[hwnd] = FALSE;
                }
                bool invoked{};
                HRESULT replacement{E_UNEXPECTED};
                manager.onQuery = [&] {
                    if (manager.queries != query) return;
                    invoked = true;
                    if (mutation == Mutation::Destroy) {
                        replacement = DestroyWindow(hwnd) ? S_OK : E_FAIL;
                        overlay.window = nullptr;
                    } else {
                        native.UnregisterOverlay(hwnd);
                        if (mutation == Mutation::Reregister) replacement = native.RegisterOverlay(hwnd);
                    }
                };
                if (association) {
                    Check(FAILED(native.AssociateOverlay(hwnd, token, Desktop())),
                          "Overlay registration change must reject association success");
                } else {
                    const auto observed = native.Observe(token, hwnd);
                    Check(!observed.window && FAILED(observed.nativeError),
                          "Overlay registration change must reject matching observation");
                }
                Check(invoked && native.Snapshot().Revision() == revision,
                      "Overlay teardown alone invalidates registration, not global policy");
                Check(manager.moves == (association && query > 20 ? 1u : 0u),
                      "Only an association authorized before teardown may call fake movement");
                if (mutation == Mutation::Reregister || mutation == Mutation::Destroy)
                    Check(SUCCEEDED(replacement), "Reentrant overlay teardown/replacement succeeded");
                if (mutation == Mutation::Reregister) {
                    manager.onQuery = {};
                    const auto current = native.Observe(token, hwnd);
                    Check(current.window.has_value(), "Fresh observation accepts new same-HWND registration");
                }
                native.Stop();
            }
        }
    }
}

void CurrentDesktopToolOverlayWithoutGuid() {
    HiddenWindow receiver;
    HiddenWindow targetWindow;
    HiddenWindow overlay{L"WindowSweaters.Overlay",
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT};
    FakeDesktopManager manager;
    manager.desktopErrorsByWindow[overlay.window] = TYPE_E_ELEMENTNOTFOUND;
    NativeVisibility native{receiver.window, manager, SyntheticVisibleFacts};
    Check(SUCCEEDED(native.Start().notifications), "Current pair fixture registration");
    SetDisplayOn(native);
    if (!native.Snapshot().MayWork()) {
        std::cout << "SKIP current pair checks: session not observed interactive\n";
        return;
    }
    Check(SUCCEEDED(native.RegisterOverlay(overlay.window)), "Current pair overlay registration");
    const auto snapshot = native.Snapshot();
    TargetVisibility visibility{NativeTarget(targetWindow.window)};
    visibility.ApplyPolicy(snapshot);
    auto token = visibility.BeginProbe(snapshot);
    auto observed = native.Observe(token, overlay.window);
    Check(observed.window && observed.window->overlay == OverlayAssociation::CurrentDesktopVerified
          && !observed.window->overlayDesktop
          && observed.window->overlayDesktopCapability == OverlayDesktopCapability::NotQueried,
          "Current membership verified explicitly without invented overlay GUID");
    Check(manager.desktopQueriesByWindow[overlay.window] == 0, "Current tool overlay GUID never queried");
    Check(visibility.Reconcile(snapshot, observed).show, "Current tool pair renders despite missing AppView GUID");
    Check(SUCCEEDED(native.AssociateOverlay(overlay.window, token, Desktop())) && manager.moves == 0,
          "Current pair association needs verification, not GUID or movement");
    Check(manager.desktopQueriesByWindow[overlay.window] == 0, "Current association avoids unsupported GUID too");

    manager.currentByWindow[overlay.window] = FALSE;
    token = visibility.BeginProbe(snapshot);
    observed = native.Observe(token, overlay.window);
    Check(observed.window && observed.window->overlay == OverlayAssociation::RecreateRequired
          && !observed.window->overlayDesktop
          && observed.window->overlayDesktopCapability == OverlayDesktopCapability::Unsupported,
          "Off-current tool without GUID requires own recreation, not fabricated matching");
    const auto transition = visibility.Reconcile(snapshot, observed);
    Check(transition.hide && transition.recreate && !transition.show && !transition.associate,
          "Hide before recreating only unsupported own overlay");
    Check(native.AssociateOverlay(overlay.window, token, Desktop()) == TYPE_E_ELEMENTNOTFOUND
          && manager.moves == 0, "Unsupported overlay cannot be moved through a fallback");
    manager.currentByWindow[targetWindow.window] = FALSE;
    token = visibility.BeginProbe(snapshot);
    observed = native.Observe(token, overlay.window);
    Check(observed.window && !observed.window->onCurrentDesktop,
          "Off-desktop target can be observed without overlay GUID requirement");
    const auto off = visibility.Reconcile(snapshot, observed);
    Check(off.reason == Suppression::OffDesktop && !off.show && !off.associate && !off.recreate,
          "Off-desktop target stays suppressed and never moved");
    manager.currentByWindow[targetWindow.window] = TRUE;
    manager.currentByWindow[overlay.window] = TRUE;
    token = visibility.BeginProbe(snapshot);
    manager.membershipError = E_FAIL;
    observed = native.Observe(token, overlay.window);
    Check(!observed.window && !visibility.Reconcile(snapshot, observed).show,
          "Public membership failure cannot pass current verification");
    manager.membershipError = S_OK;
    native.SetPaused(true);
    Check(!native.Observe(token, overlay.window).window, "Pause rejects current pair");
    native.SetPaused(false);
    DWORD session{};
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    native.HandleMessage(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, session);
    Check(!native.Observe(token, overlay.window).window, "Lock rejects current pair");
    Check(manager.moves == 0, "No fake or real desktop movement in current unsupported-GUID regressions");
}

int ReadOnlyWindowPairProbe(HWND target, HWND overlay) {
    if (!IsWindow(target) || !IsWindow(overlay)) {
        std::cerr << "FAIL probe: invalid window pair\n";
        return 2;
    }
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) return 3;
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> manager;
    HRESULT result = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(manager.GetAddressOf()));
    BOOL targetBefore{}, targetAfter{}, overlayBefore{}, overlayAfter{};
    GUID targetIdBefore{}, targetIdAfter{}, diagnosticOverlayId{};
    HRESULT overlayGuidResult = E_PENDING;
    if (SUCCEEDED(result)) result = manager->GetWindowDesktopId(target, &targetIdBefore);
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(target, &targetBefore);
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(overlay, &overlayBefore);
    if (SUCCEEDED(result)) result = manager->GetWindowDesktopId(target, &targetIdAfter);
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(target, &targetAfter);
    if (SUCCEEDED(result)) result = manager->IsWindowOnCurrentVirtualDesktop(overlay, &overlayAfter);
    if (manager) overlayGuidResult = manager->GetWindowDesktopId(overlay, &diagnosticOverlayId);
    const bool verified = SUCCEEDED(result) && targetBefore && targetAfter && overlayBefore && overlayAfter
        && IsEqualGUID(targetIdBefore, targetIdAfter) && !IsEqualGUID(targetIdBefore, GUID_NULL);
    std::cout << "READONLY pair target=" << reinterpret_cast<std::uintptr_t>(target)
              << " overlay=" << reinterpret_cast<std::uintptr_t>(overlay)
              << " query_hr=0x" << std::hex << static_cast<unsigned long>(result)
              << " overlay_guid_diagnostic_hr=0x" << static_cast<unsigned long>(overlayGuidResult)
              << std::dec << " target_current=" << targetAfter << " overlay_current=" << overlayAfter
              << " CurrentDesktopVerified=" << verified << '\n';
    manager.Reset();
    CoUninitialize();
    return verified ? 0 : 1;
}

}

int wmain(int count, wchar_t** arguments) {
    if (count == 4 && std::wcscmp(arguments[1], L"--probe-window-pair") == 0) {
        return ReadOnlyWindowPairProbe(
            reinterpret_cast<HWND>(std::wcstoull(arguments[2], nullptr, 10)),
            reinterpret_cast<HWND>(std::wcstoull(arguments[3], nullptr, 10)));
    }
    if (count != 1) return 2;
    try {
        StartupIsNotAssumedUnlocked();
        DesktopAndAssociationTransitions();
        ConservativeSuppressionAndRecovery();
        LateWorkAndReusedLifetime();
        LockSleepResumeAndBoundedPolling();
        NativeReceiverAndOwnedOverlayValidation();
        NativeFakeQueriesNeverMoveUserDesktops();
        ReentrantNativeDesktopOperations();
        ReentrantOverlayRegistrationLifetime();
        CurrentDesktopToolOverlayWithoutGuid();
        std::cout << "PASS visibility: 10 groups (pure transitions and hidden native/fake-COM checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL visibility: " << error.what() << '\n';
        return 1;
    }
}
