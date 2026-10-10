#include "Preparation.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace window_sweaters::icon_yarn {
namespace {

constexpr std::size_t MaximumIdentityBytes = 4096;
constexpr std::size_t MaximumSourceCharacters = 4096;

bool Valid(const WorkToken& token, const IconInput& input) noexcept {
    return token.targetId && token.processBirth && token.targetLifetime && token.deviceGeneration &&
           !input.canonicalAppIdentity.empty() &&
           input.canonicalAppIdentity.size() <= MaximumIdentityBytes &&
           input.canonicalAppIdentity.find('\0') == std::string::npos &&
           input.executablePath.size() <= MaximumSourceCharacters &&
           input.executablePath.find(L'\0') == std::wstring::npos;
}

std::size_t InputBytes(const IconInput& input) noexcept {
    return sizeof(IconInput) + input.canonicalAppIdentity.capacity() + 1 +
           (input.executablePath.capacity() + 1) * sizeof(wchar_t);
}

std::size_t RequestBytes(const WorkRequest& request) noexcept {
    // Conservative charge includes vector growth/node overhead and shared ownership.
    return InputBytes(*request.Input()) + 2 * sizeof(Completion) + 128;
}

} // namespace

WorkRequest::WorkRequest(WorkToken token, std::uint64_t sequence,
                         std::shared_ptr<const IconInput> input)
    : token_(token), sequence_(sequence), input_(std::move(input)) {}

CompletionGate::CompletionGate(std::size_t maxTargets) : maxTargets_(maxTargets) {
    if (!maxTargets || maxTargets > 4096) throw std::invalid_argument("invalid target bound");
}

std::optional<WorkRequest> CompletionGate::Begin(WorkToken token, IconInput input) {
    if (!Valid(token, input) || nextSequence_ == std::numeric_limits<std::uint64_t>::max() ||
        (!pending_.contains(token.targetId) && pending_.size() >= maxTargets_))
        return std::nullopt;
    // Do not retain arbitrarily over-reserved caller strings.
    IconInput bounded{std::string(input.canonicalAppIdentity.data(), input.canonicalAppIdentity.size()),
                      input.iconRevision,
                      std::wstring(input.executablePath.data(), input.executablePath.size())};
    WorkRequest request(token, nextSequence_++, std::make_shared<const IconInput>(std::move(bounded)));
    pending_.insert_or_assign(token.targetId, request);
    return request;
}

bool CompletionGate::Accept(const Completion& completion, const std::optional<WorkToken>& current) {
    if (!completion.palette || !current || *current != completion.request.Token()) return false;
    const auto found = pending_.find(current->targetId);
    if (found == pending_.end() || found->second.Sequence() != completion.request.Sequence() ||
        found->second.Token() != *current ||
        *found->second.Input() != *completion.request.Input())
        return false;
    pending_.erase(found);
    return true;
}

void CompletionGate::Forget(std::uint64_t targetId) { pending_.erase(targetId); }
void CompletionGate::Clear() { pending_.clear(); }

PreparationWorker::PreparationWorker(Query query, PreparationLimits limits)
    : limits_(limits), query_(std::move(query)) {
    if (!query_ || !limits.workerCount || limits.workerCount > 4 ||
        !limits.pendingJobs || limits.pendingJobs > 1024 ||
        !limits.subscribers || limits.subscribers > 4096 ||
        !limits.pendingBytes || limits.pendingBytes > 64 * 1024 * 1024 ||
        limits.cacheEntries > 4096 || limits.cacheBytes > 64 * 1024 * 1024 ||
        limits.jobDeadline <= std::chrono::milliseconds::zero() ||
        limits.jobDeadline > std::chrono::seconds(30))
        throw std::invalid_argument("invalid preparation bounds");
    threads_.reserve(limits.workerCount);
    for (std::size_t i = 0; i < limits.workerCount; ++i)
        threads_.emplace_back([this](std::stop_token stop) { Run(stop); });
}

PreparationWorker::~PreparationWorker() { Stop(); }

SubmitStatus PreparationWorker::Submit(const WorkRequest& request) {
    std::lock_guard lock(mutex_);
    if (stopping_) return SubmitStatus::Stopped;
    if (!Valid(request.Token(), *request.Input())) return SubmitStatus::Invalid;
    for (const auto& job : jobs_) {
        for (const auto& waiting : job->subscribers) {
            if (waiting.Token().targetId != request.Token().targetId) continue;
            if (waiting.Sequence() > request.Sequence()) return SubmitStatus::Invalid;
            if (waiting.Sequence() == request.Sequence()) return SubmitStatus::Deduplicated;
        }
    }
    for (const auto& completion : completed_) {
        if (completion.request.Token().targetId != request.Token().targetId) continue;
        if (completion.request.Sequence() > request.Sequence()) return SubmitStatus::Invalid;
        if (completion.request.Sequence() == request.Sequence()) return SubmitStatus::Deduplicated;
    }
    ForgetLocked(request.Token().targetId);
    const std::size_t bytes = RequestBytes(request);
    if (statistics_.subscribers >= limits_.subscribers ||
        bytes > limits_.pendingBytes - statistics_.pendingBytes)
        return SubmitStatus::Full;

    for (auto entry = cache_.begin(); entry != cache_.end(); ++entry) {
        if (*entry->input != *request.Input()) continue;
        completed_.push_back({request, entry->palette});
        cache_.splice(cache_.begin(), cache_, entry);
        ++statistics_.cacheHits;
        ++statistics_.subscribers;
        statistics_.pendingBytes += bytes;
        return SubmitStatus::Cached;
    }
    for (auto& job : jobs_) {
        if (*job->input != *request.Input()) continue;
        job->subscribers.push_back(request);
        ++statistics_.deduplicated;
        ++statistics_.subscribers;
        statistics_.pendingBytes += bytes;
        return SubmitStatus::Deduplicated;
    }
    const std::size_t jobBytes = InputBytes(*request.Input()) + sizeof(Job) + 128;
    if (jobs_.size() >= limits_.pendingJobs ||
        jobBytes > limits_.pendingBytes - statistics_.pendingBytes - bytes)
        return SubmitStatus::Full;
    auto job = std::make_shared<Job>(Job{request.Input(), {request}, jobBytes, false});
    jobs_.push_back(std::move(job));
    ++statistics_.subscribers;
    statistics_.pendingBytes += bytes + jobBytes;
    wake_.notify_one();
    return SubmitStatus::Accepted;
}

