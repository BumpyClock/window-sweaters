// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "KnitPreparation.h"
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
    std::size_t maxTargetKeys{4096};
};

enum class RenderQueueState { Created, Running, Faulted, Stopped };
struct RenderQueueStatus {
    RenderQueueState state{RenderQueueState::Created};
    HRESULT error{S_OK};
    std::size_t pending{}, maxPending{}, pendingBytes{}, maxPendingBytes{};
    std::size_t ready{}, maxReady{}, readyBytes{}, maxReadyBytes{};
    std::size_t active{};
    std::size_t transientBytes{}, maxTransientBytes{}, residentCacheBytes{};
    std::uint64_t renderRequests{}, acceptedRequests{}, renderPreparations{};
    std::uint64_t coalesced{}, staleCancelled{}, quotaRejected{};
    std::uint64_t tileGenerations{}, cacheHits{}, notifications{}, notificationFailures{};
    bool notificationPending{};
};

#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
struct RenderQueueTestHooks {
    std::function<void(std::stop_token)> beforeFirstDequeue;
    std::function<void(const RenderRequest&, std::stop_token)> beforeRender;
    std::function<void(const RenderRequest&, std::stop_token)> afterRender;
    std::function<void(const RenderRequest&, std::uint32_t poll, std::stop_token)> duringRender;
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

    HRESULT Start(std::uint64_t configRevision, std::uint64_t deviceGeneration) noexcept;
    HRESULT Submit(const RenderRequest& request) noexcept;
    HRESULT Cancel(const tracking::TargetId& target) noexcept;
    HRESULT Invalidate(std::uint64_t configRevision, std::uint64_t deviceGeneration) noexcept;
    bool OwnsNotification(WPARAM cookie, LPARAM reserved) const noexcept;
    HRESULT Drain(WPARAM cookie, LPARAM reserved, std::vector<RenderReady>& results) noexcept;
    HRESULT Stop() noexcept;
    RenderQueueStatus Status() const noexcept;

private:
    struct Impl;
    explicit RenderQueue(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}
