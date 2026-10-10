// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "KnitRaster.h"
#include "..\Tracking\TrackingModel.h"
#include <windows.h>
#include <memory>
#include <variant>
#include <vector>

#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
#include <functional>
#include <stop_token>
#endif

namespace window_sweaters::rendering {

// Submit copies this value into shared, const storage. No live host/model/device
// references cross the worker boundary; placement does not affect the tile key.
struct RenderRequest {
    tracking::WorkToken token;
    std::uint64_t configRevision{};
    std::uint64_t deviceGeneration{};
    tracking::PhysicalRect targetBounds;
    tracking::PhysicalRect clipRelativeToOuter;
    KnitFrameSpec frame;
    knitting::TileSpec tile;
};

enum class RenderErrorKind { InvalidInput, ResourceQuota, NativeFailure };
struct RenderError {
    RenderErrorKind kind;
    HRESULT result;
};
struct RenderReady {
    std::shared_ptr<const RenderRequest> request;
    std::variant<std::shared_ptr<const KnitPerimeter>, RenderError> outcome;
};

struct RenderQueueLimits {
    std::size_t maxPending{32};
    std::size_t maxPendingBytes{1024u * 1024u};
    std::size_t maxReady{32};
    std::size_t maxReadyBytes{64u * 1024u * 1024u};
    std::size_t maxPerimeterBytes{64u * 1024u * 1024u};
    std::size_t maxTransientBytes{128u * 1024u * 1024u};
    std::size_t cacheBytes{8u * 1024u * 1024u};
    // Bounded lifetime high-water ledger. Exhaustion rejects new HWND keys,
    // rather than forgetting tombstones and resurrecting cancelled work.
    std::size_t maxTargetKeys{4096};
};

enum class RenderQueueState { Created, Running, Faulted, Stopped };
struct RenderQueueStatus {
    RenderQueueState state{RenderQueueState::Created};
    HRESULT error{S_OK};
    std::size_t pending{}, maxPending{}, pendingBytes{}, maxPendingBytes{};
    std::size_t ready{}, maxReady{}, readyBytes{}, maxReadyBytes{};
    std::size_t active{};
    // Reservations bound pixel buffers, tile scratch, retained tiles and input.
    // They exclude opaque D2D/WIC allocator overhead, not a measured heap total.
    std::size_t transientBytes{}, maxTransientBytes{}, residentCacheBytes{};
    std::uint64_t renderRequests{}, acceptedRequests{}, renderPreparations{};
    std::uint64_t coalesced{}, staleCancelled{}, quotaRejected{};
    std::uint64_t tileGenerations{}, cacheHits{}, notifications{}, notificationFailures{};
    bool notificationPending{};
};

#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
// Gates only surround the real production renderer; there is no substitute
// preparation implementation. Gates must release when stop is requested.
struct RenderQueueTestHooks {
    std::function<void(std::stop_token)> beforeFirstDequeue;
    std::function<void(const RenderRequest&, std::stop_token)> beforeRender;
    std::function<void(const RenderRequest&, std::stop_token)> afterRender;
};
#endif

class RenderQueue final {
public:
    static constexpr UINT ReadyMessage = WM_APP + 30;
    static HRESULT Create(HWND receiver, const RenderQueueLimits& limits,
                          std::unique_ptr<RenderQueue>& queue
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
                          , RenderQueueTestHooks hooks = {}
#endif
                          ) noexcept;
    ~RenderQueue();
    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    // One queue per host, one MTA worker and one worker-owned TileCache.
    // Create, Start and all mutations/drains are receiver-owner-thread only.
    // Configuration revision zero is the valid cold-start/default snapshot;
    // deviceGeneration must be nonzero.
    HRESULT Start(std::uint64_t configRevision, std::uint64_t deviceGeneration) noexcept;
    // S_OK opens/replaces a host pending gate; S_FALSE/FAILED opens none.
    // Every accepted current job completes, with sleeping ready backpressure.
    // Coalescing retires earlier same-target work. Cancel/Invalidate/Stop and
    // terminal Status.error explicitly retire affected host gates; the host
    // must retire those gates itself, not wait for a cancelled completion.
    // S_FALSE rejects stale/cancelled tokens. Quota failures are explicit HRESULTs.
    HRESULT Submit(const RenderRequest& request) noexcept;
    // Successful cancellation also seals an as-yet-unsubmitted newer lifetime.
    // Old PID/start/generation cancellation cannot retire a reused HWND.
    HRESULT Cancel(const tracking::TargetId& target) noexcept;
    // Revisions must advance monotonically; old pending/active/ready work dies.
    HRESULT Invalidate(std::uint64_t configRevision, std::uint64_t deviceGeneration) noexcept;
    bool OwnsNotification(WPARAM cookie, LPARAM reserved) const noexcept;
    // Call from the consumed ReadyMessage only. No worker invokes host callbacks.
    // Before attachment the host MUST revalidate tracking/model acceptance,
    // config/device generations and current geometry; movement never submits.
    HRESULT Drain(WPARAM cookie, LPARAM reserved, std::vector<RenderReady>& results) noexcept;
    // Receiver must remain alive until Stop returns. Cancels queued jobs, joins
    // the finite in-flight render, then closes/removes reserved notifications.
    // A stopped queue cannot be restarted. Drain output is now host-owned.
    HRESULT Stop() noexcept;
    // Thread-safe. A failed PostMessage faults the queue, releases undeliverable
    // results, and is visible here/at Submit/Drain; no hidden retry or idle timer.
    RenderQueueStatus Status() const noexcept;

private:
    struct Impl;
    explicit RenderQueue(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace window_sweaters::rendering
