#include "WindowTracker.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <exception>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace window_sweaters::tracking {

NativeQueryCounts& QueryCounts() noexcept {
    static thread_local NativeQueryCounts counts;
    return counts;
}

namespace {

thread_local std::unordered_map<HWINEVENTHOOK, WindowTracker*> hookOwners;

Policy MakePolicy(const TrackerConfig& config) {
    Policy policy;
    policy.mode = config.mode;
    policy.fixtureTarget = reinterpret_cast<WindowHandle>(config.fixtureTarget);
    policy.fixtureIdentity = config.initialFixtureIdentity;
    policy.hostPid = GetCurrentProcessId();
    policy.excludedPids = config.excludedPids;
    policy.excludedClasses = config.excludedClasses;
    policy.maxTargets = std::min(config.maxTargets, TrackerTargetLimit);
    policy.resizeQuietPeriod = std::max<Milliseconds>(config.resizeQuietPeriod, TrackerTimerPeriodMs);
    policy.selectedTargets = config.selectedTargets;
    return policy;
}

void Append(ActionBatch& destination, ActionBatch source) {
    destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                       std::make_move_iterator(source.end()));
}

std::uint64_t ProcessStart(DWORD pid, NativeQueryFailure* failure = nullptr) noexcept {
    ++QueryCounts().processOpens;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        if (failure) *failure={NativeQueryStage::OpenProcess,GetLastError()};
        return 0;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool success = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    if (!success && failure) *failure={NativeQueryStage::ProcessTimes,GetLastError()};
    CloseHandle(process);
    const auto start=success ? (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime : 0;
    if (success && !start && failure) *failure={NativeQueryStage::ProcessTimes,ERROR_INVALID_DATA};
    return start;
}

std::uint64_t ObservedIdentity(DWORD threadId, const wchar_t* className) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash = (hash ^ threadId) * 1099511628211ULL;
    while (*className) {
        hash = (hash ^ static_cast<std::uint16_t>(*className++)) * 1099511628211ULL;
    }
    return hash ? hash : 1;
}

bool ReadStyle(HWND hwnd, int index, LONG_PTR& style) noexcept {
    SetLastError(ERROR_SUCCESS);
    style = GetWindowLongPtrW(hwnd, index);
    return style != 0 || GetLastError() == ERROR_SUCCESS;
}

// Single definition of "ordinary" for Query and the Probe prefilter. Packaged app frames use captioned WS_POPUP roots.
bool OrdinaryTopLevel(HWND hwnd, const wchar_t* className, LONG_PTR style, LONG_PTR extendedStyle) noexcept {
    const bool supportedPopup = !(style & WS_POPUP) || std::wcscmp(className,L"ApplicationFrameWindow")==0;
    return !(style & WS_CHILD) && supportedPopup && (style & WS_CAPTION) == WS_CAPTION
        && !(extendedStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
        && GetAncestor(hwnd, GA_ROOT) == hwnd && !GetWindow(hwnd, GW_OWNER)
        && hwnd != GetDesktopWindow() && hwnd != GetShellWindow();
}

// True only on a definite rejection; any unreadable fact defers to the full Query.
bool PlainlyUnsupported(HWND hwnd) noexcept {
    wchar_t className[256]{};
    LONG_PTR style{}, extendedStyle{};
    return ReadStyle(hwnd,GWL_STYLE,style) && ReadStyle(hwnd,GWL_EXSTYLE,extendedStyle)
        && GetClassNameW(hwnd,className,static_cast<int>(std::size(className)))
        && !OrdinaryTopLevel(hwnd,className,style,extendedStyle);
}

Observation FailedQuery(HWND hwnd) {
    const auto handle = reinterpret_cast<WindowHandle>(hwnd);
    return IsWindow(hwnd) ? Observation{UncertainWindow{handle}}
                          : Observation{DestroyedWindow{handle}};
}

class PhysicalCoordinates final {
public:
    PhysicalCoordinates() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinates() {
        if (previous_) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
    bool Valid() const noexcept { return previous_ != nullptr; }
private:
    DPI_AWARENESS_CONTEXT previous_{};
};

struct Enumeration {
    std::vector<WindowHandle>* handles;
    std::unordered_set<WindowHandle>* seen;
    bool truncated{};
    bool allocationFailed{};
};

BOOL CALLBACK EnumerateWindow(HWND hwnd, LPARAM parameter) noexcept {
    auto& state = *reinterpret_cast<Enumeration*>(parameter);
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        return TRUE;
    }
    const auto handle = reinterpret_cast<WindowHandle>(hwnd);
    if (state.handles->size() >= TrackerEnumerationLimit) {
        state.truncated = true;
        return FALSE;
    }
    try {
        if (state.seen->insert(handle).second) {
            state.handles->push_back(handle);
        }
        return TRUE;
    } catch (...) {
        state.allocationFailed = true;
        return FALSE;
    }
}

Milliseconds EventTimestamp(DWORD time) noexcept {
    const auto now = GetTickCount64();
    const auto elapsed = static_cast<DWORD>(static_cast<DWORD>(now) - time);
    return elapsed <= now ? now - elapsed : 0;
}

} // namespace

