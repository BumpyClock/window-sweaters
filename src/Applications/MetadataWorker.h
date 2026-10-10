#pragma once

#include "WindowOwnerMetadata.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace window_sweaters::applications {

class MetadataRequest final {
public:
    const TargetToken& Token() const noexcept { return token_; }
    std::uint64_t Sequence() const noexcept { return sequence_; }
    bool operator==(const MetadataRequest&) const = default;

private:
    friend class CompletionGate;
    MetadataRequest(TargetToken token, std::uint64_t sequence);
    TargetToken token_;
    std::uint64_t sequence_;
};

using ImmutableMetadata = std::shared_ptr<const MetadataResult>;
struct MetadataCompletion {
    MetadataRequest request;
    ImmutableMetadata metadata;
};

// Owner-thread authority. Host must supply its current Tracking TargetId+configuration revision to
// Accept, Forget on destruction, and SetConfigurationRevision on committed mutations. Begin before
// Submit; Full/Stopped requires request-specific Retire or a new Begin for a bounded retry. Retire
// every cancellation returned by Stop before reusing this gate. A result never owns a border.
class CompletionGate final {
public:
    explicit CompletionGate(std::uint64_t configurationRevision = 0, std::size_t maxTargets = 512);
    std::optional<MetadataRequest> Begin(const TargetToken& token);
    bool Accept(const MetadataCompletion& completion, const std::optional<TargetToken>& currentTarget);
    bool Retire(const MetadataRequest& request);
    void Forget(tracking::WindowHandle hwnd);
    void SetConfigurationRevision(std::uint64_t revision);
    std::size_t Size() const noexcept { return pending_.size(); }

private:
    std::uint64_t revision_;
    std::uint64_t nextSequence_{1};
    std::size_t maxTargets_;
    std::map<tracking::WindowHandle, MetadataRequest> pending_;
};

struct WorkerLimits {
    std::size_t queuedRequests{64};
    std::size_t completedRequests{64};
    std::size_t cacheEntries{128};
    std::size_t cacheBytes{4 * 1024 * 1024};
    std::chrono::milliseconds cacheLifetime{30000};
};
struct WorkerStatistics {
    std::size_t queued{};
    std::size_t completed{};
    std::size_t cached{};
    std::size_t cacheBytes{};
    std::uint64_t cacheHits{};
    std::uint64_t queries{};
    std::uint64_t coalesced{};
    std::uint64_t unavailableQueries{};
    bool inFlight{};
    bool completionBackpressure{};
};
enum class SubmitResult { Accepted, Coalesced, Full, Obsolete, Stopped };

// One local MTA worker; read-only expected-root HWND operations, no UI callbacks, resource loading
// or external service. Window facts cache only by complete target token, never by process alone.
// One CompletionGate supplies its monotonic sequences. Submit/Drain/Statistics are thread-safe;
// Stop/destruction belong to one owner and must not run in Query. Query owns its dependencies.
// Queue/completion counts and cache count/charged retained bytes are bounded, including negative
// results in completions. No unavailable result is cached. Cache TTL advances only on requests.
// A full completion queue blocks dequeue, never drops accepted current requests. Drain wakes it.
// Stop returns request-specific cancellations for queued/completed/in-flight work and joins the
// current query. Caller retires those requests on its owner-thread gate; destruction without Stop
// is only safe when the gate is also discarded. Documented synchronous shell/process
// APIs have no cancellable hard deadline. Custom Query must be local and bounded, not arbitrary I/O.
// Allocator overhead/process-wide memory is not the cache byte statistic. No recurring timer runs.
class MetadataWorker final {
public:
    using Query = std::function<MetadataResult(const TargetToken&)>;
    using LifetimeQuery = std::function<WindowLifetimeResult(const TargetToken&)>;
    // Custom Query owns before/after validation. Without LifetimeQuery its results are not cached.
    // Supplying LifetimeQuery permits target-scoped caching with matching before/after live facts.
    explicit MetadataWorker(WorkerLimits limits = {}, Query query = {}, LifetimeQuery lifetime = {});
    ~MetadataWorker();
    MetadataWorker(const MetadataWorker&) = delete;
    MetadataWorker& operator=(const MetadataWorker&) = delete;
    SubmitResult Submit(const MetadataRequest& request);
    std::vector<MetadataCompletion> DrainCompletions();
    WorkerStatistics Statistics() const;
    [[nodiscard]] std::vector<MetadataRequest> Stop();

private:
    struct CacheEntry {
        TargetToken token;
        WindowOwnerLifetime lifetime;
        ImmutableMetadata metadata;
        std::size_t bytes;
        std::chrono::steady_clock::time_point expires;
    };
    std::optional<CacheEntry> FindCached(const TargetToken& token);
    void Cache(const TargetToken& token, const WindowOwnerLifetime& lifetime, ImmutableMetadata metadata);
    void Run(std::stop_token stop);
    WorkerLimits limits_;
    Query query_;
    LifetimeQuery lifetime_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<MetadataRequest> queued_;
    std::deque<MetadataCompletion> completed_;
    std::list<CacheEntry> cache_;
    std::optional<MetadataRequest> inFlight_;
    WorkerStatistics statistics_;
    bool stopping_{};
    std::jthread thread_;
};

} // namespace window_sweaters::applications
