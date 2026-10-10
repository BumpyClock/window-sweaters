// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Rendering\GraphicsRecovery.h"
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace window_sweaters::rendering;
using namespace std::chrono_literals;

namespace {
std::size_t assertions{};

void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

GraphicsRecovery::TimePoint At(std::chrono::milliseconds milliseconds) {
    return GraphicsRecovery::TimePoint{milliseconds};
}

void CheckUpdate(const RecoveryUpdate& update, std::initializer_list<RecoveryAction> actions,
    std::uint64_t generation, HRESULT error) {
    Check(std::ranges::equal(update.Actions(), actions), "Exact ordered recovery actions");
    Check(update.generation == generation, "Exact output device generation");
    Check(update.error == error, "Exact output HRESULT, not a silent default");
}

void CheckInert(const RecoveryUpdate& update) {
    Check(update.Actions().empty() && !update.initialization && !update.retry,
        "Stale/duplicate input neither authorizes work nor rearms a timer");
}

RecoveryRetry RequireRetry(const RecoveryUpdate& update, std::uint64_t generation,
    std::uint32_t number, GraphicsRecovery::TimePoint due) {
    Check(update.retry.has_value(), "A bounded retry is explicitly scheduled");
    Check(update.retry->attempt == RecoveryAttempt{generation, number}, "Exact retry attempt token");
    Check(update.retry->due == due, "Exact retry deadline");
    Check(!update.initialization, "Scheduling does not also initialize");
    return *update.retry;
}

RecoveryAttempt RequireInitialization(const RecoveryUpdate& update, std::uint64_t generation,
    std::uint32_t number) {
    Check(update.initialization == RecoveryAttempt{generation, number}, "Exact initialization token");
    Check(!update.retry, "An active initialization has no queued retry");
    return *update.initialization;
}

struct NativeFaultFixture {
    GraphicsRecovery recovery;
    std::vector<HRESULT> initializeResults;
    std::vector<RecoveryAction> actions;
    std::vector<GraphicsRecovery::TimePoint> initializeTimes;
    std::optional<RecoveryRetry> timer;
    std::size_t initializations{};
    DWORD ownerThread{GetCurrentThreadId()};

    explicit NativeFaultFixture(std::initializer_list<HRESULT> results) : initializeResults(results) {}

    void Apply(const RecoveryUpdate& update, GraphicsRecovery::TimePoint now) {
        Check(GetCurrentThreadId() == ownerThread, "Fake device initialization stays on its owner thread");
        for (const auto action : update.Actions()) {
            actions.push_back(action);
            switch (action) {
            case RecoveryAction::HideCurrent:
                timer.reset();
                Check(!recovery.CanRender(update.generation), "Hide precedes any new render authority");
                break;
            case RecoveryAction::InitDevice: {
                Check(update.initialization.has_value(), "InitDevice always carries a completion token");
                Check(initializations < initializeResults.size(), "Unexpected initialization has no fake success fallback");
                timer.reset();
                const auto result = initializeResults[initializations++];
                initializeTimes.push_back(now);
                std::cout << "INJECT Initialize generation=" << update.generation
                    << " attempt=" << update.initialization->number << " HRESULT=0x"
                    << std::hex << static_cast<std::uint32_t>(result) << std::dec
                    << " result=" << (SUCCEEDED(result) ? "success" : "failure")
                    << " recoverable=" << (GraphicsRecovery::IsRecoverable(result) ? "yes" : "no") << '\n';
                Apply(recovery.CompleteInitialization(*update.initialization, result, now), now);
                break;
            }
            case RecoveryAction::RebuildCurrent:
                timer.reset();
                Check(recovery.CanRender(update.generation), "Rebuild carries current device authority only");
                break;
            case RecoveryAction::NotifyUnavailable:
                timer.reset();
                Check(recovery.State() == RecoveryState::Unavailable && FAILED(update.error),
                    "Unavailable is explicit and diagnosable");
                std::cout << "UNAVAILABLE generation=" << update.generation << " HRESULT=0x"
                    << std::hex << static_cast<std::uint32_t>(update.error) << std::dec << '\n';
                break;
            }
        }
        if (update.retry) {
            Check(!timer, "Only one owner timer is outstanding");
            timer = update.retry;
        }
    }

    void Fault(HRESULT result, std::uint64_t generation, GraphicsRecovery::TimePoint now) {
        Apply(recovery.BeginFault(result, generation, now), now);
    }

