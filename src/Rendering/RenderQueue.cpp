// SPDX-License-Identifier: GPL-3.0-only
#include "RenderQueue.h"
#include "..\Geometry\Geometry.h"
#include "..\Knitting\WicSurface.h"
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace window_sweaters::rendering {
namespace {
constexpr std::size_t MiB = 1024u * 1024u;
constexpr HRESULT Quota = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
std::atomic<UINT_PTR> nextCookie{1};

HRESULT ReceiverError(HWND receiver, DWORD owner) noexcept {
    DWORD pid{};
    const auto thread = GetWindowThreadProcessId(receiver, &pid);
    if (!thread) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (pid != GetCurrentProcessId() || thread != owner) return E_ACCESSDENIED;
    return S_OK;
}

bool ValidLimits(const RenderQueueLimits& l) noexcept {
    return l.maxPending && l.maxPending <= 512
        && l.maxReady && l.maxReady <= 512
        && l.maxTargetKeys && l.maxTargetKeys <= 65536
        && l.maxPendingBytes >= sizeof(RenderRequest) && l.maxPendingBytes <= 16 * MiB
        && l.maxReadyBytes >= sizeof(RenderReady) + sizeof(RenderRequest)
        && l.maxReadyBytes <= 256 * MiB
        && l.maxPerimeterBytes && l.maxPerimeterBytes <= 64 * MiB
        && l.maxTransientBytes && l.maxTransientBytes <= 512 * MiB
        && l.cacheBytes <= 8 * MiB;
}

bool ValidToken(const tracking::WorkToken& token) noexcept {
    return token.target.hwnd && token.target.pid && token.target.processStart
        && token.target.generation && token.appearanceRevision;
}

struct Budget {
    std::size_t requestBytes{}, readyBytes{}, transientBytes{};
    std::optional<RenderError> error;
};

struct TileBudget {
    std::size_t retained{}, pixels{}, cells{};
};

// The core's public layout function covers uncharted tiles. Chart repeats use a
// conservative rounding bound, not a second renderer/cache implementation.
TileBudget BoundTile(knitting::TileSpec spec) {
    spec = knitting::NormalizeTileSpec(std::move(spec));
    auto material = spec.material == knitting::Material::Raised
        ? ws_knit_raised_material(spec.gauge) : spec.gauge;
    std::uint64_t width{}, height{}, cells{};
    if (!spec.chart) {
        ws_knit_layout layout{};
        if (!ws_knit_make_layout(&material, spec.bandDip, spec.rasterScale, &layout))
            throw RenderError{RenderErrorKind::ResourceQuota, Quota};
        width = static_cast<std::uint64_t>(layout.width_dip) * layout.raster_scale;
        height = static_cast<std::uint64_t>(layout.height_dip) * layout.raster_scale;
    } else {
        const bool raised = spec.material == knitting::Material::Raised;
        const float density = raised ? 1.f : 2.f;
        const float step = spec.bandDip / std::max(1.5f, material.rows) / density;
        const float sw = step * material.aspect;
        const float columns = static_cast<float>(spec.chart->width) * density;
        const float rows = static_cast<float>(spec.chart->height) * density;
        const float nc = columns * std::max(1.f, std::ceil(8.f / (sw * columns)));
        const float nr = rows * std::max(1.f, std::ceil(8.f / (step * rows)));
        if (!std::isfinite(nc) || !std::isfinite(nr) || nc > 4096.f || nr > 4096.f)
            throw RenderError{RenderErrorKind::ResourceQuota, Quota};
        // +1 covers lround and float rounding; scale 4 is the largest automatic
        // choice, so reservations remain safe near the core's step threshold.
        const double wd = std::max(8., std::ceil(static_cast<double>(sw * nc)) + 1.);
        const double hd = std::max(8., std::ceil(static_cast<double>(step * nr)) + 1.);
        const int scale = spec.rasterScale ? spec.rasterScale : 4;
        if (wd * scale > 4096. || hd * scale > 4096.)
            throw RenderError{RenderErrorKind::ResourceQuota, Quota};
        width = static_cast<std::uint64_t>(wd) * scale;
        height = static_cast<std::uint64_t>(hd) * scale;
        if (raised) {
            const auto count = (static_cast<std::uint64_t>(nc) + 4u)
                * (static_cast<std::uint64_t>(nr) + 4u);
            if (count > 256u * 1024u)
                throw RenderError{RenderErrorKind::ResourceQuota, Quota};
            // MakeTile reserves the validated complete count before appending.
            // This bounds peak scratch, with no overlapping growth allocations.
            // v145 x64 large vectors add up to 47 alignment/bookkeeping bytes:
            // 39 in Release, 47 in Debug (including its allocation sentinel).
            cells = count * sizeof(knitting::detail::YarnCell) + 47u;
        }
    }
    const auto pixels = static_cast<std::size_t>(width * height * 4u);
    // Albedo plus an equal-size stitch height image.
    return {sizeof(knitting::Tile) + 2 * pixels + (spec.chart ? spec.chart->ByteSize() : 0),
            pixels, static_cast<std::size_t>(cells)};
}

Budget BoundRequest(const RenderRequest& r, const RenderQueueLimits& limits) noexcept {
    Budget b;
    b.requestBytes = sizeof(RenderRequest) + (r.tile.chart ? r.tile.chart->ByteSize() : 0);
    const auto errorBytes = b.requestBytes + sizeof(RenderReady);
    b.readyBytes = errorBytes;
    try {
        auto f = r.frame;
        if (!std::isfinite(f.widthDip) || !std::isfinite(f.heightDip)
            || !std::isfinite(f.radiusDip) || !std::isfinite(f.bandDip)
            || !std::isfinite(f.tuckDip) || !std::isfinite(f.deviceScale)
            || !std::isfinite(f.dim) || f.widthDip <= 0.f || f.heightDip <= 0.f
            || f.bandDip > 256.f || f.deviceScale < knitting::MinimumDeviceScale
            || f.deviceScale > knitting::MaximumDeviceScale || f.dim < 0.f || f.dim > 1.f
            || ((f.contentWidthPixels == 0) != (f.contentHeightPixels == 0))
            || (f.anchor != KnitAnchor::Corner && f.anchor != KnitAnchor::Centre))
            throw std::invalid_argument("Invalid frame");
        if (f.contentWidthPixels) {
            f.widthDip = static_cast<float>(f.contentWidthPixels) / f.deviceScale;
            f.heightDip = static_cast<float>(f.contentHeightPixels) / f.deviceScale;
        }
        f.bandDip = std::max(2.f, f.bandDip);
        f.radiusDip = std::clamp(f.radiusDip, 0.f, std::min(f.widthDip, f.heightDip) * .5f);
        f.tuckDip = std::max(1.f, f.tuckDip);
        const float half = std::min(f.widthDip, f.heightDip) * .5f - 2.f;
        if (f.tuckDip > half) f.tuckDip = half > 1.f ? half : 1.f;
        const auto naturalInset = static_cast<std::uint32_t>(std::ceil(f.bandDip * f.deviceScale));
        const auto inset = f.physicalPaddingPixels ? f.physicalPaddingPixels : naturalInset;
        if (static_cast<std::uint64_t>(inset) + 1 < naturalInset
            || static_cast<std::uint64_t>(inset) > naturalInset + 1u)
            throw std::invalid_argument("Invalid physical padding");
        const double cw = f.contentWidthPixels ? f.contentWidthPixels
            : std::ceil(static_cast<double>(f.widthDip) * f.deviceScale);
        const double ch = f.contentHeightPixels ? f.contentHeightPixels
            : std::ceil(static_cast<double>(f.heightDip) * f.deviceScale);
        const auto margin = geometry::ShadowMarginPixels(inset);
        const double w = cw + 2. * (inset + margin), h = ch + 2. * (inset + margin);
        if (!std::isfinite(w) || !std::isfinite(h) || w > 32768. || h > 32768.)
            throw RenderError{RenderErrorKind::ResourceQuota, Quota};
        const float ow = f.widthDip + 2.f * f.bandDip;
        const float oh = f.heightDip + 2.f * f.bandDip;
        float depth = f.bandDip + f.tuckDip
            + std::max(0.f, f.radiusDip - f.tuckDip) * (1.f - .7071067811865475244f) + 1.f;
        depth = std::min(depth, std::min(ow, oh) * .5f);
        const double narrow = std::ceil(depth * f.deviceScale) + 2. + margin;
        const double output = 2. * narrow * (w + h) * 4.;
        if (!std::isfinite(output) || output > static_cast<double>(limits.maxPerimeterBytes))
            throw RenderError{RenderErrorKind::ResourceQuota, Quota};
        auto tileSpec = r.tile;
        tileSpec.bandDip = f.bandDip;
        tileSpec.deviceScale = f.deviceScale;
        const auto tile = BoundTile(tileSpec);
        auto retainedTiles = tile.retained, largestPixels = tile.pixels, largestCells = tile.cells;
        if (tileSpec.chart) {
            const auto& look = tileSpec.chart->appearance;
            auto solid = tileSpec;
            solid.chart.reset();
            solid.stitch = knitting::Stitch::Stockinette;
            solid.material = knitting::Material::Raised;
            if (look.flags & (designs::ChartSolidCorners | designs::ChartOuterCuff)) {
                const auto aux = BoundTile(solid);
                if (look.flags & designs::ChartSolidCorners) retainedTiles += aux.retained;
                if (look.flags & designs::ChartOuterCuff) retainedTiles += aux.retained;
                largestPixels = std::max(largestPixels, aux.pixels);
                largestCells = std::max(largestCells, aux.cells);
            }
        }
        const auto outputBytes = static_cast<std::size_t>(output);
        b.readyBytes = errorBytes + sizeof(KnitPerimeter) + outputBytes + tile.retained;
        // Four output vectors, one side's live colour and stitch-height WIC bitmaps
        // plus the height read-back, uploaded tile copies (albedo and height) and
        // the core's eight simultaneous tile-generation pixel buffers.
        b.transientBytes = b.requestBytes + sizeof(KnitPerimeter) + outputBytes
            + static_cast<std::size_t>(3. * narrow * std::max(w, h) * 4.)
            + 2 * retainedTiles + 8 * largestPixels + largestCells;
        if (b.readyBytes > limits.maxReadyBytes || b.transientBytes > limits.maxTransientBytes)
            throw RenderError{RenderErrorKind::ResourceQuota, Quota};
    } catch (const RenderError& error) {
        b.error = error;
    } catch (const std::invalid_argument&) {
        b.error = RenderError{RenderErrorKind::InvalidInput, E_INVALIDARG};
    } catch (...) {
        b.error = RenderError{RenderErrorKind::NativeFailure, E_FAIL};
    }
    if (b.error) { b.readyBytes = errorBytes; b.transientBytes = 0; }
    return b;
}

std::size_t ReadyBytes(const RenderReady& ready) noexcept {
    auto bytes = sizeof(RenderReady) + sizeof(RenderRequest)
        + (ready.request->tile.chart ? ready.request->tile.chart->ByteSize() : 0);
    if (const auto* perimeter = std::get_if<std::shared_ptr<const KnitPerimeter>>(&ready.outcome)) {
        bytes += sizeof(KnitPerimeter) + (*perimeter)->tile->ByteSize();
        for (const auto& piece : (*perimeter)->pieces) bytes += piece.image.ByteSize();
    }
    return bytes;
}
}

struct RenderQueue::Impl {
    struct Lifetime {
        tracking::TargetId target;
        std::uint64_t appearance{}, serial{};
        bool cancelled{};
    };
    struct Job {
        std::shared_ptr<const RenderRequest> request;
        Budget budget;
        std::uint64_t serial{};
    };

    Impl(HWND window, RenderQueueLimits bounds) : receiver(window), owner(GetCurrentThreadId()),
        limits(bounds), cookie(nextCookie.fetch_add(1, std::memory_order_relaxed)) {
        pending.reserve(limits.maxPending);
        ready.reserve(limits.maxReady);
        lifetimes.reserve(limits.maxTargetKeys);
    }
    HWND receiver;
    const DWORD owner;
    const RenderQueueLimits limits;
    const UINT_PTR cookie;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::jthread worker;
    std::vector<Job> pending;
    std::vector<RenderReady> ready;
    std::unordered_map<tracking::WindowHandle, Lifetime> lifetimes;
    RenderQueueStatus status;
    std::uint64_t config{}, device{}, nextSerial{};
    bool startupDone{};
    HRESULT startupResult{E_PENDING};
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
    RenderQueueTestHooks hooks;
#endif

    bool Current(const Job& job) const noexcept {
        const auto& r = *job.request;
        const auto it = lifetimes.find(r.token.target.hwnd);
        return status.state == RenderQueueState::Running && r.configRevision == config
            && r.deviceGeneration == device && it != lifetimes.end()
            && it->second.target == r.token.target && !it->second.cancelled
            && it->second.serial == job.serial;
    }

    void Prune(const tracking::TargetId& target) noexcept {
        std::erase_if(pending, [&](const Job& job) {
            if (job.request->token.target != target) return false;
            status.pendingBytes -= job.budget.requestBytes;
            ++status.staleCancelled;
            return true;
        });
        std::erase_if(ready, [&](const RenderReady& result) {
            if (result.request->token.target != target) return false;
            status.readyBytes -= ReadyBytes(result);
            ++status.staleCancelled;
            return true;
        });
        status.pending = pending.size();
        status.ready = ready.size();
    }

    void ClearWork() noexcept {
        status.staleCancelled += pending.size() + ready.size();
        pending.clear();
        ready.clear();
        status.pending = status.pendingBytes = status.ready = status.readyBytes = 0;
    }

    void Notify() noexcept {
        if (status.notificationPending) return;
        auto hr = ReceiverError(receiver, owner);
        if (SUCCEEDED(hr)) {
            SetLastError(ERROR_SUCCESS);
            if (!PostMessageW(receiver, RenderQueue::ReadyMessage, cookie, 0)) {
                const auto error = GetLastError();
                hr = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
            }
        }
        if (FAILED(hr)) {
            status.error = hr;
            status.state = RenderQueueState::Faulted;
            ++status.notificationFailures;
            ClearWork();
            condition.notify_all();
        } else {
            status.notificationPending = true;
            ++status.notifications;
        }
    }

    void Run(std::stop_token stop) noexcept {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            std::lock_guard lock(mutex);
            startupResult = com;
            startupDone = true;
        }
        condition.notify_all();
        if (FAILED(com)) return;
        {
            knitting::TileCache cache(limits.cacheBytes);
            try {
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
                if (hooks.beforeFirstDequeue) hooks.beforeFirstDequeue(stop);
#endif
                for (;;) {
                    Job job;
                    {
                        std::unique_lock lock(mutex);
                        condition.wait(lock, [&] {
                            return status.state != RenderQueueState::Running || stop.stop_requested()
                                || (!pending.empty() && ready.size() < limits.maxReady
                                    && pending.front().budget.readyBytes <= limits.maxReadyBytes - status.readyBytes);
                        });
                        if (status.state != RenderQueueState::Running || stop.stop_requested()) break;
                        job = std::move(pending.front());
                        pending.erase(pending.begin());
                        status.pending = pending.size();
                        status.pendingBytes -= job.budget.requestBytes;
                        status.active = 1;
                        status.transientBytes = job.budget.transientBytes;
                        status.maxTransientBytes = std::max(status.maxTransientBytes, status.transientBytes);
                    }
                    RenderReady result{job.request, RenderError{RenderErrorKind::NativeFailure, E_FAIL}};
                    if (job.budget.error) result.outcome = *job.budget.error;
                    else {
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
                        if (hooks.beforeRender) hooks.beforeRender(*job.request, stop);
#endif
                        {
                            std::lock_guard lock(mutex);
                            if (!Current(job)) {
                                ++status.staleCancelled;
                                status.active = status.transientBytes = 0;
                                continue;
                            }
                            ++status.renderPreparations;
                        }
                        try {
                            auto perimeter = std::make_shared<const KnitPerimeter>(
                                KnitRaster::RenderPerimeter(job.request->frame, job.request->tile, cache));
                            result.outcome = std::move(perimeter);
                        } catch (const std::invalid_argument&) {
                            result.outcome = RenderError{RenderErrorKind::InvalidInput, E_INVALIDARG};
                        } catch (const std::bad_alloc&) {
                            result.outcome = RenderError{RenderErrorKind::ResourceQuota, E_OUTOFMEMORY};
                        } catch (const std::system_error& error) {
                            result.outcome = RenderError{RenderErrorKind::NativeFailure,
                                static_cast<HRESULT>(error.code().value())};
                        } catch (...) {
                            result.outcome = RenderError{RenderErrorKind::NativeFailure, E_FAIL};
                        }
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
                        if (hooks.afterRender) hooks.afterRender(*job.request, stop);
#endif
                    }
                    {
                        std::lock_guard lock(mutex);
                        status.active = status.transientBytes = 0;
                        status.tileGenerations = cache.GenerationCount();
                        status.cacheHits = cache.HitCount();
                        status.residentCacheBytes = cache.ResidentBytes();
                        if (!Current(job)) { ++status.staleCancelled; continue; }
                        // The reservation precedes all rendering. Never publish a
                        // success whose actual retained charge exceeds that bound.
                        auto bytes = ReadyBytes(result);
                        if (bytes > job.budget.readyBytes || bytes > limits.maxReadyBytes - status.readyBytes) {
                            result.outcome = RenderError{RenderErrorKind::ResourceQuota, Quota};
                            bytes = ReadyBytes(result);
                        }
                        if (const auto* error = std::get_if<RenderError>(&result.outcome);
                            error && error->kind == RenderErrorKind::ResourceQuota)
                            ++status.quotaRejected;
                        ready.push_back(std::move(result));
                        status.ready = ready.size();
                        status.readyBytes += bytes;
                        status.maxReady = std::max(status.maxReady, status.ready);
                        status.maxReadyBytes = std::max(status.maxReadyBytes, status.readyBytes);
                        Notify();
                    }
                }
            } catch (...) {
                std::lock_guard lock(mutex);
                status.error = E_FAIL;
                status.state = RenderQueueState::Faulted;
                ClearWork();
                Notify();
            }
        }
        {
            std::lock_guard lock(mutex);
            status.active = status.transientBytes = status.residentCacheBytes = 0;
        }
        CoUninitialize();
    }

    void Shutdown() noexcept {
        {
            std::lock_guard lock(mutex);
            status.state = RenderQueueState::Stopped;
            ClearWork();
        }
        worker.request_stop();
        condition.notify_all();
        if (worker.joinable()) worker.join();
        HWND closingReceiver{};
        {
            std::lock_guard lock(mutex);
            closingReceiver = receiver;
            receiver = nullptr;
            lifetimes.clear();
            status.notificationPending = false;
            status.active = status.transientBytes = status.residentCacheBytes = 0;
        }
        // PeekMessage can dispatch sent messages. Do not hold the queue mutex
        // across that owner-thread reentrancy boundary.
        if (GetCurrentThreadId() == owner && closingReceiver) {
            MSG message{};
            while (PeekMessageW(&message, closingReceiver, RenderQueue::ReadyMessage,
                                RenderQueue::ReadyMessage, PM_REMOVE)) {}
        }
    }
};

