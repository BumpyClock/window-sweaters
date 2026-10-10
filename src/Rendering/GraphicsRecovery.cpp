// SPDX-License-Identifier: GPL-3.0-only
#include "GraphicsRecovery.h"
#include <limits>

namespace window_sweaters::rendering {
namespace {
constexpr std::array retryDelays{
    std::chrono::milliseconds{100},
    std::chrono::milliseconds{500},
    std::chrono::milliseconds{2000},
};
constexpr HRESULT arithmeticOverflow = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
}

bool GraphicsRecovery::IsRecoverable(HRESULT result) noexcept {
    switch (result) {
    case D2DERR_RECREATE_TARGET:
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return true;
    default:
        return false;
    }
}

bool GraphicsRecovery::CanRender(std::uint64_t generation) const noexcept {
    return state_ == RecoveryState::Ready && generation == generation_;
}

RecoveryUpdate GraphicsRecovery::Update() const noexcept {
    RecoveryUpdate update;
    update.generation = generation_;
    update.error = lastError_;
    return update;
}

void GraphicsRecovery::MakeUnavailable(HRESULT error, RecoveryUpdate& update) noexcept {
    state_ = RecoveryState::Unavailable;
    work_ = std::monostate{};
    lastError_ = error;
    update.error = error;
    update.Add(RecoveryAction::NotifyUnavailable);
}

void GraphicsRecovery::ScheduleRetry(std::uint32_t number, TimePoint now, RecoveryUpdate& update) noexcept {
    const auto delay = std::chrono::duration_cast<Clock::duration>(retryDelays[number - 1]);
    if (now > TimePoint::max() - delay) {
        MakeUnavailable(arithmeticOverflow, update);
        return;
    }
    const RecoveryRetry retry{{generation_, number}, now + delay};
    work_ = retry;
    update.retry = retry;
}

RecoveryUpdate GraphicsRecovery::BeginFault(HRESULT result, std::uint64_t observedGeneration,
    TimePoint now) noexcept {
    if (state_ != RecoveryState::Ready || observedGeneration != generation_ || SUCCEEDED(result))
        return Update();

    lastError_ = result;
    state_ = RecoveryState::Recovering;
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        auto update = Update();
        update.Add(RecoveryAction::HideCurrent);
        MakeUnavailable(arithmeticOverflow, update);
        return update;
    }
    ++generation_;
    auto update = Update();
    update.Add(RecoveryAction::HideCurrent);
    if (!IsRecoverable(result)) {
        MakeUnavailable(result, update);
        return update;
    }
    ScheduleRetry(1, now, update);
    return update;
}

RecoveryUpdate GraphicsRecovery::RetryDue(TimePoint now, const RecoveryRetry& ticket) noexcept {
    const auto* pending = std::get_if<RecoveryRetry>(&work_);
    if (state_ != RecoveryState::Recovering || !pending || *pending != ticket || now < pending->due)
        return Update();

    const auto attempt = pending->attempt;
    work_ = attempt;
    auto update = Update();
    update.initialization = attempt;
    update.Add(RecoveryAction::InitDevice);
    return update;
}

RecoveryUpdate GraphicsRecovery::CompleteInitialization(const RecoveryAttempt& attempt, HRESULT result,
    TimePoint now) noexcept {
    const auto* active = std::get_if<RecoveryAttempt>(&work_);
    if (state_ != RecoveryState::Recovering || !active || *active != attempt)
        return Update();

    work_ = std::monostate{};
    lastError_ = result;
    auto update = Update();
    if (SUCCEEDED(result)) {
        state_ = RecoveryState::Ready;
        update.Add(RecoveryAction::RebuildCurrent);
    } else if (!IsRecoverable(result) || attempt.number >= retryDelays.size()) {
        MakeUnavailable(result, update);
    } else {
        ScheduleRetry(attempt.number + 1, now, update);
    }
    return update;
}

RecoveryUpdate GraphicsRecovery::Stop() noexcept {
    if (state_ == RecoveryState::Stopped) return Update();
    state_ = RecoveryState::Stopped;
    work_ = std::monostate{};
    if (generation_ != std::numeric_limits<std::uint64_t>::max()) ++generation_;
    auto update = Update();
    update.Add(RecoveryAction::HideCurrent);
    return update;
}

}