    void Fire(const RecoveryRetry& ticket, GraphicsRecovery::TimePoint now) {
        Apply(recovery.RetryDue(now, ticket), now);
    }
};

void RecoverableClassificationIsExact() {
    constexpr std::array recoverable{
        std::pair{D2DERR_RECREATE_TARGET, 0x8899000cU},
        std::pair{DXGI_ERROR_DEVICE_REMOVED, 0x887a0005U},
        std::pair{DXGI_ERROR_DEVICE_RESET, 0x887a0007U},
        std::pair{DXGI_ERROR_DEVICE_HUNG, 0x887a0006U},
        std::pair{DXGI_ERROR_DRIVER_INTERNAL_ERROR, 0x887a0020U},
    };
    for (const auto& [result, value] : recoverable) {
        Check(static_cast<std::uint32_t>(result) == value, "Exact documented recoverable HRESULT value");
        Check(GraphicsRecovery::IsRecoverable(result), "The five requested failures are recoverable");
        std::cout << "CLASSIFY HRESULT=0x" << std::hex << value << std::dec << " recoverable=yes\n";
    }
    constexpr std::array rejected{
        S_OK, S_FALSE, DXGI_STATUS_OCCLUDED, E_FAIL, E_INVALIDARG, E_OUTOFMEMORY, E_ACCESSDENIED,
        E_ABORT, E_NOTIMPL, D2DERR_WRONG_STATE, DXGI_ERROR_INVALID_CALL, DXGI_ERROR_UNSUPPORTED,
        DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_WAIT_TIMEOUT, DXGI_ERROR_WAS_STILL_DRAWING,
        DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, DXGI_ERROR_SDK_COMPONENT_MISSING,
        HRESULT_FROM_WIN32(ERROR_CANCELLED), HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW),
        static_cast<HRESULT>(0x8899000dU), static_cast<HRESULT>(0x80001234U),
    };
    for (const auto result : rejected) {
        Check(!GraphicsRecovery::IsRecoverable(result), "Other HRESULTs are not device-recovery permission");
        std::cout << "CLASSIFY HRESULT=0x" << std::hex << static_cast<std::uint32_t>(result)
            << std::dec << " recoverable=no\n";
    }
}

void ReadyAndIgnoredFaults() {
    GraphicsRecovery recovery;
    Check(recovery.State() == RecoveryState::Ready && recovery.Generation() == 1
        && recovery.LastError() == S_OK, "Host generation starts at one with an initialized device");
    Check(recovery.CanRender(1) && !recovery.CanRender(0) && !recovery.CanRender(2),
        "Ready only admits matching device generations");
    for (const auto result : {S_OK, S_FALSE, DXGI_STATUS_OCCLUDED}) {
        CheckInert(recovery.BeginFault(result, 1, At(0ms)));
    }
    CheckInert(recovery.BeginFault(DXGI_ERROR_DEVICE_REMOVED, 0, At(0ms)));
    CheckInert(recovery.BeginFault(E_FAIL, 2, At(0ms)));
    Check(recovery.State() == RecoveryState::Ready && recovery.Generation() == 1
        && recovery.LastError() == S_OK, "Success and stale/future reports do not revoke a current device");
}