RenderQueue::RenderQueue(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RenderQueue::~RenderQueue() { impl_->Shutdown(); }

HRESULT RenderQueue::Create(HWND receiver, const RenderQueueLimits& limits,
                            std::unique_ptr<RenderQueue>& queue
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
                            , RenderQueueTestHooks hooks
#endif
                            ) noexcept {
    if (queue || !receiver || !ValidLimits(limits)) return E_INVALIDARG;
    const auto hr = ReceiverError(receiver, GetCurrentThreadId());
    if (FAILED(hr)) return hr;
    try {
        auto impl = std::make_unique<Impl>(receiver, limits);
        if (!impl->cookie) return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
#ifdef WINDOW_SWEATERS_RENDER_QUEUE_TESTING
        impl->hooks = std::move(hooks);
#endif
        queue.reset(new RenderQueue(std::move(impl)));
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT RenderQueue::Start(std::uint64_t configRevision, std::uint64_t deviceGeneration) noexcept {
    auto& p = *impl_;
    if (GetCurrentThreadId() != p.owner) return RPC_E_WRONG_THREAD;
    if (!deviceGeneration) return E_INVALIDARG;
    std::unique_lock lock(p.mutex);
    if (p.status.state != RenderQueueState::Created) return E_UNEXPECTED;
    const auto receiver = ReceiverError(p.receiver, p.owner);
    if (FAILED(receiver)) return receiver;
    p.config = configRevision;
    p.device = deviceGeneration;
    p.status.state = RenderQueueState::Running;
    try { p.worker = std::jthread([&p](std::stop_token stop) { p.Run(stop); }); }
    catch (const std::system_error& error) {
        p.status.state = RenderQueueState::Faulted;
        p.status.error = HRESULT_FROM_WIN32(static_cast<DWORD>(error.code().value()));
        return p.status.error;
    } catch (...) {
        p.status.state = RenderQueueState::Faulted;
        p.status.error = E_OUTOFMEMORY;
        return p.status.error;
    }
    p.condition.wait(lock, [&] { return p.startupDone; });
    if (FAILED(p.startupResult)) {
        p.status.state = RenderQueueState::Faulted;
        p.status.error = p.startupResult;
        lock.unlock();
        p.worker.join();
        return p.startupResult;
    }
    return S_OK;
}

HRESULT RenderQueue::Submit(const RenderRequest& request) noexcept {
    auto& p = *impl_;
    if (GetCurrentThreadId() != p.owner) return RPC_E_WRONG_THREAD;
    std::lock_guard lock(p.mutex);
    ++p.status.renderRequests;
    if (p.status.state == RenderQueueState::Faulted) return p.status.error;
    if (p.status.state != RenderQueueState::Running) return E_UNEXPECTED;
    if (!ValidToken(request.token)) return E_INVALIDARG;
    auto it = p.lifetimes.find(request.token.target.hwnd);
    if (request.configRevision != p.config || request.deviceGeneration != p.device
        || (it != p.lifetimes.end()
            && ((it->second.target == request.token.target
                    && (it->second.cancelled || request.token.appearanceRevision < it->second.appearance))
                || (it->second.target != request.token.target
                    && request.token.target.generation <= it->second.target.generation)))) {
        ++p.status.staleCancelled;
        return S_FALSE;
    }
    const auto budget = BoundRequest(request, p.limits);
    std::size_t replacedBytes{}, replacedCount{}, coalescedCount{};
    for (const auto& job : p.pending) {
        if (job.request->token.target.hwnd == request.token.target.hwnd) {
            replacedBytes += job.budget.requestBytes;
            ++replacedCount;
            if (job.request->token.target == request.token.target) ++coalescedCount;
        }
    }
    if ((it == p.lifetimes.end() && p.lifetimes.size() == p.limits.maxTargetKeys)
        || p.pending.size() - replacedCount == p.limits.maxPending
        || budget.requestBytes > p.limits.maxPendingBytes - (p.status.pendingBytes - replacedBytes)
        || budget.readyBytes > p.limits.maxReadyBytes) {
        ++p.status.quotaRejected;
        return Quota;
    }
    if (p.nextSerial == std::numeric_limits<std::uint64_t>::max())
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    try {
        auto snapshot = std::make_shared<const RenderRequest>(request);
        if (it == p.lifetimes.end())
            it = p.lifetimes.emplace(request.token.target.hwnd, Impl::Lifetime{}).first;
        else p.Prune(it->second.target);
        p.status.coalesced += coalescedCount;
        it->second = {request.token.target, request.token.appearanceRevision, ++p.nextSerial, false};
        p.pending.push_back({std::move(snapshot), budget, p.nextSerial});
        ++p.status.acceptedRequests;
        p.status.pending = p.pending.size();
        p.status.pendingBytes += budget.requestBytes;
        p.status.maxPending = std::max(p.status.maxPending, p.status.pending);
        p.status.maxPendingBytes = std::max(p.status.maxPendingBytes, p.status.pendingBytes);
        p.condition.notify_one();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

HRESULT RenderQueue::Cancel(const tracking::TargetId& target) noexcept {
    auto& p = *impl_;
    if (GetCurrentThreadId() != p.owner) return RPC_E_WRONG_THREAD;
    std::lock_guard lock(p.mutex);
    if (p.status.state == RenderQueueState::Faulted) return p.status.error;
    if (p.status.state != RenderQueueState::Running) return E_UNEXPECTED;
    if (!ValidToken({target, 1})) return E_INVALIDARG;
    auto it = p.lifetimes.find(target.hwnd);
    if (it != p.lifetimes.end()) {
        if (it->second.target != target && target.generation <= it->second.target.generation)
            return S_FALSE;
        p.Prune(it->second.target);
        it->second = {target, 0, 0, true};
    } else {
        if (p.lifetimes.size() == p.limits.maxTargetKeys) return Quota;
        try { p.lifetimes.emplace(target.hwnd, Impl::Lifetime{target, 0, 0, true}); }
        catch (...) {
            p.status.state = RenderQueueState::Faulted;
            p.status.error = E_OUTOFMEMORY;
                p.ClearWork();
                p.Notify();
            p.condition.notify_one();
            return E_OUTOFMEMORY;
            }
    }
    p.condition.notify_one();
    return S_OK;
}

HRESULT RenderQueue::Invalidate(std::uint64_t configRevision, std::uint64_t deviceGeneration) noexcept {
    auto& p = *impl_;
    if (GetCurrentThreadId() != p.owner) return RPC_E_WRONG_THREAD;
    std::lock_guard lock(p.mutex);
    if (p.status.state == RenderQueueState::Faulted) return p.status.error;
    if (p.status.state != RenderQueueState::Running) return E_UNEXPECTED;
    if (configRevision < p.config || deviceGeneration < p.device) return E_INVALIDARG;
    if (configRevision == p.config && deviceGeneration == p.device) return S_FALSE;
    p.config = configRevision;
    p.device = deviceGeneration;
    p.ClearWork();
    for (auto& [handle, lifetime] : p.lifetimes) {
        (void)handle;
        lifetime.serial = 0;
    }
    p.condition.notify_one();
    return S_OK;
}

bool RenderQueue::OwnsNotification(WPARAM cookie, LPARAM reserved) const noexcept {
    const auto& p = *impl_;
    if (GetCurrentThreadId() != p.owner || reserved != 0 || cookie != p.cookie) return false;
    std::lock_guard lock(p.mutex);
    return p.receiver && p.status.state != RenderQueueState::Stopped;
}

HRESULT RenderQueue::Drain(WPARAM cookie, LPARAM reserved, std::vector<RenderReady>& results) noexcept {
    auto& p = *impl_;
    if (GetCurrentThreadId() != p.owner) return RPC_E_WRONG_THREAD;
    std::lock_guard lock(p.mutex);
    if (!p.receiver || cookie != p.cookie || reserved != 0) return E_INVALIDARG;
    if (p.status.state == RenderQueueState::Stopped) return E_UNEXPECTED;
    try { results.reserve(results.size() + p.ready.size()); }
    catch (...) {
        p.status.state = RenderQueueState::Faulted;
        p.status.error = E_OUTOFMEMORY;
        p.status.notificationPending = false;
        p.ClearWork();
        p.condition.notify_one();
        return E_OUTOFMEMORY;
    }
    for (auto& result : p.ready) results.push_back(std::move(result));
    p.ready.clear();
    p.status.ready = p.status.readyBytes = 0;
    p.status.notificationPending = false;
    p.condition.notify_one();
    return p.status.error;
}

HRESULT RenderQueue::Stop() noexcept {
    if (GetCurrentThreadId() != impl_->owner) return RPC_E_WRONG_THREAD;
    impl_->Shutdown();
    return S_OK;
}

RenderQueueStatus RenderQueue::Status() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->status;
}
} // namespace window_sweaters::rendering