WindowTracker::WindowTracker(HWND receiver, TrackerConfig config)
    : receiver_(receiver), ownerThreadId_(GetCurrentThreadId()), config_(std::move(config)),
      model_(MakePolicy(config_)) {}

WindowTracker::~WindowTracker() {
    if (!OnOwnerThread() && started_) {
        std::terminate();
    }
    ShutdownHooks();
}

bool WindowTracker::OnOwnerThread() const noexcept {
    return GetCurrentThreadId() == ownerThreadId_;
}

Observation WindowTracker::Query(HWND hwnd, NativeQueryFailure* failure) {
    if (failure) *failure={};
    const auto handle = reinterpret_cast<WindowHandle>(hwnd);
    const auto failed=[&](NativeQueryStage stage,DWORD error=0,HRESULT result=S_OK) {
        if (failure) *failure={stage,error,result};
        return FailedQuery(hwnd);
    };
    const auto uncertain=[&](NativeQueryStage stage,DWORD error=0) {
        if (failure) *failure={stage,error};
        return Observation{UncertainWindow{handle}};
    };
    if (!hwnd || !IsWindow(hwnd)) {
        if (failure) *failure={NativeQueryStage::Window,ERROR_INVALID_WINDOW_HANDLE};
        return DestroyedWindow{handle};
    }
    const PhysicalCoordinates coordinates;
    if (!coordinates.Valid()) {
        return failed(NativeQueryStage::DpiContext,GetLastError());
    }
    DWORD pid{};
    const DWORD threadId = GetWindowThreadProcessId(hwnd, &pid);
    const auto threadError=threadId && pid ? DWORD{} : GetLastError();
    const auto processStart = ProcessStart(pid,failure);
    wchar_t className[256]{};
    LONG_PTR style{}, extendedStyle{};
    RECT rect{}, outer{};
    DWORD cloaked{};
    const UINT dpi = GetDpiForWindow(hwnd);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(MONITORINFO)};
    if (!threadId || !pid) return failed(NativeQueryStage::WindowProcess,threadError);
    if (!processStart) return FailedQuery(hwnd);
    if (!dpi) return failed(NativeQueryStage::Dpi,GetLastError());
    if (!GetClassNameW(hwnd,className,static_cast<int>(std::size(className))))
        return failed(NativeQueryStage::Class,GetLastError());
    if (!ReadStyle(hwnd,GWL_STYLE,style)) return failed(NativeQueryStage::Style,GetLastError());
    if (!ReadStyle(hwnd,GWL_EXSTYLE,extendedStyle)) return failed(NativeQueryStage::ExtendedStyle,GetLastError());
    QueryCounts().dwmReads+=2;
    const auto frameResult=DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&rect,sizeof(rect));
    if (FAILED(frameResult)) return failed(NativeQueryStage::FrameBounds,0,frameResult);
    const auto cloakResult=DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
    if (FAILED(cloakResult)) return failed(NativeQueryStage::Cloak,0,cloakResult);
    if (!GetWindowRect(hwnd,&outer)) return failed(NativeQueryStage::WindowBounds,GetLastError());
    if (!GetMonitorInfoW(monitor,&monitorInfo)) return failed(NativeQueryStage::Monitor,GetLastError());
    if (rect.right<=rect.left || rect.bottom<=rect.top) return failed(NativeQueryStage::EmptyBounds);
    if (failure) {
        // Earlier successful APIs may leave a nonzero last-error value.
        *failure={};
    }
    DWORD finalPid{};
    wchar_t finalClassName[256]{};
    if (!IsWindow(hwnd)) {
        if (failure) *failure={NativeQueryStage::RevalidateWindow,ERROR_INVALID_WINDOW_HANDLE};
        return DestroyedWindow{handle};
    }
    if (GetWindowThreadProcessId(hwnd,&finalPid)!=threadId || finalPid!=pid)
        return uncertain(NativeQueryStage::RevalidateProcess,GetLastError());
    const auto finalStart=ProcessStart(finalPid,failure);
    if (!finalStart) return UncertainWindow{handle};
    if (finalStart!=processStart) return uncertain(NativeQueryStage::RevalidateProcess);
    if (!GetClassNameW(hwnd,finalClassName,static_cast<int>(std::size(finalClassName))))
        return uncertain(NativeQueryStage::RevalidateClass,GetLastError());
    if (std::wcscmp(className,finalClassName)!=0) return uncertain(NativeQueryStage::RevalidateClass);
    const auto& bounds = monitorInfo.rcMonitor;
    const bool fullscreen = outer.left <= bounds.left && outer.top <= bounds.top
        && outer.right >= bounds.right && outer.bottom >= bounds.bottom;
    const bool ordinary = OrdinaryTopLevel(hwnd, className, style, extendedStyle);
    return AliveWindow{
        handle, {pid, processStart, ObservedIdentity(threadId, className)}, className,
        {rect.left, rect.top, rect.right, rect.bottom}, dpi, ordinary,
        IsWindowVisible(hwnd) != FALSE, IsIconic(hwnd) != FALSE, cloaked != 0,
        IsZoomed(hwnd) != FALSE, fullscreen, (extendedStyle & WS_EX_TOPMOST) != 0,
    };
}