void ExactOrderingDeadlinesAndErrors() {
    GraphicsRecovery recovery;
    const auto fault = recovery.BeginFault(D2DERR_RECREATE_TARGET, 1, At(1000ms));
    CheckUpdate(fault, {RecoveryAction::HideCurrent}, 2, D2DERR_RECREATE_TARGET);
    const auto first = RequireRetry(fault, 2, 1, At(1100ms));
    Check(recovery.State() == RecoveryState::Recovering && recovery.Generation() == 2,
        "Fault invalidates generation synchronously before any owner action");
    Check(!recovery.CanRender(1) && !recovery.CanRender(2), "Recovering has no render authority");
    CheckInert(recovery.RetryDue(At(1099ms), first));
    auto update = recovery.RetryDue(At(1100ms), first);
    CheckUpdate(update, {RecoveryAction::InitDevice}, 2, D2DERR_RECREATE_TARGET);
    const auto attempt1 = RequireInitialization(update, 2, 1);
    CheckInert(recovery.RetryDue(At(1200ms), first));
    update = recovery.CompleteInitialization(attempt1, DXGI_ERROR_DEVICE_RESET, At(1110ms));
    CheckUpdate(update, {}, 2, DXGI_ERROR_DEVICE_RESET);
    const auto second = RequireRetry(update, 2, 2, At(1610ms));
    CheckInert(recovery.RetryDue(At(1609ms), second));
    update = recovery.RetryDue(At(1610ms), second);
    CheckUpdate(update, {RecoveryAction::InitDevice}, 2, DXGI_ERROR_DEVICE_RESET);
    const auto attempt2 = RequireInitialization(update, 2, 2);
    update = recovery.CompleteInitialization(attempt2, DXGI_ERROR_DRIVER_INTERNAL_ERROR, At(1700ms));
    CheckUpdate(update, {}, 2, DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    const auto third = RequireRetry(update, 2, 3, At(3700ms));
    CheckInert(recovery.RetryDue(At(3699ms), third));
    update = recovery.RetryDue(At(3700ms), third);
    CheckUpdate(update, {RecoveryAction::InitDevice}, 2, DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    const auto attempt3 = RequireInitialization(update, 2, 3);
    update = recovery.CompleteInitialization(attempt3, DXGI_ERROR_DEVICE_HUNG, At(3701ms));
    CheckUpdate(update, {RecoveryAction::NotifyUnavailable}, 2, DXGI_ERROR_DEVICE_HUNG);
    Check(!update.retry && !update.initialization, "Third failure has no fourth attempt");
    Check(recovery.State() == RecoveryState::Unavailable && recovery.LastError() == DXGI_ERROR_DEVICE_HUNG
        && !recovery.CanRender(2), "Exhaustion keeps the final exact failure and denies rendering");
}

void StormHasThreeInitializationsAndNoHotLoop() {
    NativeFaultFixture fixture{DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_ERROR_DEVICE_HUNG};
    fixture.Fault(DXGI_ERROR_DEVICE_REMOVED, 1, At(0ms));
    const auto first = *fixture.timer;
    for (std::uint32_t index = 0; index < 10000; ++index) {
        fixture.Fault(index % 2 ? E_FAIL : DXGI_ERROR_DEVICE_RESET, index % 2 ? 1 : 2, At(99ms));
        fixture.Fire(first, At(99ms));
    }
    Check(fixture.actions == std::vector{RecoveryAction::HideCurrent} && fixture.initializations == 0
        && fixture.timer == first, "Fault storm coalesces without extra hiding, initialization or deadline extension");
    fixture.Fire(first, At(100ms));
    const auto second = *fixture.timer;
    fixture.Fire(first, At(10000ms));
    Check(fixture.initializations == 1 && fixture.timer == second, "Old timer cannot steal next attempt");
    fixture.Fire(second, At(600ms));
    const auto third = *fixture.timer;
    fixture.Fire(third, At(2600ms));
    Check(fixture.initializeTimes == std::vector{At(100ms), At(600ms), At(2600ms)},
        "Exactly 2600 ms total policy wait for three instantaneous failed initializations");
    Check(fixture.actions == std::vector{RecoveryAction::HideCurrent, RecoveryAction::InitDevice,
        RecoveryAction::InitDevice, RecoveryAction::InitDevice, RecoveryAction::NotifyUnavailable},
        "One hide, three attempts, one failure notification in order");
    for (std::uint32_t index = 0; index < 10000; ++index) {
        fixture.Fault(D2DERR_RECREATE_TARGET, 2, At(100000ms));
        fixture.Fire(third, At(100000ms));
    }
    Check(fixture.initializations == 3 && fixture.actions.size() == 5 && !fixture.timer
        && fixture.recovery.Generation() == 2 && fixture.recovery.State() == RecoveryState::Unavailable,
        "Permanent failure cannot produce a hot loop or per-window retry storm");
}

void SuccessAtEachBoundedAttempt() {
    for (std::uint32_t successfulAttempt = 1; successfulAttempt <= 3; ++successfulAttempt) {
        NativeFaultFixture fixture{};
        fixture.initializeResults.assign(successfulAttempt - 1, DXGI_ERROR_DEVICE_RESET);
        fixture.initializeResults.push_back(S_OK);
        fixture.Fault(D2DERR_RECREATE_TARGET, 1, At(0ms));
        RecoveryRetry lastTimer;
        while (fixture.timer) {
            lastTimer = *fixture.timer;
            fixture.Fire(lastTimer, lastTimer.due);
        }
        Check(fixture.initializations == successfulAttempt && fixture.actions.front() == RecoveryAction::HideCurrent
            && fixture.actions.back() == RecoveryAction::RebuildCurrent && fixture.actions.size() == successfulAttempt + 2,
            "Any of the three attempts may restore exactly once");
        Check(fixture.recovery.State() == RecoveryState::Ready && fixture.recovery.Generation() == 2
            && fixture.recovery.LastError() == S_OK && fixture.recovery.CanRender(2) && !fixture.recovery.CanRender(1),
            "Successful reinitialization admits only the replacement device generation");
        CheckInert(fixture.recovery.RetryDue(At(100000ms), lastTimer));
        CheckInert(fixture.recovery.CompleteInitialization(lastTimer.attempt, E_FAIL, At(100000ms)));
        Check(fixture.recovery.State() == RecoveryState::Ready && fixture.recovery.LastError() == S_OK,
            "Duplicate completion cannot undo a successful rebuild");
    }
}

void RecoverableFaultFamiliesUseProductionRetryPath() {
    for (const auto result : {D2DERR_RECREATE_TARGET, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET,
        DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_DRIVER_INTERNAL_ERROR}) {
        NativeFaultFixture fixture{S_OK};
        fixture.Fault(result, 1, At(0ms));
        Check(fixture.timer.has_value() && fixture.recovery.LastError() == result,
            "Every classified recoverable fault enters the actual production schedule");
        const auto ticket = *fixture.timer;
        fixture.Fire(ticket, At(100ms));
        Check(fixture.initializations == 1 && fixture.recovery.State() == RecoveryState::Ready,
            "Every recoverable fault can use one shared successful recovery job");
    }
}

void NonrecoverableFailuresAreTerminalAndExplicit() {
    for (const auto result : {E_INVALIDARG, E_OUTOFMEMORY, DXGI_ERROR_INVALID_CALL, DXGI_ERROR_UNSUPPORTED}) {
        GraphicsRecovery recovery;
        auto update = recovery.BeginFault(result, 1, At(0ms));
        CheckUpdate(update, {RecoveryAction::HideCurrent, RecoveryAction::NotifyUnavailable}, 2, result);
        Check(!update.retry && recovery.State() == RecoveryState::Unavailable
            && recovery.LastError() == result && !recovery.CanRender(1) && !recovery.CanRender(2),
            "Unknown/nonrecoverable failure hides immediately without retrying");
        CheckInert(recovery.BeginFault(DXGI_ERROR_DEVICE_REMOVED, 2, At(100000ms)));
    }
    NativeFaultFixture fixture{E_OUTOFMEMORY};
    fixture.Fault(D2DERR_RECREATE_TARGET, 1, At(0ms));
    const auto ticket = *fixture.timer;
    fixture.Fire(ticket, ticket.due);
    Check(fixture.actions == std::vector{RecoveryAction::HideCurrent, RecoveryAction::InitDevice,
        RecoveryAction::NotifyUnavailable} && fixture.initializations == 1 && !fixture.timer
        && fixture.recovery.LastError() == E_OUTOFMEMORY,
        "Nonrecoverable Initialize failure terminates at the first attempt with its HRESULT");
}

void TokensRejectEarlyForgedAndLateCompletion() {
    GraphicsRecovery recovery;
    auto update = recovery.BeginFault(DXGI_ERROR_DEVICE_RESET, 1, At(0ms));
    const auto first = *update.retry;
    CheckInert(recovery.CompleteInitialization(first.attempt, S_OK, At(0ms)));
    auto forged = first;
    forged.due = At(99ms);
    CheckInert(recovery.RetryDue(At(100ms), forged));
    forged = first;
    forged.attempt.generation = 3;
    CheckInert(recovery.RetryDue(At(100ms), forged));
    update = recovery.RetryDue(At(100ms), first);
    const auto attempt1 = RequireInitialization(update, 2, 1);
    for (const auto attempt : {RecoveryAttempt{1, 1}, RecoveryAttempt{3, 1}, RecoveryAttempt{2, 0},
        RecoveryAttempt{2, 2}, RecoveryAttempt{2, 4}}) {
        CheckInert(recovery.CompleteInitialization(attempt, S_OK, At(100ms)));
    }
    CheckInert(recovery.BeginFault(E_FAIL, 2, At(200ms)));
    Check(recovery.LastError() == DXGI_ERROR_DEVICE_RESET, "In-flight faults coalesce without replacing cause");
    update = recovery.CompleteInitialization(attempt1, DXGI_ERROR_DEVICE_RESET, At(100ms));
    const auto second = RequireRetry(update, 2, 2, At(600ms));
    CheckInert(recovery.CompleteInitialization(attempt1, S_OK, At(500ms)));
    CheckInert(recovery.CompleteInitialization(second.attempt, S_OK, At(500ms)));
    update = recovery.RetryDue(At(600ms), second);
    const auto attempt2 = RequireInitialization(update, 2, 2);
    CheckInert(recovery.CompleteInitialization(attempt1, S_OK, At(600ms)));
    Check(recovery.State() == RecoveryState::Recovering && !recovery.CanRender(2),
        "A late success from an earlier attempt cannot resurrect Ready");
    update = recovery.CompleteInitialization(attempt2, S_FALSE, At(600ms));
    CheckUpdate(update, {RecoveryAction::RebuildCurrent}, 2, S_FALSE);
    Check(recovery.CanRender(2) && recovery.LastError() == S_FALSE,
        "HRESULT success classification is SUCCEEDED, not only S_OK");
}

void OldCycleCannotResurrectReadyOrAuthorizeOldWork() {
    GraphicsRecovery recovery;
    auto update = recovery.BeginFault(D2DERR_RECREATE_TARGET, 1, At(0ms));
    const auto oldTimer = *update.retry;
    update = recovery.RetryDue(At(100ms), oldTimer);
    const auto oldAttempt = *update.initialization;
    recovery.CompleteInitialization(oldAttempt, S_OK, At(100ms));
    Check(!recovery.CanRender(1), "Canceled old-device CPU output is not upload authority after success");
    update = recovery.BeginFault(DXGI_ERROR_DEVICE_REMOVED, 2, At(200ms));
    const auto currentTimer = RequireRetry(update, 3, 1, At(300ms));
    CheckInert(recovery.RetryDue(At(10000ms), oldTimer));
    CheckInert(recovery.CompleteInitialization(oldAttempt, S_OK, At(250ms)));
    CheckInert(recovery.BeginFault(E_FAIL, 2, At(250ms)));
    update = recovery.RetryDue(At(300ms), currentTimer);
    const auto currentAttempt = RequireInitialization(update, 3, 1);
    CheckInert(recovery.CompleteInitialization(oldAttempt, S_OK, At(300ms)));
    Check(recovery.State() == RecoveryState::Recovering && !recovery.CanRender(2)
        && !recovery.CanRender(3), "Previous cycle cannot restore Ready during a new active recovery");
    update = recovery.CompleteInitialization(currentAttempt, S_OK, At(300ms));
    CheckUpdate(update, {RecoveryAction::RebuildCurrent}, 3, S_OK);
    Check(!recovery.CanRender(1) && !recovery.CanRender(2) && recovery.CanRender(3),
        "Device admission remains separate from Host lifetime/config/input/pause prerequisites");
    CheckInert(recovery.BeginFault(D2DERR_RECREATE_TARGET, 2, At(400ms)));
}

void LateTimerDoesNotCatchUpOrCompressBackoff() {
    GraphicsRecovery recovery;
    const auto fault = recovery.BeginFault(D2DERR_RECREATE_TARGET, 1, At(0ms));
    const auto first = *fault.retry;
    auto update = recovery.RetryDue(At(100000ms), first);
    CheckUpdate(update, {RecoveryAction::InitDevice}, 2, D2DERR_RECREATE_TARGET);
    const auto attempt = RequireInitialization(update, 2, 1);
    CheckInert(recovery.RetryDue(At(100001ms), first));
    update = recovery.CompleteInitialization(attempt, DXGI_ERROR_DEVICE_REMOVED, At(100100ms));
    const auto second = RequireRetry(update, 2, 2, At(100600ms));
    CheckInert(recovery.RetryDue(At(100599ms), second));
    Check(recovery.State() == RecoveryState::Recovering, "Long API latency never consumes several attempts at once");
}

void StopRevokesAllStatesAndCannotRestart() {
    for (const auto startingState : {RecoveryState::Ready, RecoveryState::Recovering,
        RecoveryState::Unavailable}) {
        for (const bool initialize : {false, true}) {
            GraphicsRecovery recovery;
            std::optional<RecoveryRetry> timer;
            std::optional<RecoveryAttempt> attempt;
            if (startingState != RecoveryState::Ready) {
                auto update = recovery.BeginFault(startingState == RecoveryState::Unavailable
                    ? E_FAIL : D2DERR_RECREATE_TARGET, 1, At(0ms));
                timer = update.retry;
                if (initialize && timer) attempt = recovery.RetryDue(At(100ms), *timer).initialization;
            }
            Check(recovery.State() == startingState, "Arrange the requested pre-stop state");
            const auto previousGeneration = recovery.Generation();
            const auto previousError = recovery.LastError();
            const auto stopped = recovery.Stop();
            CheckUpdate(stopped, {RecoveryAction::HideCurrent}, previousGeneration + 1, previousError);
            Check(recovery.State() == RecoveryState::Stopped && !recovery.CanRender(previousGeneration)
                && !recovery.CanRender(previousGeneration + 1), "Stop immediately invalidates all device authority");
            CheckInert(recovery.Stop());
            CheckInert(recovery.BeginFault(DXGI_ERROR_DEVICE_RESET, previousGeneration + 1, At(100000ms)));
            if (timer) CheckInert(recovery.RetryDue(At(100000ms), *timer));
            if (attempt) {
                CheckInert(recovery.CompleteInitialization(*attempt, S_OK, At(100000ms)));
                CheckInert(recovery.CompleteInitialization(*attempt, E_FAIL, At(100000ms)));
            }
            Check(recovery.Generation() == previousGeneration + 1 && recovery.State() == RecoveryState::Stopped,
                "Repeated Stop, pending timers and late completions cannot restart or advance again");
        }
    }
    GraphicsRecovery newController;
    Check(newController.State() == RecoveryState::Ready && newController.CanRender(1),
        "Explicit new controller construction is independent of a stopped controller");
}

void GenerationExhaustionNeverWraps() {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr auto overflow = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    GraphicsRecovery recovery{maximum - 1};
    auto update = recovery.BeginFault(D2DERR_RECREATE_TARGET, maximum - 1, At(0ms));
    CheckUpdate(update, {RecoveryAction::HideCurrent}, maximum, D2DERR_RECREATE_TARGET);
    const auto lastTimer = RequireRetry(update, maximum, 1, At(100ms));
    update = recovery.RetryDue(At(100ms), lastTimer);
    const auto lastAttempt = RequireInitialization(update, maximum, 1);
    recovery.CompleteInitialization(lastAttempt, S_OK, At(100ms));
    Check(recovery.CanRender(maximum), "Last representable replacement generation can succeed");
    update = recovery.BeginFault(DXGI_ERROR_DEVICE_REMOVED, maximum, At(200ms));
    CheckUpdate(update, {RecoveryAction::HideCurrent, RecoveryAction::NotifyUnavailable}, maximum, overflow);
    Check(recovery.State() == RecoveryState::Unavailable && recovery.Generation() == maximum
        && !recovery.CanRender(maximum) && !recovery.CanRender(0) && !update.retry,
        "Generation exhaustion fails closed without reusing generation zero or the old maximum");
    CheckInert(recovery.CompleteInitialization(lastAttempt, S_OK, At(300ms)));
    update = recovery.Stop();
    CheckUpdate(update, {RecoveryAction::HideCurrent}, maximum, overflow);
    Check(recovery.State() == RecoveryState::Stopped && !recovery.CanRender(maximum),
        "Stop also fails closed at the maximum generation");
    CheckInert(recovery.Stop());

    GraphicsRecovery pending{maximum - 1};
    const auto pendingTimer = *pending.BeginFault(D2DERR_RECREATE_TARGET, maximum - 1, At(0ms)).retry;
    pending.Stop();
    CheckInert(pending.RetryDue(At(1000ms), pendingTimer));
    Check(pending.Generation() == maximum && pending.State() == RecoveryState::Stopped,
        "Saturated Stop cancels pending work even when the number cannot advance");

    GraphicsRecovery active{maximum - 1};
    const auto activeTimer = *active.BeginFault(D2DERR_RECREATE_TARGET, maximum - 1, At(0ms)).retry;
    const auto activeAttempt = *active.RetryDue(At(100ms), activeTimer).initialization;
    active.Stop();
    CheckInert(active.CompleteInitialization(activeAttempt, S_OK, At(200ms)));
    Check(active.State() == RecoveryState::Stopped && active.Generation() == maximum
        && !active.CanRender(maximum), "Saturated Stop also rejects a late active success with the same generation");
}

void DeadlineOverflowIsExplicitAndBounded() {
    constexpr auto overflow = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    const auto maximum = GraphicsRecovery::TimePoint::max();
    GraphicsRecovery fits;
    const auto fault = fits.BeginFault(D2DERR_RECREATE_TARGET, 1, maximum - 100ms);
    const auto ticket = RequireRetry(fault, 2, 1, maximum);
    CheckInert(fits.RetryDue(maximum - 1ms, ticket));
    const auto attempt = RequireInitialization(fits.RetryDue(maximum, ticket), 2, 1);
    const auto failed = fits.CompleteInitialization(attempt, DXGI_ERROR_DEVICE_RESET, maximum);
    CheckUpdate(failed, {RecoveryAction::NotifyUnavailable}, 2, overflow);
    Check(!failed.retry && fits.State() == RecoveryState::Unavailable, "Retry deadline overflow cannot create an immediate loop");

    GraphicsRecovery overflows;
    const auto update = overflows.BeginFault(DXGI_ERROR_DEVICE_REMOVED, 1, maximum - 99ms);
    CheckUpdate(update, {RecoveryAction::HideCurrent, RecoveryAction::NotifyUnavailable}, 2, overflow);
    Check(overflows.Generation() == 2 && !overflows.CanRender(1) && !overflows.CanRender(2)
        && !update.retry, "Initial deadline overflow still invalidates and hides before notification");

    GraphicsRecovery minimum;
    const auto earliest = minimum.BeginFault(D2DERR_RECREATE_TARGET, 1, GraphicsRecovery::TimePoint::min());
    RequireRetry(earliest, 2, 1, GraphicsRecovery::TimePoint::min() + 100ms);
}

void SeparateRecoveriesKeepMonotonicGenerations() {
    GraphicsRecovery recovery;
    for (std::uint64_t generation = 1; generation <= 100; ++generation) {
        const auto now = At(std::chrono::milliseconds{static_cast<std::int64_t>(generation * 1000)});
        const auto fault = recovery.BeginFault(D2DERR_RECREATE_TARGET, generation, now);
        const auto ticket = RequireRetry(fault, generation + 1, 1, now + 100ms);
        const auto attempt = RequireInitialization(recovery.RetryDue(now + 100ms, ticket), generation + 1, 1);
        const auto success = recovery.CompleteInitialization(attempt, S_OK, now + 100ms);
        CheckUpdate(success, {RecoveryAction::RebuildCurrent}, generation + 1, S_OK);
        Check(!recovery.CanRender(generation) && recovery.CanRender(generation + 1),
            "New faults reset attempt budget, never device generation or stale-work rejection");
    }
    Check(recovery.Generation() == 101, "One hundred successful recovery cycles advance exactly one hundred generations");
}
}

int main() {
    const std::array tests{
        std::pair{"exact HRESULT classification", RecoverableClassificationIsExact},
        std::pair{"initial Ready and ignored faults", ReadyAndIgnoredFaults},
        std::pair{"generation/action/deadline/error order", ExactOrderingDeadlinesAndErrors},
        std::pair{"storm/three attempts/no hot loop", StormHasThreeInitializationsAndNoHotLoop},
        std::pair{"success at attempts one/two/three", SuccessAtEachBoundedAttempt},
        std::pair{"all recoverable production paths", RecoverableFaultFamiliesUseProductionRetryPath},
        std::pair{"nonrecoverable terminal failure", NonrecoverableFailuresAreTerminalAndExplicit},
        std::pair{"early/forged/late attempt tokens", TokensRejectEarlyForgedAndLateCompletion},
        std::pair{"old cycle/old device work rejection", OldCycleCannotResurrectReadyOrAuthorizeOldWork},
        std::pair{"late timer/Initialize latency budget", LateTimerDoesNotCatchUpOrCompressBackoff},
        std::pair{"Stop cancels and never restarts", StopRevokesAllStatesAndCannotRestart},
        std::pair{"generation overflow saturation", GenerationExhaustionNeverWraps},
        std::pair{"deadline arithmetic overflow", DeadlineOverflowIsExplicitAndBounded},
        std::pair{"monotonic independent recoveries", SeparateRecoveriesKeepMonotonicGenerations},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS " << name << '\n';
        }
        std::cout << tests.size() << " tests, " << assertions
            << " assertions passed; fake Initialize only, no real driver-reset or Host/GPU integration claim\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