void PreparationWorker::ForgetLocked(std::uint64_t targetId) {
    for (auto job = jobs_.begin(); job != jobs_.end();) {
        auto& subscribers = (*job)->subscribers;
        for (auto waiting = subscribers.begin(); waiting != subscribers.end();) {
            if (waiting->Token().targetId != targetId) { ++waiting; continue; }
            statistics_.pendingBytes -= RequestBytes(*waiting);
            --statistics_.subscribers;
            waiting = subscribers.erase(waiting);
        }
        if (subscribers.empty() && !(*job)->running) {
            statistics_.pendingBytes -= (*job)->bytes;
            job = jobs_.erase(job);
        }
        else ++job;
    }
    for (auto completion = completed_.begin(); completion != completed_.end();) {
        if (completion->request.Token().targetId != targetId) { ++completion; continue; }
        statistics_.pendingBytes -= RequestBytes(completion->request);
        --statistics_.subscribers;
        completion = completed_.erase(completion);
    }
}

void PreparationWorker::Forget(std::uint64_t targetId) {
    std::lock_guard lock(mutex_);
    ForgetLocked(targetId);
}

std::vector<Completion> PreparationWorker::Drain() {
    std::lock_guard lock(mutex_);
    std::vector<Completion> results;
    results.reserve(completed_.size());
    while (!completed_.empty()) {
        statistics_.pendingBytes -= RequestBytes(completed_.front().request);
        --statistics_.subscribers;
        results.push_back(std::move(completed_.front()));
        completed_.pop_front();
    }
    return results;
}

PreparationStatistics PreparationWorker::Statistics() const {
    std::lock_guard lock(mutex_);
    auto result = statistics_;
    result.pendingJobs = jobs_.size();
    result.running = static_cast<std::size_t>(std::count_if(
        jobs_.begin(), jobs_.end(), [](const auto& job) { return job->running; }));
    result.queued = result.pendingJobs - result.running;
    result.completed = completed_.size();
    result.cacheEntries = cache_.size();
    return result;
}

void PreparationWorker::Cache(std::shared_ptr<const IconInput> input, ImmutablePalette palette) {
    const std::size_t bytes = InputBytes(*input) + sizeof(PaletteResult) + sizeof(CacheEntry) + 128;
    if (!limits_.cacheEntries || bytes > limits_.cacheBytes) return;
    while (!cache_.empty() && (cache_.size() >= limits_.cacheEntries ||
                              bytes > limits_.cacheBytes - statistics_.cacheBytes)) {
        statistics_.cacheBytes -= cache_.back().bytes;
        cache_.pop_back();
    }
    cache_.push_front({std::move(input), std::move(palette), bytes});
    statistics_.cacheBytes += bytes;
}

void PreparationWorker::Run(std::stop_token stop) {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] {
                return stopping_ || std::any_of(jobs_.begin(), jobs_.end(),
                                                [](const auto& item) { return !item->running; });
            });
            if (stopping_ || stop.stop_requested()) return;
            const auto next = std::find_if(jobs_.begin(), jobs_.end(),
                                          [](const auto& item) { return !item->running; });
            job = *next;
            job->running = true;
            ++statistics_.queries;
        }
        const auto deadline = std::chrono::steady_clock::now() + limits_.jobDeadline;
        PaletteResult result = Fallback{FallbackReason::QueryFailed};
        try {
            result = query_(*job->input, stop, deadline);
        } catch (const std::bad_alloc&) {
            result = Fallback{FallbackReason::ResourceLimit};
        } catch (...) {
            result = Fallback{FallbackReason::QueryFailed};
        }
        if (stop.stop_requested()) result = Fallback{FallbackReason::Cancelled};
        else if (std::chrono::steady_clock::now() >= deadline)
            result = Fallback{FallbackReason::DeadlineExceeded};
        auto palette = std::make_shared<const PaletteResult>(std::move(result));
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            Cache(job->input, palette);
            for (const auto& request : job->subscribers) completed_.push_back({request, palette});
            statistics_.pendingBytes -= job->bytes;
            jobs_.remove(job);
        }
    }
}

void PreparationWorker::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    for (auto& thread : threads_) thread.request_stop();
    wake_.notify_all();
    threads_.clear();
    std::lock_guard lock(mutex_);
    jobs_.clear();
    completed_.clear();
    cache_.clear();
    statistics_.pendingBytes = statistics_.subscribers = statistics_.cacheBytes = 0;
}

} // namespace window_sweaters::icon_yarn