bool WindowTracker::Start(ActionBatch& initialActions) {
    DWORD receiverPid{};
    const auto receiverThread = GetWindowThreadProcessId(receiver_, &receiverPid);
    if (!OnOwnerThread() || started_ || receiverThread != ownerThreadId_
        || receiverPid != GetCurrentProcessId()) {
        stats_.lastWin32Error = ERROR_INVALID_PARAMETER;
        return false;
    }
    // Fixture mode seals the identity as well as the numeric HWND. Reuse cannot broaden the fixture.
    if (config_.mode == TrackingMode::FixtureOnly) {
        if (!config_.initialFixtureIdentity) {
            stats_.lastWin32Error = ERROR_INVALID_PARAMETER;
            return false;
        }
        if (config_.mode==TrackingMode::SelectedOnly) {
            for (const auto& selected:config_.selectedTargets) {
                const auto observation=Query(reinterpret_cast<HWND>(selected.hwnd));
                const auto eligibility=Evaluate(observation,model_.GetPolicy());
                if (eligibility!=Eligibility::Eligible && eligibility!=Eligibility::Suppressed) {
                    stats_.lastWin32Error=ERROR_INVALID_WINDOW_HANDLE;
                    return false;
                }
            }
        }
        auto policy = MakePolicy(config_);
        const auto observation = Query(config_.fixtureTarget);
        if (Evaluate(observation, policy) != Eligibility::Eligible
            && Evaluate(observation, policy) != Eligibility::Suppressed) {
            stats_.lastWin32Error = ERROR_INVALID_WINDOW_HANDLE;
            return false;
        }
        const auto& window = std::get<AliveWindow>(observation);
        wchar_t title[128]{};
        if (!GetWindowTextW(config_.fixtureTarget, title, static_cast<int>(std::size(title)))
            || !MatchesFixtureSelection(window, *config_.initialFixtureIdentity, title)) {
            stats_.lastWin32Error = ERROR_INVALID_WINDOW_HANDLE;
            return false;
        }
        if (!model_.SealFixtureIdentity(*config_.initialFixtureIdentity)) {
            stats_.lastWin32Error = ERROR_INVALID_PARAMETER;
            return false;
        }
    }

    static constexpr std::array events{
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND,
        EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        EVENT_OBJECT_CREATE, EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE,
        EVENT_OBJECT_REORDER, EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
    };
    try {
        hooks_.reserve(events.size());
        for (const DWORD event : events) {
            const auto hook = SetWinEventHook(event, event, nullptr, WinEventProc, 0, 0,
                                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (!hook) {
                stats_.lastWin32Error = GetLastError();
                ShutdownHooks();
                return false;
            }
            hooks_.push_back(hook);
            hookOwners.emplace(hook, this);
        }
        if (!SetTimer(receiver_, TrackerTimerId, TrackerTimerPeriodMs, nullptr)) {
            stats_.lastWin32Error = GetLastError();
            ShutdownHooks();
            return false;
        }
        started_ = true;
        if (config_.mode==TrackingMode::SelectedOnly) {
            const auto now=GetTickCount64();
            for (const auto& selected:config_.selectedTargets) {
                const auto observation=Query(reinterpret_cast<HWND>(selected.hwnd));
                const auto eligibility=Evaluate(observation,model_.GetPolicy());
                if (eligibility!=Eligibility::Eligible && eligibility!=Eligibility::Suppressed) {
                    stats_.lastWin32Error=ERROR_INVALID_WINDOW_HANDLE;
                    ShutdownHooks();
                    return false;
                }
                Append(initialActions,model_.Observe(observation,Dirty::Lifecycle,now));
            }
        }
        reconciliationRequested_ = true;
        const auto now = GetTickCount64();
        Reconcile(now);
        Drain(initialActions, now);
        return true;
    } catch (...) {
        stats_.lastWin32Error = ERROR_NOT_ENOUGH_MEMORY;
        ShutdownHooks();
        return false;
    }
}

void WindowTracker::ShutdownHooks() noexcept {
    if (!OnOwnerThread()) {
        return;
    }
    if (started_) {
        KillTimer(receiver_, TrackerTimerId);
    }
    started_ = false;
    for (const auto hook : hooks_) {
        hookOwners.erase(hook);
        UnhookWinEvent(hook);
    }
    hooks_.clear();
    pending_.clear();
    queue_.clear();
    geometryReprobe_.Clear();
    reconciliation_.clear();
    reconciliationIndex_ = 0;
    dispatchPosted_ = false;
    nextReconciliation_ = 0;
}

ActionBatch WindowTracker::Stop() {
    if (!OnOwnerThread()) {
        return {};
    }
    ShutdownHooks();
    return model_.Clear();
}

void CALLBACK WindowTracker::WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                         LONG objectId, LONG childId, DWORD threadId, DWORD eventTime) {
    (void)threadId;
    const auto found = hookOwners.find(hook);
    if (found != hookOwners.end()) {
        found->second->OnWinEvent(event, hwnd, objectId, childId, eventTime);
    }
}

