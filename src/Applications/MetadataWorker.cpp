#include "MetadataWorker.h"
#include "ProcessMetadata.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace window_sweaters::applications {
namespace {
constexpr std::size_t maxEntries = 4096;
void ValidateLimits(const WorkerLimits& limits) {
    if (limits.queuedRequests == 0 || limits.queuedRequests > maxEntries ||
        limits.completedRequests == 0 || limits.completedRequests > maxEntries ||
        limits.cacheEntries > maxEntries || limits.cacheBytes > 16 * 1024 * 1024 ||
        limits.cacheLifetime.count() < 0 || limits.cacheLifetime > std::chrono::minutes(5))
        throw std::invalid_argument("Invalid application metadata worker limits");
}
} // namespace

TargetToken::TargetToken(tracking::TargetId target, std::uint64_t configurationRevision)
    : target_(target), configurationRevision_(configurationRevision) {}
std::optional<TargetToken> TargetToken::FromTarget(tracking::TargetId target, std::uint64_t revision) {
    if (target.hwnd == 0 || target.generation == 0 ||
        !ProcessInstance::FromValues(target.pid, target.processStart)) return std::nullopt;
    return TargetToken(target, revision);
}
ProcessInstance TargetToken::Process() const {
    return *ProcessInstance::FromValues(target_.pid, target_.processStart);
}
MetadataRequest::MetadataRequest(TargetToken token, std::uint64_t sequence)
    : token_(std::move(token)), sequence_(sequence) {}

CompletionGate::CompletionGate(std::uint64_t configurationRevision, std::size_t maxTargets)
    : revision_(configurationRevision), maxTargets_(maxTargets) {
    if (maxTargets == 0 || maxTargets > maxEntries)
        throw std::invalid_argument("Invalid application completion gate capacity");
}
std::optional<MetadataRequest> CompletionGate::Begin(const TargetToken& token) {
    if (token.ConfigurationRevision() != revision_ || nextSequence_ == 0) return std::nullopt;
    const auto hwnd = token.Target().hwnd;
    if (!pending_.contains(hwnd) && pending_.size() >= maxTargets_) return std::nullopt;
    MetadataRequest request(token, nextSequence_++);
    pending_.insert_or_assign(hwnd, request);
    return request;
}
bool CompletionGate::Accept(const MetadataCompletion& completion, const std::optional<TargetToken>& currentTarget) {
    const auto found = pending_.find(completion.request.Token().Target().hwnd);
    if (!currentTarget || *currentTarget != completion.request.Token() || !completion.metadata ||
        found == pending_.end() || found->second != completion.request ||
        completion.request.Token().ConfigurationRevision() != revision_) return false;
    pending_.erase(found);
    return true;
}
bool CompletionGate::Retire(const MetadataRequest& request) {
    const auto found = pending_.find(request.Token().Target().hwnd);
    if (found == pending_.end() || found->second != request) return false;
    pending_.erase(found);
    return true;
}
void CompletionGate::Forget(tracking::WindowHandle hwnd) { pending_.erase(hwnd); }
void CompletionGate::SetConfigurationRevision(std::uint64_t revision) {
    if (revision_ == revision) return;
    revision_ = revision;
    pending_.clear();
}

