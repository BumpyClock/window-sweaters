#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "TrackingModel.h"
#include "GeometryReprobe.h"

#include <deque>
#include <unordered_map>

namespace window_sweaters::tracking {

inline constexpr UINT TrackerDispatchMessage = WM_APP + 20;
inline constexpr UINT_PTR TrackerTimerId = 0x5753;
inline constexpr UINT TrackerTimerPeriodMs = 100;
inline constexpr Milliseconds TrackerReconcilePeriodMs = 2000;
inline constexpr std::size_t TrackerQueueLimit = 1024;
inline constexpr std::size_t TrackerTargetLimit = 512;
inline constexpr std::size_t TrackerDispatchBudget = 64;
inline constexpr std::size_t TrackerReconcileBudget = 32;
inline constexpr std::size_t TrackerEnumerationLimit = 4096;

struct TrackerConfig {
    TrackingMode mode{TrackingMode::Production};
    HWND fixtureTarget{};
    std::optional<WindowIdentity> initialFixtureIdentity;
    std::vector<std::uint32_t> excludedPids;
    std::vector<std::wstring> excludedClasses;
    std::size_t maxTargets{512};
    Milliseconds resizeQuietPeriod{250};
    std::vector<WindowSelection> selectedTargets;
};

struct TrackerStats {
    std::uint64_t callbacks{};
    std::uint64_t coalesced{};
    std::uint64_t queueOverflows{};
    std::uint64_t uncertainQueries{};
    std::uint64_t reconciliations{};
    std::uint64_t enumerationTruncations{};
    DWORD lastWin32Error{};
};

// Receiver HWND, all calls, hook installation/removal and destruction belong to one UI thread.
// No overlay ownership and no worker callbacks live here.
class WindowTracker final {
public:
    explicit WindowTracker(HWND receiver, TrackerConfig config = {});
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;
    WindowTracker(WindowTracker&&) = delete;
    WindowTracker& operator=(WindowTracker&&) = delete;

    bool Start(ActionBatch& initialActions);
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, ActionBatch& actions);
    ActionBatch Stop();
    void InvalidateAppearance();
    bool ValidateWork(const WorkToken& token, ActionBatch& updates);
    const TrackerStats& Stats() const noexcept { return stats_; }
    const TrackingModel& Model() const noexcept { return model_; }

    static Observation Query(HWND hwnd);

private:
    struct Pending {
        Dirty dirty{Dirty::None};
        std::optional<Milliseconds> createdAt;
        bool geometryHint{};
        std::optional<TargetId> reprobeTarget;
        bool destroyed{};
    };
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG objectId, LONG childId, DWORD threadId, DWORD eventTime);
    void OnWinEvent(DWORD event, HWND hwnd, LONG objectId, LONG childId, DWORD eventTime) noexcept;
    void Enqueue(WindowHandle hwnd, Dirty dirty, std::optional<Milliseconds> createdAt = std::nullopt,
                 bool geometryHint = false, std::optional<TargetId> reprobeTarget = std::nullopt,
                 bool destroyed = false);
    void PostDispatch() noexcept;
    void Drain(ActionBatch& actions, Milliseconds now);
    void Reconcile(Milliseconds now);
    void Probe(WindowHandle hwnd, const Pending& pending, Milliseconds now, ActionBatch& actions);
    void ShutdownHooks() noexcept;
    bool OnOwnerThread() const noexcept;

    HWND receiver_{};
    DWORD ownerThreadId_{};
    TrackerConfig config_;
    TrackingModel model_;
    GeometryReprobe geometryReprobe_;
    TrackerStats stats_;
    std::vector<HWINEVENTHOOK> hooks_;
    std::unordered_map<WindowHandle, Pending> pending_;
    std::deque<WindowHandle> queue_;
    std::vector<WindowHandle> reconciliation_;
    std::size_t reconciliationIndex_{};
    Milliseconds nextReconciliation_{};
    bool started_{};
    bool dispatchPosted_{};
    bool reconciliationRequested_{};
};

} // namespace window_sweaters::tracking