void WindowTracker::OnWinEvent(DWORD event, HWND hwnd, LONG objectId, LONG childId,
                              DWORD eventTime) noexcept {
    if (!started_ || !hwnd || objectId != OBJID_WINDOW || childId != CHILDID_SELF
        || hwnd == GetDesktopWindow() || hwnd == GetShellWindow()
        || (config_.mode == TrackingMode::FixtureOnly && hwnd != config_.fixtureTarget)) {
        return;
    }
    if (config_.mode==TrackingMode::SelectedOnly &&
        !model_.SelectionActive(reinterpret_cast<WindowHandle>(hwnd))) return;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()
        || std::find(config_.excludedPids.begin(), config_.excludedPids.end(), pid)
            != config_.excludedPids.end()) {
        return;
    }
    ++stats_.callbacks;
    Dirty dirty = Dirty::Lifecycle;
    switch (event) {
    case EVENT_OBJECT_LOCATIONCHANGE: dirty = Dirty::Geometry; break;
    case EVENT_SYSTEM_FOREGROUND:
    case EVENT_OBJECT_REORDER: dirty = Dirty::Ordering; break;
    case EVENT_SYSTEM_MOVESIZESTART: dirty = Dirty::MoveSizeStart | Dirty::Geometry; break;
    case EVENT_SYSTEM_MOVESIZEEND: dirty = Dirty::MoveSizeEnd | Dirty::Geometry; break;
    default: break;
    }
    try {
        Enqueue(reinterpret_cast<WindowHandle>(hwnd), dirty,
                event == EVENT_OBJECT_CREATE ? std::optional{EventTimestamp(eventTime)} : std::nullopt,
                Has(dirty, Dirty::Geometry), std::nullopt,
                config_.mode==TrackingMode::SelectedOnly && event==EVENT_OBJECT_DESTROY);
    } catch (...) {
        ++stats_.queueOverflows;
        reconciliationRequested_ = true;
        stats_.lastWin32Error = ERROR_NOT_ENOUGH_MEMORY;
    }
}