MetadataWorker::MetadataWorker(WorkerLimits limits, Query query)
    : limits_(limits), query_(query ? std::move(query) : Query([](const ProcessInstance& process) {
        return QueryProcessMetadata(process);
    })) {
    ValidateLimits(limits_);
    thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
MetadataWorker::~MetadataWorker() { (void)Stop(); }

SubmitResult MetadataWorker::Submit(const MetadataRequest& request) {
    std::lock_guard lock(mutex_);
    if (stopping_) return SubmitResult::Stopped;
    const auto hwnd = request.Token().Target().hwnd;
    const auto sameWindow = [hwnd](const auto& item) { return item.Token().Target().hwnd == hwnd; };
    if (inFlight_ && sameWindow(*inFlight_) && inFlight_->Sequence() >= request.Sequence())
        return SubmitResult::Obsolete;
    const auto pending = std::find_if(queued_.begin(), queued_.end(), sameWindow);
    if (pending != queued_.end()) {
        if (pending->Sequence() >= request.Sequence()) return SubmitResult::Obsolete;
        *pending = request;
        ++statistics_.coalesced;
        wake_.notify_one();
        return SubmitResult::Coalesced;
    }
    if (queued_.size() >= limits_.queuedRequests) return SubmitResult::Full;
    queued_.push_back(request);
    wake_.notify_one();
    return SubmitResult::Accepted;
}

std::vector<MetadataCompletion> MetadataWorker::DrainCompletions() {
    std::lock_guard lock(mutex_);
    std::vector<MetadataCompletion> result;
    result.reserve(completed_.size());
    while (!completed_.empty()) {
        result.push_back(std::move(completed_.front()));
        completed_.pop_front();
    }
    wake_.notify_all();
    return result;
}
WorkerStatistics MetadataWorker::Statistics() const {
    std::lock_guard lock(mutex_);
    auto result = statistics_;
    result.queued = queued_.size();
    result.completed = completed_.size();
    result.cached = cache_.size();
    result.inFlight = inFlight_.has_value();
    result.completionBackpressure = !stopping_ && !queued_.empty() &&
        completed_.size() >= limits_.completedRequests;
    return result;
}
std::vector<MetadataRequest> MetadataWorker::Stop() {
    std::vector<MetadataRequest> cancelled;
    {
        std::lock_guard lock(mutex_);
        cancelled.reserve(queued_.size() + completed_.size() + (inFlight_ ? 1 : 0));
        for (const auto& request : queued_) cancelled.push_back(request);
        for (const auto& completion : completed_) cancelled.push_back(completion.request);
        if (inFlight_) cancelled.push_back(*inFlight_);
        stopping_ = true;
        queued_.clear();
        completed_.clear();
    }
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    cache_.clear();
    statistics_.cacheBytes = 0;
    return cancelled;
}

ImmutableMetadata MetadataWorker::FindCached(const ProcessInstance& process) {
    const auto now = std::chrono::steady_clock::now();
    for (auto entry = cache_.begin(); entry != cache_.end();) {
        if (entry->expires <= now) {
            statistics_.cacheBytes -= entry->bytes;
            entry = cache_.erase(entry);
        } else {
            ++entry;
        }
    }
    const auto found = std::find_if(cache_.begin(), cache_.end(),
        [&process](const CacheEntry& entry) { return entry.process == process; });
    if (found == cache_.end()) return {};
    auto metadata = found->metadata;
    cache_.splice(cache_.begin(), cache_, found);
    ++statistics_.cacheHits;
    return metadata;
}
void MetadataWorker::Cache(const ProcessInstance& process, ImmutableMetadata metadata) {
    const auto* value = std::get_if<AppMetadata>(metadata.get());
    if (!value || limits_.cacheEntries == 0 || limits_.cacheLifetime.count() == 0) return;
    const auto bytes = sizeof(CacheEntry) + 2 * sizeof(void*) + value->RetainedBytes() +
        sizeof(MetadataResult) + 4 * sizeof(void*);
    if (bytes > limits_.cacheBytes) return;
    while (!cache_.empty() && (cache_.size() >= limits_.cacheEntries ||
        statistics_.cacheBytes > limits_.cacheBytes - bytes)) {
        statistics_.cacheBytes -= cache_.back().bytes;
        cache_.pop_back();
    }
    cache_.push_front({process, std::move(metadata), bytes,
        std::chrono::steady_clock::now() + limits_.cacheLifetime});
    statistics_.cacheBytes += bytes;
}

void MetadataWorker::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::unique_lock lock(mutex_);
        if (!wake_.wait(lock, stop, [this] {
            return stopping_ || (!queued_.empty() && completed_.size() < limits_.completedRequests);
        }) || stopping_) return;
        MetadataRequest request = queued_.front();
        queued_.pop_front();
        inFlight_ = request;
        auto metadata = FindCached(request.Token().Process());
        if (!metadata) {
            ++statistics_.queries;
            lock.unlock();
            MetadataResult result = MetadataUnavailable{UnavailableReason::QueryException, QueryStage::Worker};
            try {
                result = query_(request.Token().Process());
            } catch (...) {
                // A failing injected backend cannot escape the worker or publish guessed identity.
            }
            metadata = std::make_shared<const MetadataResult>(std::move(result));
            lock.lock();
            if (std::holds_alternative<MetadataUnavailable>(*metadata)) ++statistics_.unavailableQueries;
            if (!stopping_) Cache(request.Token().Process(), metadata);
        }
        inFlight_.reset();
        if (stopping_ || stop.stop_requested()) return;
        completed_.push_back({std::move(request), std::move(metadata)});
    }
}

} // namespace window_sweaters::applications
