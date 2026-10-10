#pragma once

#include "IconPalette.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace window_sweaters::icon_yarn {

struct WorkToken {
    std::uint64_t targetId{};
    std::uint64_t processBirth{};
    std::uint64_t targetLifetime{};
    std::uint64_t appearanceRevision{};
    std::uint64_t deviceGeneration{};
    bool operator==(const WorkToken&) const = default;
};

// Paths are transient source facts from verified process metadata, not persistent identities.
// Empty source means unavailable. Native decoding accepts only a bounded absolute local path.
struct IconInput {
    std::string canonicalAppIdentity;
    std::uint64_t iconRevision{};
    std::wstring executablePath;
    bool operator==(const IconInput&) const = default;
};

class WorkRequest final {
public:
    const WorkToken& Token() const noexcept { return token_; }
    std::uint64_t Sequence() const noexcept { return sequence_; }
    const std::shared_ptr<const IconInput>& Input() const noexcept { return input_; }
private:
    friend class CompletionGate;
    WorkRequest(WorkToken token, std::uint64_t sequence, std::shared_ptr<const IconInput> input);
    WorkToken token_;
    std::uint64_t sequence_;
    std::shared_ptr<const IconInput> input_;
};

using ImmutablePalette = std::shared_ptr<const PaletteResult>;
struct Completion {
    WorkRequest request;
    ImmutablePalette palette;
};

// Owner-thread authority. Begin replaces that target's previous request. Take accepted
// completions only with the owner's current complete token; forgetting never touches a border.
class CompletionGate final {
public:
    explicit CompletionGate(std::size_t maxTargets = 512);
    std::optional<WorkRequest> Begin(WorkToken token, IconInput input);
    bool Accept(const Completion& completion, const std::optional<WorkToken>& current);
    void Forget(std::uint64_t targetId);
    void Clear();
    std::size_t Size() const noexcept { return pending_.size(); }
private:
    std::size_t maxTargets_;
    std::uint64_t nextSequence_{1};
    std::map<std::uint64_t, WorkRequest> pending_;
};

struct PreparationLimits {
    std::size_t workerCount{2};
    std::size_t pendingJobs{32};
    std::size_t subscribers{128};
    std::size_t pendingBytes{256 * 1024};
    std::size_t cacheEntries{64};
    std::size_t cacheBytes{256 * 1024};
    std::chrono::milliseconds jobDeadline{500};
};
enum class SubmitStatus { Accepted, Deduplicated, Cached, Full, Invalid, Stopped };
struct PreparationStatistics {
    std::size_t pendingJobs{}, queued{}, running{}, subscribers{}, completed{};
    std::size_t pendingBytes{}, cacheEntries{}, cacheBytes{};
    std::uint64_t queries{}, cacheHits{}, deduplicated{};
};

// Query is a local decoder boundary. It must honor stop/deadline and bound its own allocations;
// the worker bounds admitted jobs/subscribers/completions and cache, not arbitrary custom code.
// No UI callback is run. Submit/Drain/Statistics are thread-safe. Stop belongs to one owner,
// cancels queued jobs and joins workers. It must not be called from Query.
class PreparationWorker final {
public:
    using Query = std::function<PaletteResult(
        const IconInput&, std::stop_token, std::chrono::steady_clock::time_point)>;
    explicit PreparationWorker(Query query, PreparationLimits limits = {});
    ~PreparationWorker();
    PreparationWorker(const PreparationWorker&) = delete;
    PreparationWorker& operator=(const PreparationWorker&) = delete;
    SubmitStatus Submit(const WorkRequest& request);
    std::vector<Completion> Drain();
    void Forget(std::uint64_t targetId);
    PreparationStatistics Statistics() const;
    void Stop();
private:
    struct Job {
        std::shared_ptr<const IconInput> input;
        std::list<WorkRequest> subscribers;
        std::size_t bytes;
        bool running{};
    };
    struct CacheEntry {
        std::shared_ptr<const IconInput> input;
        ImmutablePalette palette;
        std::size_t bytes;
    };
    void Run(std::stop_token stop);
    void Cache(std::shared_ptr<const IconInput> input, ImmutablePalette palette);
    void ForgetLocked(std::uint64_t targetId);
    PreparationLimits limits_;
    Query query_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::list<std::shared_ptr<Job>> jobs_;
    std::deque<Completion> completed_;
    std::list<CacheEntry> cache_;
    PreparationStatistics statistics_;
    bool stopping_{};
    std::vector<std::jthread> threads_;
};

} // namespace window_sweaters::icon_yarn