void WindowTracker::PostDispatch() noexcept {
    if (started_ && !dispatchPosted_ && !queue_.empty()) {
        dispatchPosted_ = PostMessageW(receiver_, TrackerDispatchMessage, 0, 0) != FALSE;
        if (!dispatchPosted_) {
            stats_.lastWin32Error = GetLastError();
        }
    }
}

void WindowTracker::Enqueue(WindowHandle hwnd, Dirty dirty, std::optional<Milliseconds> createdAt,
                            bool geometryHint, std::optional<TargetId> reprobeTarget, bool destroyed) {
    if (config_.mode==TrackingMode::SelectedOnly && !model_.SelectionActive(hwnd)) return;
    if (auto found = pending_.find(hwnd); found != pending_.end()) {
        found->second.dirty = found->second.dirty | dirty;
        found->second.geometryHint = found->second.geometryHint || geometryHint;
        found->second.destroyed = found->second.destroyed || destroyed;
        if (!reprobeTarget) found->second.reprobeTarget.reset();
        if (createdAt && (!found->second.createdAt || *createdAt > *found->second.createdAt)) {
            found->second.createdAt = createdAt;
        }
        ++stats_.coalesced;
    } else if (pending_.size() < TrackerQueueLimit
               && (model_.Current(hwnd) || pending_.size() < TrackerQueueLimit - TrackerTargetLimit)) {
        // Insert both structures atomically, including allocation failure.
        pending_.emplace(hwnd, Pending{dirty, createdAt, geometryHint, reprobeTarget, destroyed});
        try {
            queue_.push_back(hwnd);
        } catch (...) {
            pending_.erase(hwnd);
            throw;
        }
    } else {
        ++stats_.queueOverflows;
        reconciliationRequested_ = true;
    }
    PostDispatch();
}

void WindowTracker::Probe(WindowHandle hwnd, const Pending& pending, Milliseconds now,
                          ActionBatch& actions) {
    if (pending.reprobeTarget && model_.Current(hwnd) != pending.reprobeTarget) return;
    if (pending.destroyed) {
        Append(actions,model_.Observe(DestroyedWindow{hwnd},Dirty::Lifecycle,now));
        geometryReprobe_.Cancel(hwnd);
        return;
    }
    // Unknown plainly-unsupported windows would only yield Unsupported, which creates no model state.
    if (config_.mode==TrackingMode::Production && !model_.Current(hwnd)
        && PlainlyUnsupported(reinterpret_cast<HWND>(hwnd))) {
        geometryReprobe_.Cancel(hwnd);
        return;
    }
    const auto observation = Query(reinterpret_cast<HWND>(hwnd));
    if (std::holds_alternative<UncertainWindow>(observation)) {
        ++stats_.uncertainQueries;
    }
    Append(actions, model_.Observe(observation, pending.dirty, now, pending.createdAt));
    const auto current = model_.Current(hwnd);
    if (!current) geometryReprobe_.Cancel(hwnd);
    else if (pending.geometryHint && !geometryReprobe_.Request(*current, now)) {
        ++stats_.queueOverflows;
        reconciliationRequested_ = true;
    }
}

