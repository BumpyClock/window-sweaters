// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace window_sweaters::rendering {

enum class RecoveryState { Ready, Recovering, Unavailable, Stopped };
enum class RecoveryAction { HideCurrent, InitDevice, RebuildCurrent, NotifyUnavailable };

struct RecoveryAttempt {
    std::uint64_t generation{};
    std::uint32_t number{};
    bool operator==(const RecoveryAttempt&) const noexcept = default;
};

struct RecoveryRetry {
    RecoveryAttempt attempt;
    std::chrono::steady_clock::time_point due;
    bool operator==(const RecoveryRetry&) const noexcept = default;
};

struct RecoveryUpdate {
    std::uint64_t generation{};
    HRESULT error{S_OK};
    std::optional<RecoveryAttempt> initialization;
    std::optional<RecoveryRetry> retry;

    std::span<const RecoveryAction> Actions() const noexcept {
        return {actions_.data(), actionCount_};
    }

private:
    friend class GraphicsRecovery;
    void Add(RecoveryAction action) noexcept { actions_[actionCount_++] = action; }
    std::array<RecoveryAction, 2> actions_{};
    std::size_t actionCount_{};
};

// Pure policy, called serially by the Host's graphics-owner thread. Construct
// after initial device creation, using the Host's current generation (normally 1).
// Copy update.generation to the Host before applying actions in order.
// HideCurrent also releases overlay device references before the shared device;
// InitDevice calls GraphicsDevice::Initialize once and returns its HRESULT with
// the exact attempt token to CompleteInitialization. Host discards partial or
// stale initialized resources; the controller owns no resources to release.
// Arm one Host timer only when an update supplies retry; preserve that ticket
// through the timer callback. Delays are 100/500/2000 ms after the fault/previous
// failed completion, excluding Initialize latency. There is no catch-up loop.
// Cancel the timer on Ready/Unavailable/Stop; already-posted stale tickets are inert.
//
// RebuildCurrent means enumerate today's eligible targets, never replay captured
// work. CanRender checks only device authority: Host must additionally revalidate
// target lifetime/event, configuration, render input, pause and visibility tokens
// before uploading/showing. No raw devices, target callbacks or settings live here.
// At UINT64_MAX, generation cannot advance: terminal state still revokes authority.
// Log update.error/LastError on faults and NotifyUnavailable. New construction is
// the only restart after Stopped; Unavailable does not automatically retry.
class GraphicsRecovery final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit GraphicsRecovery(std::uint64_t initialGeneration = 1) noexcept
        : generation_(initialGeneration) {}
    GraphicsRecovery(const GraphicsRecovery&) = delete;
    GraphicsRecovery& operator=(const GraphicsRecovery&) = delete;
    GraphicsRecovery(GraphicsRecovery&&) = delete;
    GraphicsRecovery& operator=(GraphicsRecovery&&) = delete;

    static bool IsRecoverable(HRESULT result) noexcept;
    RecoveryState State() const noexcept { return state_; }
    std::uint64_t Generation() const noexcept { return generation_; }
    HRESULT LastError() const noexcept { return lastError_; }
    bool CanRender(std::uint64_t generation) const noexcept;

    RecoveryUpdate BeginFault(HRESULT result, std::uint64_t observedGeneration, TimePoint now) noexcept;
    RecoveryUpdate RetryDue(TimePoint now, const RecoveryRetry& ticket) noexcept;
    RecoveryUpdate CompleteInitialization(const RecoveryAttempt& attempt, HRESULT result, TimePoint now) noexcept;
    RecoveryUpdate Stop() noexcept;

private:
    RecoveryUpdate Update() const noexcept;
    void ScheduleRetry(std::uint32_t number, TimePoint now, RecoveryUpdate& update) noexcept;
    void MakeUnavailable(HRESULT error, RecoveryUpdate& update) noexcept;

    RecoveryState state_{RecoveryState::Ready};
    std::uint64_t generation_;
    HRESULT lastError_{S_OK};
    std::variant<std::monostate, RecoveryRetry, RecoveryAttempt> work_;
};

} // namespace window_sweaters::rendering
