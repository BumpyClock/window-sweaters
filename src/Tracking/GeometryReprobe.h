#pragma once

#include "TrackingModel.h"

#include <deque>

namespace window_sweaters::tracking {

inline constexpr Milliseconds GeometryReprobeDelayMs = 100;
inline constexpr unsigned GeometryReprobeAttempts = 3;
inline constexpr std::size_t GeometryReprobeLimit = 512;
inline constexpr std::size_t GeometryReprobeBudget = 32;

// DWM may still report the previous frame when a LOCATIONCHANGE hint is drained.
// Finite delayed owner-thread queries cover that lag without turning movement into rendering.
class GeometryReprobe final {
public:
    bool Request(TargetId target, Milliseconds now) {
        for (auto& retry : retries_) {
            if (retry.target.hwnd == target.hwnd) {
                if (retry.target != target) retry = {target, now + GeometryReprobeDelayMs, GeometryReprobeAttempts};
                else retry.remaining = GeometryReprobeAttempts;
                return true;
            }
        }
        if (retries_.size() >= GeometryReprobeLimit) return false;
        retries_.push_back({target, now + GeometryReprobeDelayMs, GeometryReprobeAttempts});
        return true;
    }

    std::vector<TargetId> TakeDue(Milliseconds now) {
        std::vector<TargetId> result;
        const auto candidates = retries_.size();
        for (std::size_t scan = 0; scan < candidates && result.size() < GeometryReprobeBudget; ++scan) {
            const auto& front = retries_.front();
            if (front.due > now) {
                retries_.push_back(front);
                retries_.pop_front();
                continue;
            }
            result.push_back(front.target);
            if (front.remaining > 1)
                retries_.push_back({front.target, now + GeometryReprobeDelayMs, front.remaining - 1});
            retries_.pop_front();
        }
        return result;
    }

    void Cancel(WindowHandle hwnd) noexcept {
        std::erase_if(retries_, [hwnd](const auto& retry) { return retry.target.hwnd == hwnd; });
    }
    void Clear() noexcept { retries_.clear(); }
    std::size_t Size() const noexcept { return retries_.size(); }

private:
    struct Retry {
        TargetId target;
        Milliseconds due;
        unsigned remaining;
    };
    std::deque<Retry> retries_;
};

} // namespace window_sweaters::tracking