void WindowTracker::Drain(ActionBatch& actions, Milliseconds now) {
    for (std::size_t work = 0; work < TrackerDispatchBudget && !queue_.empty(); ++work) {
        const auto hwnd = queue_.front();
        queue_.pop_front();
        const auto found = pending_.find(hwnd);
        const auto pending = found->second;
        pending_.erase(found);
        Probe(hwnd, pending, now, actions);
    }
    PostDispatch();
}

void WindowTracker::Reconcile(Milliseconds now) {
    if (reconciliationIndex_ >= reconciliation_.size()
        && (reconciliationRequested_ || now >= nextReconciliation_)) {
        reconciliation_ = model_.Handles();
        if (config_.mode==TrackingMode::SelectedOnly) {
            reconciliation_.clear();
            for (const auto& selected:config_.selectedTargets)
                if (model_.SelectionActive(selected.hwnd)) reconciliation_.push_back(selected.hwnd);
        } else if (config_.mode == TrackingMode::FixtureOnly) {
            if (reconciliation_.empty()) {
                reconciliation_.push_back(reinterpret_cast<WindowHandle>(config_.fixtureTarget));
            }
        } else {
            std::unordered_set<WindowHandle> seen(reconciliation_.begin(), reconciliation_.end());
            Enumeration enumeration{&reconciliation_, &seen};
            EnumWindows(EnumerateWindow, reinterpret_cast<LPARAM>(&enumeration));
            if (enumeration.truncated) {
                ++stats_.enumerationTruncations;
            }
            if (enumeration.allocationFailed) {
                stats_.lastWin32Error = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
        reconciliationIndex_ = 0;
        nextReconciliation_ = now + TrackerReconcilePeriodMs;
        reconciliationRequested_ = false;
        ++stats_.reconciliations;
    }
    for (std::size_t work = 0; work < TrackerReconcileBudget
         && reconciliationIndex_ < reconciliation_.size(); ++work) {
        Enqueue(reconciliation_[reconciliationIndex_++], Dirty::Lifecycle);
    }
}

bool WindowTracker::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, ActionBatch& actions) {
    (void)lParam;
    if (!OnOwnerThread() || !started_) {
        return false;
    }
    const auto now = GetTickCount64();
    if (message == TrackerDispatchMessage) {
        dispatchPosted_ = false;
        Drain(actions, now);
        return true;
    }
    if (message == WM_TIMER && wParam == TrackerTimerId) {
        for (const auto& target : geometryReprobe_.TakeDue(now)) {
            if (model_.Current(target.hwnd) == target)
                Enqueue(target.hwnd, Dirty::Geometry, std::nullopt, false, target);
        }
        for (const auto hwnd : model_.SettlingHandles()) {
            Enqueue(hwnd, Dirty::Settlement);
        }
        Reconcile(now);
        Drain(actions, now); // Also recovers a failed PostMessage.
        return true;
    }
    return false;
}

void WindowTracker::InvalidateAppearance() {
    if (!OnOwnerThread() || !started_) {
        return;
    }
    for (const auto hwnd : model_.Handles()) {
        Enqueue(hwnd, Dirty::Appearance);
    }
}

bool WindowTracker::ValidateWork(const WorkToken& token, ActionBatch& updates) {
    if (!OnOwnerThread() || !started_ || model_.Current(token.target.hwnd) != token.target) {
        return false;
    }
    const auto found = pending_.find(token.target.hwnd);
    const auto pending = found != pending_.end() ? found->second : Pending{};
    if (found != pending_.end()) {
        pending_.erase(found);
        const auto queued = std::find(queue_.begin(), queue_.end(), token.target.hwnd);
        if (queued != queue_.end()) {
            queue_.erase(queued);
        }
    }
    Probe(token.target.hwnd, pending, GetTickCount64(), updates);
    return model_.Accepts(token);
}

} // namespace window_sweaters::tracking
