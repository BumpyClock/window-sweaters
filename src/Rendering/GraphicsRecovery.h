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

}
