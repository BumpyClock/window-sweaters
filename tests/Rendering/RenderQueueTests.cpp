// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Rendering\RenderQueue.h"
#include "..\..\src\Knitting\WicSurface.h"
#include <objbase.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

using namespace window_sweaters;
using namespace window_sweaters::rendering;
using namespace std::chrono_literals;

namespace allocation_probe {
thread_local bool observing{};
std::atomic<std::size_t> live{}, peak{}, allocations{}, largest{};

struct alignas(std::max_align_t) Header {
    std::size_t observedBytes{};
};

void Max(std::atomic<std::size_t>& counter, std::size_t value) noexcept {
    auto old = counter.load();
    while (old < value && !counter.compare_exchange_weak(old, value)) {}
}

void Start() noexcept {
    live = peak = allocations = largest = 0;
    observing = true;
}
void Finish() noexcept { observing = false; }
}

void* operator new(std::size_t bytes) {
    using allocation_probe::Header;
    if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(Header)) throw std::bad_alloc();
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + std::max(bytes, std::size_t{1})));
    if (!header) throw std::bad_alloc();
    header->observedBytes = allocation_probe::observing && bytes >= 1024u * 1024u ? bytes : 0;
    if (header->observedBytes) {
        ++allocation_probe::allocations;
        allocation_probe::Max(allocation_probe::largest, bytes);
        allocation_probe::Max(allocation_probe::peak, allocation_probe::live.fetch_add(bytes) + bytes);
    }
    return header + 1;
}

void operator delete(void* pointer) noexcept {
    if (!pointer) return;
    auto* header = static_cast<allocation_probe::Header*>(pointer) - 1;
    if (header->observedBytes) allocation_probe::live.fetch_sub(header->observedBytes);
    std::free(header);
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

namespace {
constexpr HRESULT Quota = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
std::size_t assertions{};

void Check(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

class Gate {
public:
    void Wait(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, stop, [&] { return open_; });
    }
    void Entered() {
        std::unique_lock lock(mutex_);
        Check(condition_.wait_for(lock, 5s, [&] { return entered_; }), "Worker gate deadline");
    }
    void Open() {
        std::lock_guard lock(mutex_);
        open_ = true;
        condition_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool entered_{}, open_{};
};

RenderQueueTestHooks Pinned(const std::shared_ptr<Gate>& gate) {
    RenderQueueTestHooks hooks;
    hooks.beforeFirstDequeue = [gate](std::stop_token stop) { gate->Wait(stop); };
    return hooks;
}

RenderRequest Request(tracking::WindowHandle handle = 100, std::uint64_t appearance = 1) {
    RenderRequest r;
    r.token = {{handle, GetCurrentProcessId(), 123456, 1}, appearance};
    r.configRevision = r.deviceGeneration = 1;
    r.targetBounds = {20, 30, 116, 102};
    r.clipRelativeToOuter = {0, 0, 108, 84};
    r.frame.widthDip = 96.f;
    r.frame.heightDip = 72.f;
    r.frame.contentWidthPixels = 96;
    r.frame.contentHeightPixels = 72;
    r.frame.radiusDip = 0.f;
    r.frame.bandDip = 4.f;
    return r;
}

class Receiver {
public:
    Receiver() {
        static const ATOM atom = [] {
            WNDCLASSW wc{};
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpfnWndProc = WindowProc;
            wc.lpszClassName = L"WindowSweaters.RenderQueueTests.Receiver";
            return RegisterClassW(&wc);
        }();
        Check(atom != 0, "Register message-only receiver");
        window = CreateWindowExW(0, MAKEINTATOM(atom), L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
        Check(window != nullptr, "Create message-only receiver");
    }
    ~Receiver() {
        if (queue) queue->Stop();
        if (window) DestroyWindow(window);
    }
    void Start(RenderQueueLimits limits = {}, RenderQueueTestHooks hooks = {}) {
        Check(RenderQueue::Create(window, limits, queue, std::move(hooks)) == S_OK, "Create queue");
        Check(queue->Start(1, 1) == S_OK, "Start queue");
    }
    void Pump() {
        MSG message{};
        while (PeekMessageW(&message, window, RenderQueue::ReadyMessage,
                            RenderQueue::ReadyMessage, PM_REMOVE)) {
            DispatchMessageW(&message);
        }
    }
    template<class Predicate> void Wait(Predicate predicate, bool pump = false,
                                       std::chrono::seconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (pump) Pump();
            if (predicate()) break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Queue progress deadline");
            Sleep(1);
        }
    }
    void Collect(std::size_t count, std::chrono::seconds timeout = 5s) {
        Wait([&] { return results.size() >= count; }, true, timeout);
        Check(drainError == S_OK, "Owner drain succeeds");
    }
    HWND window{};
    std::unique_ptr<RenderQueue> queue;
    std::vector<RenderReady> results;
    std::size_t callbacks{};
    DWORD callbackThread{};
    WPARAM lastCookie{};
    HRESULT drainError{S_OK};
private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == RenderQueue::ReadyMessage) {
            if (self->queue && self->queue->OwnsNotification(wparam, lparam)) {
                ++self->callbacks;
                self->callbackThread = GetCurrentThreadId();
                self->lastCookie = wparam;
                self->drainError = self->queue->Drain(wparam, lparam, self->results);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

const KnitPerimeter& Perimeter(const RenderReady& ready) {
    const auto* p = std::get_if<std::shared_ptr<const KnitPerimeter>>(&ready.outcome);
    Check(p && *p, "Real immutable perimeter, not a success placeholder");
    return **p;
}

void ExpectError(const RenderReady& ready, RenderErrorKind kind, HRESULT result) {
    const auto* error = std::get_if<RenderError>(&ready.outcome);
    Check(error && error->kind == kind && error->result == result, "Typed render failure");
}

void RealCpuFixtureAndCacheReuse() {
    Receiver receiver;
    std::atomic<DWORD> workerThread{};
    std::atomic<bool> workerMta{};
    RenderQueueTestHooks hooks;
    hooks.beforeRender = [&](const RenderRequest&, std::stop_token) {
        workerThread.store(GetCurrentThreadId());
        APTTYPE apartment{};
        APTTYPEQUALIFIER qualifier{};
        workerMta.store(CoGetApartmentType(&apartment, &qualifier) == S_OK && apartment == APTTYPE_MTA);
    };
    receiver.Start({}, std::move(hooks));
    auto first = Request();
    Check(receiver.queue->Submit(first) == S_OK, "Submit fixture");
    first.frame.contentWidthPixels = 200;
    first.tile.baseArgb = 0xffee1122;
    receiver.Collect(1);
    const auto& p = Perimeter(receiver.results[0]);
    Check(p.width == 108 && p.height == 84 && p.contentInsetPixels == 6 && p.shadowMarginPixels == 2,
        "Physical fixture dimensions");
    Check(p.pieces[0].image.width == 108 && p.pieces[0].image.height == 8
        && p.pieces[1].x == 100 && p.pieces[1].image.width == 8
        && p.pieces[1].image.height == 84 && p.pieces[2].y == 76
        && p.pieces[3].image.width == 8, "Four narrow fixture pieces");
    Check(receiver.results[0].request->frame.contentWidthPixels == 96
        && receiver.results[0].request->tile.baseArgb == 0xff4e8098u, "Request owns immutable value snapshot");
    knitting::TileCache fixtureCache;
    const auto direct = KnitRaster::RenderPerimeter(Request().frame, Request().tile, fixtureCache);
    for (std::size_t i = 0; i < 4; ++i)
        Check(p.pieces[i].image.pixels == direct.pieces[i].image.pixels, "Queue wraps real CPU renderer fixture");
    const auto composed = KnitRaster::Compose(p);
    Check(composed.pixels[42u * 108u + 54u] == 0, "Fixture content remains transparent");
    Check((composed.pixels[4u * 108u + 54u] >> 24) == 255, "Fixture has opaque actual yarn");
    const auto firstWorker = workerThread.load();
    auto second = Request(101);
    second.frame.contentWidthPixels = 160;
    second.frame.radiusDip = 8.f;
    second.frame.dim = .5f;
    Check(receiver.queue->Submit(second) == S_OK, "Same appearance key across windows");
    receiver.Collect(2);
    Check(Perimeter(receiver.results[1]).tile == p.tile, "Worker cache reuses immutable tile");
    const auto status = receiver.queue->Status();
    Check(status.tileGenerations == 1 && status.cacheHits == 1, "Exact generation/cache-hit counts");
    Check(status.residentCacheBytes == p.tile->ByteSize() && status.residentCacheBytes <= 8u * 1024u * 1024u,
          "Cache resident bytes counted");
    Check(status.transientBytes == 0 && status.maxTransientBytes > p.tile->ByteSize(), "Transient reservation released");
    Check(receiver.callbackThread == GetCurrentThreadId() && workerThread.load() != GetCurrentThreadId(),
          "CPU preparation off owner, delivery only owner");
    Check(workerThread.load() == firstWorker && workerMta.load(), "One shared MTA worker across target windows");
}

void CoalescingAndPendingCountBound() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueLimits limits;
    limits.maxPending = 2;
    limits.maxPendingBytes = 2 * sizeof(RenderRequest);
    receiver.Start(limits, Pinned(gate));
    gate->Entered();
    Check(receiver.queue->Submit(Request(100, 1)) == S_OK, "First target");
    Check(receiver.queue->Submit(Request(100, 2)) == S_OK, "Replace pending target");
    Check(receiver.queue->Submit(Request(100, 3)) == S_OK, "Replace latest pending target");
    Check(receiver.queue->Submit(Request(101)) == S_OK, "Second pending target");
    Check(receiver.queue->Submit(Request(102)) == Quota, "Pending count quota surfaced");
    const auto pending = receiver.queue->Status();
    Check(pending.pending == 2 && pending.maxPending == 2 && pending.coalesced == 2
        && pending.pendingBytes == 2 * sizeof(RenderRequest), "Bounded coalesced pending accounting");
    Check(pending.renderRequests == 5 && pending.acceptedRequests == 4
        && pending.renderPreparations == 0 && pending.tileGenerations == 0, "Pinned worker does no speculative render");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().ready == 2; });
    Check(receiver.queue->Status().notifications == 1 && receiver.callbacks == 0, "One coalesced owned notification");
    receiver.Collect(2);
    Check(receiver.callbacks == 1 && receiver.results[0].request->token.appearanceRevision == 3
        && receiver.results[1].request->token.target.hwnd == 101, "Only latest target job delivered");
    Check(receiver.queue->Status().staleCancelled == 2 && receiver.queue->Status().renderPreparations == 2,
          "Superseded jobs never prepared");
}

void PendingBytesBoundIncludesChart() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueLimits limits;
    limits.maxPending = 8;
    limits.maxPendingBytes = 2 * sizeof(RenderRequest);
    receiver.Start(limits, Pinned(gate));
    gate->Entered();
    Check(receiver.queue->Submit(Request(100)) == S_OK && receiver.queue->Submit(Request(101)) == S_OK,
          "Pending byte capacity");
    Check(receiver.queue->Submit(Request(102)) == Quota, "Bytes bound before count bound");
    auto chart = Request(103);
    chart.tile.chart = std::make_shared<const knitting::Chart>("large-input", 1, 32, 32,
        std::vector<std::uint32_t>(1024, 0xff112233));
    Check(receiver.queue->Submit(chart) == Quota, "Owned immutable chart charged as input");
    Check(receiver.queue->Status().quotaRejected == 2
        && receiver.queue->Status().maxPendingBytes == 2 * sizeof(RenderRequest), "Exact pending byte high-water");
    Check(receiver.queue->Stop() == S_OK, "Stop releases pinned pending work");
    Check(receiver.queue->Status().pendingBytes == 0, "Pending bytes released");
}

void ReadyCountBackpressure() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueLimits limits;
    limits.maxReady = 1;
    receiver.Start(limits, Pinned(gate));
    gate->Entered();
    Check(receiver.queue->Submit(Request(100)) == S_OK && receiver.queue->Submit(Request(101)) == S_OK,
          "Queue two while one ready slot");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Sleep(20);
    const auto blocked = receiver.queue->Status();
    Check(blocked.pending == 1 && blocked.renderPreparations == 1 && blocked.active == 0,
          "Worker sleeps before preparing output without result capacity");
    receiver.Collect(2);
    Check(receiver.queue->Status().maxReady == 1 && receiver.queue->Status().notifications == 2,
          "Draining releases bounded ready capacity");
}

void ReadyBytesBackpressure() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueLimits limits;
    limits.maxReadyBytes = 28000;
    receiver.Start(limits, Pinned(gate));
    gate->Entered();
    Check(receiver.queue->Submit(Request(100)) == S_OK && receiver.queue->Submit(Request(101)) == S_OK,
          "Queue two under retained result byte cap");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Sleep(20);
    const auto blocked = receiver.queue->Status();
    Check(blocked.pending == 1 && blocked.readyBytes <= limits.maxReadyBytes
        && blocked.renderPreparations == 1, "Ready bytes reserve before render, not after over-allocation");
    receiver.Collect(2);
    Check(receiver.queue->Status().maxReadyBytes <= limits.maxReadyBytes
        && receiver.queue->Status().readyBytes == 0, "Ready byte accounting drains");
}

void EveryAcceptedCurrentJobCompletesAndCapacityRecovers() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueLimits limits;
    limits.maxPending = 8;
    limits.maxReady = 1;
    limits.maxReadyBytes = 28000;
    receiver.Start(limits, Pinned(gate));
    gate->Entered();
    for (tracking::WindowHandle hwnd = 100; hwnd < 108; ++hwnd) {
        auto request = Request(hwnd);
        if (hwnd == 104) request.frame.deviceScale = 0.f;
        Check(receiver.queue->Submit(request) == S_OK, "Accept bounded current request");
    }
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Sleep(20);
    Check(receiver.queue->Status().pending == 7, "Full ready queue retains every accepted pending completion");
    receiver.Collect(8);
    for (std::size_t i = 0; i < receiver.results.size(); ++i) {
        Check(receiver.results[i].request->token == Request(100 + i).token, "Each current token completes once in order");
        if (i == 4) ExpectError(receiver.results[i], RenderErrorKind::InvalidInput, E_INVALIDARG);
        else (void)Perimeter(receiver.results[i]);
    }
    const auto drained = receiver.queue->Status();
    Check(drained.acceptedRequests == 8 && drained.pending == 0 && drained.ready == 0
        && drained.readyBytes == 0 && drained.staleCancelled == 0 && drained.maxReady == 1,
          "No accepted-current job silently dropped under output pressure");
    Check(receiver.queue->Submit(Request(108)) == S_OK, "Pending/ready capacity recovers after draining");
    receiver.Collect(9);
    Check(receiver.results[8].request->token == Request(108).token, "Recovered capacity produces next real completion");
}

void ExplicitResourceAndInvalidErrors() {
    for (int mode = 0; mode < 3; ++mode) {
        Receiver receiver;
        RenderQueueLimits limits;
        if (mode == 0) limits.maxTransientBytes = 1;
        if (mode == 1) limits.maxPerimeterBytes = 4096;
        if (mode == 2) limits.maxReadyBytes = sizeof(RenderRequest) + sizeof(RenderReady);
        receiver.Start(limits);
        Check(receiver.queue->Submit(Request()) == S_OK, "Accept request for typed quota completion");
        receiver.Collect(1);
        ExpectError(receiver.results[0], RenderErrorKind::ResourceQuota, Quota);
        const auto status = receiver.queue->Status();
        Check(status.tileGenerations == 0 && status.renderPreparations == 0 && status.transientBytes == 0
            && status.quotaRejected == 1, "Quota failure before raster allocation");
    }
    Receiver receiver;
    receiver.Start();
    auto invalid = Request();
    invalid.frame.widthDip = std::numeric_limits<float>::quiet_NaN();
    Check(receiver.queue->Submit(invalid) == S_OK, "Invalid frame yields typed completion");
    receiver.Collect(1);
    ExpectError(receiver.results[0], RenderErrorKind::InvalidInput, E_INVALIDARG);
    invalid = Request(101);
    invalid.tile.gauge.rows = -1.f;
    Check(receiver.queue->Submit(invalid) == S_OK, "Invalid tile yields typed completion");
    receiver.Collect(2);
    ExpectError(receiver.results[1], RenderErrorKind::InvalidInput, E_INVALIDARG);
    auto huge = Request(102);
    huge.frame.contentWidthPixels = huge.frame.contentHeightPixels = 32768;
    Check(receiver.queue->Submit(huge) == S_OK, "Oversize frame yields typed completion");
    receiver.Collect(3);
    ExpectError(receiver.results[2], RenderErrorKind::ResourceQuota, Quota);
    Check(receiver.queue->Status().renderPreparations == 0 && receiver.queue->Status().tileGenerations == 0,
          "Invalid/oversize work does not become successful placeholders");
}

void ExactCancellationAndHandleReuse() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    receiver.Start({}, Pinned(gate));
    gate->Entered();
    const auto old = Request(100);
    Check(receiver.queue->Submit(old) == S_OK, "Queue old lifetime");
    Check(receiver.queue->Cancel(old.token.target) == S_OK, "Cancel exact old lifetime");
    Check(receiver.queue->Submit(old) == S_FALSE, "Cancelled lifetime cannot resurrect");
    auto reused = old;
    ++reused.token.target.generation;
    ++reused.token.target.pid;
    ++reused.token.target.processStart;
    Check(receiver.queue->Submit(reused) == S_OK, "Reused HWND with newer full identity");
    Check(receiver.queue->Cancel(old.token.target) == S_FALSE, "Old cancellation cannot kill reused HWND");
    Check(receiver.queue->Submit(old) == S_FALSE, "Old lifetime cannot replace new pending");
    auto imposter = reused;
    ++imposter.token.target.pid;
    Check(receiver.queue->Submit(imposter) == S_FALSE, "Same generation different PID is not same lifetime");
    gate->Open();
    receiver.Collect(1);
    Check(receiver.results[0].request->token == reused.token, "Only exact new lifetime delivered");
    auto next = reused;
    next.token.appearanceRevision = 2;
    Check(receiver.queue->Submit(next) == S_OK, "New appearance accepted");
    Check(receiver.queue->Submit(reused) == S_FALSE, "Old appearance cannot replace newer");
    receiver.Collect(2);
    auto cancelledReuse = reused;
    ++cancelledReuse.token.target.generation;
    Check(receiver.queue->Cancel(cancelledReuse.token.target) == S_OK, "Cancel newer lifetime before first submission");
    Check(receiver.queue->Submit(cancelledReuse) == S_FALSE, "Unsubmitted cancelled reused lifetime cannot resurrect");
}

void ActiveCancellationBeforeRender() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueTestHooks hooks;
    hooks.beforeRender = [gate](const RenderRequest&, std::stop_token stop) { gate->Wait(stop); };
    receiver.Start({}, std::move(hooks));
    const auto request = Request();
    Check(receiver.queue->Submit(request) == S_OK, "Start cancellable active request");
    gate->Entered();
    Check(receiver.queue->Cancel(request.token.target) == S_OK, "Cancel dequeued request");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().active == 0; });
    Check(receiver.queue->Status().renderPreparations == 0 && receiver.queue->Status().tileGenerations == 0
        && receiver.queue->Status().ready == 0 && receiver.queue->Status().staleCancelled == 1,
          "Cancelled before raster executes, no result resurrection");
}

void ActiveAndReadyInvalidation() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueTestHooks hooks;
    hooks.afterRender = [gate](const RenderRequest&, std::stop_token stop) { gate->Wait(stop); };
    receiver.Start({}, std::move(hooks));
    auto request = Request();
    Check(receiver.queue->Submit(request) == S_OK, "Prepare old config on real renderer");
    gate->Entered();
    Check(receiver.queue->Invalidate(2, 1) == S_OK, "Invalidate active config");
    Check(receiver.queue->Submit(request) == S_FALSE, "Reject old config request");
    request.configRevision = 2;
    Check(receiver.queue->Submit(request) == S_OK, "Accept new config request");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Check(receiver.queue->Status().staleCancelled == 2, "Active and submitted old config counted stale");
    Check(receiver.queue->Invalidate(2, 2) == S_OK, "Invalidate ready old device before owner drain");
    Check(receiver.queue->Status().readyBytes == 0, "Invalidated ready bytes released");
    Check(receiver.queue->Submit(request) == S_FALSE, "Reject old device request");
    request.deviceGeneration = 2;
    Check(receiver.queue->Submit(request) == S_OK, "Accept new device request");
    receiver.Collect(1);
    Check(receiver.results[0].request->configRevision == 2
        && receiver.results[0].request->deviceGeneration == 2, "Only current config/device delivered");
    Check(receiver.queue->Status().tileGenerations == 1 && receiver.queue->Status().cacheHits == 2,
          "Config/device generations invalidate attachments, not stable CPU appearance cache");
    Check(receiver.queue->Invalidate(1, 2) == E_INVALIDARG
        && receiver.queue->Invalidate(2, 1) == E_INVALIDARG
        && receiver.queue->Invalidate(2, 2) == S_FALSE, "Generations cannot roll backward");
}

void ReadyCancellationAndNotificationCoalescing() {
    Receiver receiver;
    receiver.Start();
    const auto old = Request(100);
    Check(receiver.queue->Submit(old) == S_OK, "Prepare cancellable ready result");
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Check(receiver.queue->Cancel(old.token.target) == S_OK, "Cancel ready result");
    Check(receiver.queue->Status().ready == 0 && receiver.queue->Status().readyBytes == 0,
          "Cancelled result and its memory released");
    auto reused = old;
    ++reused.token.target.generation;
    Check(receiver.queue->Submit(reused) == S_OK, "Queue reused lifetime before old notification drains");
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Check(receiver.queue->Status().notifications == 1, "Existing posted wake covers latest ready work");
    receiver.Collect(1);
    Check(receiver.callbacks == 1 && receiver.results[0].request->token == reused.token, "No cancelled delivery");
}

void NewAppearanceRetiresActiveAndReadyResults() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueTestHooks hooks;
    hooks.afterRender = [gate](const RenderRequest&, std::stop_token stop) { gate->Wait(stop); };
    receiver.Start({}, std::move(hooks));
    Check(receiver.queue->Submit(Request(100, 1)) == S_OK, "Prepare active first appearance");
    gate->Entered();
    Check(receiver.queue->Submit(Request(100, 2)) == S_OK, "Replace active appearance without raw target reference");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    Check(receiver.queue->Status().staleCancelled == 1, "Active old appearance explicitly retired");
    Check(receiver.queue->Submit(Request(100, 3)) == S_OK, "Replace ready appearance before owner attachment");
    Check(receiver.queue->Status().ready == 0 && receiver.queue->Status().readyBytes == 0,
          "Replacement clears old ready retained charge");
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    receiver.Collect(1);
    Check(receiver.results[0].request->token == Request(100, 3).token
        && receiver.queue->Status().staleCancelled == 2, "Only final appearance completes after superseding active/ready");
    Check(receiver.queue->Status().tileGenerations == 1 && receiver.queue->Status().cacheHits == 2,
          "Appearance token retirement does not perturb full stable cache keys");
}

void AuxiliaryChartTilesAndPathQuotas() {
    Receiver receiver;
    receiver.Start();
    auto request = Request(100);
    knitting::ChartAppearance look;
    look.flags = designs::ChartSolidCorners | designs::ChartOuterCuff;
    look.cornerArgb = 0xff663322;
    look.cuffArgb = 0xff112244;
    request.tile.chart = std::make_shared<const knitting::Chart>("auxiliary-chart", 1, 2, 2,
        std::vector<std::uint32_t>{0, 0xffaabbcc, 0xffaabbcc, 0}, look);
    Check(receiver.queue->Submit(request) == S_OK, "Real chart with auxiliary corner/cuff tiles");
    receiver.Collect(1);
    (void)Perimeter(receiver.results[0]);
    Check(receiver.queue->Status().tileGenerations == 3 && receiver.queue->Status().cacheHits == 0,
          "Main and both auxiliary tile generations accurately counted");
    request.token.target.hwnd = 101;
    Check(receiver.queue->Submit(request) == S_OK, "Reuse all auxiliary cached tiles");
    receiver.Collect(2);
    Check(receiver.queue->Status().tileGenerations == 3 && receiver.queue->Status().cacheHits == 3,
          "Every actual cache Get hit, including auxiliary tiles, counted");
    auto oversized = Request(102);
    look = {};
    look.flags = designs::ChartSculptedYarn;
    oversized.tile.gauge.rows = 128.f;
    oversized.tile.gauge.aspect = .5f;
    oversized.frame.bandDip = 2.f;
    oversized.tile.chart = std::make_shared<const knitting::Chart>("path-quota", 1, 1, 1,
        std::vector<std::uint32_t>{0}, look);
    Check(receiver.queue->Submit(oversized) == S_OK, "Accept request for explicit path allocation quota");
    receiver.Collect(3);
    ExpectError(receiver.results[2], RenderErrorKind::ResourceQuota, Quota);
    Check(receiver.queue->Status().renderPreparations == 2 && receiver.queue->Status().tileGenerations == 3,
          "Chart path quota stops before native scratch/path allocation");
}

void DenseChartTransientReservationCoversActualCorePeak() {
    static_assert(sizeof(knitting::detail::YarnCell) == 12);
    constexpr std::size_t cellBytes = 2501568;
    // v145 x64 allocation sizes: toolchain-specific; Debug adds a sentinel.
#ifdef _DEBUG
    constexpr std::size_t allocationBytes = 2501615;
#else
    constexpr std::size_t allocationBytes = 2501607;
#endif
    constexpr std::size_t previousGrowthPeak = 6221460;
    constexpr std::size_t budget = 5u * 1024u * 1024u;
    Check(cellBytes < budget && previousGrowthPeak > budget, "Dense fixture distinguishes reserve from overlapping growth");
    auto request = Request();
    request.frame.bandDip = 2.f;
    request.tile.gauge.rows = 128.f;
    request.tile.gauge.aspect = 1.28f;
    knitting::ChartAppearance look;
    look.flags = designs::ChartSculptedYarn;
    request.tile.chart = std::make_shared<const knitting::Chart>("dense-transient-regression", 1, 1, 1,
        std::vector<std::uint32_t>{0}, look);
    {
        Receiver receiver;
        RenderQueueLimits limits;
        limits.maxTransientBytes = budget;
        RenderQueueTestHooks hooks;
        hooks.beforeRender = [](const RenderRequest&, std::stop_token) { allocation_probe::Start(); };
        hooks.afterRender = [](const RenderRequest&, std::stop_token) { allocation_probe::Finish(); };
        receiver.Start(limits, std::move(hooks));
        Check(receiver.queue->Submit(request) == S_OK, "Accept admissible dense chart under 5 MiB transient cap");
        receiver.Collect(1, 120s);
        const auto& perimeter = Perimeter(receiver.results[0]);
        Check(perimeter.tile->layout.columns == 400 && perimeter.tile->layout.rows == 512
            && perimeter.tile->image.width == 32 && perimeter.tile->image.height == 32,
              "Real dense chart layout is valid, not over the CPU path quota");
        std::cout << "Dense YarnCell allocations=" << allocation_probe::allocations.load()
                  << " largest=" << allocation_probe::largest.load()
                  << " peak=" << allocation_probe::peak.load()
                  << " live=" << allocation_probe::live.load() << '\n';
        Check(allocation_probe::allocations.load() == 1 && allocation_probe::largest.load() == allocationBytes
            && allocation_probe::peak.load() == allocationBytes && allocation_probe::live.load() == 0,
              "Real core reserves one exact YarnCell allocation with no old/new overlap");
        const auto status = receiver.queue->Status();
        std::cout << "Dense transient reservation=" << status.maxTransientBytes << " limit=" << budget << '\n';
        Check(status.maxTransientBytes >= allocation_probe::peak.load()
            && status.maxTransientBytes < allocationBytes + 128u * 1024u
            && status.maxTransientBytes <= budget && status.transientBytes == 0
            && status.quotaRejected == 0 && status.tileGenerations == 1,
              "Successful dense completion reservation bounds observed core peak");
    }
    {
        Receiver receiver;
        RenderQueueLimits limits;
        limits.maxTransientBytes = allocationBytes - 1;
        std::atomic<bool> rendered{};
        RenderQueueTestHooks hooks;
        hooks.beforeRender = [&](const RenderRequest&, std::stop_token) { rendered = true; };
        receiver.Start(limits, std::move(hooks));
        Check(receiver.queue->Submit(request) == S_OK, "Accept dense request for explicit lower-budget quota completion");
        receiver.Collect(1);
        ExpectError(receiver.results[0], RenderErrorKind::ResourceQuota, Quota);
        const auto status = receiver.queue->Status();
        Check(!rendered.load() && status.renderPreparations == 0 && status.tileGenerations == 0
            && status.maxTransientBytes == 0 && status.quotaRejected == 1,
              "Quota rejects below actual core allocation before any rendering");
    }
}

void ColdStartConfigurationRevisionZero() {
    Receiver receiver;
    Check(RenderQueue::Create(receiver.window, {}, receiver.queue) == S_OK, "Create default-config queue");
    Check(receiver.queue->Start(0, 0) == E_INVALIDARG, "Cold start still requires live device generation");
    Check(receiver.queue->Start(0, 1) == S_OK, "Configuration revision zero is valid before any stored commit");
    auto request = Request();
    request.configRevision = 0;
    Check(receiver.queue->Submit(request) == S_OK, "Submit actual cold-start appearance");
    receiver.Collect(1);
    (void)Perimeter(receiver.results[0]);
    Check(receiver.results[0].request->configRevision == 0, "Cold-start completion retains revision zero");
    Check(receiver.queue->Invalidate(1, 1) == S_OK, "First committed configuration invalidates cold-start work");
    Check(receiver.queue->Submit(request) == S_FALSE, "Old default revision cannot re-enter after commit");
    request.configRevision = 1;
    Check(receiver.queue->Submit(request) == S_OK, "Current first-commit revision remains admissible");
    receiver.Collect(2);
    Check(receiver.results[1].request->configRevision == 1, "First committed completion follows default");
}

void BoundedLifetimeLedger() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueLimits limits;
    limits.maxTargetKeys = 1;
    receiver.Start(limits, Pinned(gate));
    gate->Entered();
    auto old = Request(100);
    Check(receiver.queue->Cancel(old.token.target) == S_OK, "Tombstone unknown full lifetime");
    Check(receiver.queue->Submit(old) == S_FALSE, "Unknown cancelled lifetime remains cancelled");
    Check(receiver.queue->Submit(Request(101)) == Quota, "Bounded ledger does not forget cancellation");
    ++old.token.target.generation;
    Check(receiver.queue->Submit(old) == S_OK, "Reuse existing high-water ledger slot");
    Check(receiver.queue->Cancel(Request(101).token.target) == Quota, "New ledger cancellation quota explicit");
    gate->Open();
    receiver.Collect(1);
}

void StopPendingAndActiveWithoutDetachedWork() {
    {
        auto gate = std::make_shared<Gate>();
        Receiver receiver;
        receiver.Start({}, Pinned(gate));
        gate->Entered();
        Check(receiver.queue->Submit(Request(100)) == S_OK && receiver.queue->Submit(Request(101)) == S_OK,
              "Queue pending jobs before stop");
        Check(receiver.queue->Stop() == S_OK, "Stop requests test gate cancellation and joins worker");
        const auto status = receiver.queue->Status();
        Check(status.state == RenderQueueState::Stopped && status.pending == 0 && status.ready == 0
            && status.active == 0 && status.transientBytes == 0 && status.tileGenerations == 0
            && status.staleCancelled == 2, "Stopped pending queue owns no work");
        Check(receiver.queue->Start(1, 1) == E_UNEXPECTED && receiver.queue->Submit(Request()) == E_UNEXPECTED,
              "Stopped queue cannot restart or accept work");
        Check(receiver.queue->Stop() == S_OK, "Stop is idempotent");
    }
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    std::atomic<DWORD> workerId{};
    RenderQueueTestHooks hooks;
    hooks.afterRender = [gate, &workerId](const RenderRequest&, std::stop_token stop) {
        workerId.store(GetCurrentThreadId());
        gate->Wait(stop);
    };
    receiver.Start({}, std::move(hooks));
    Check(receiver.queue->Submit(Request()) == S_OK, "Start one finite real CPU job");
    gate->Entered();
    const auto worker = OpenThread(SYNCHRONIZE, FALSE, workerId.load());
    Check(worker != nullptr, "Observe actual in-flight worker lifetime");
    Check(receiver.queue->Stop() == S_OK, "Stop joins finite prepared job before receiver destruction");
    const auto wait = WaitForSingleObject(worker, 0);
    CloseHandle(worker);
    Check(wait == WAIT_OBJECT_0, "Worker actually exits before Stop returns");
    const auto status = receiver.queue->Status();
    Check(status.renderPreparations == 1 && status.tileGenerations == 1 && status.staleCancelled == 1
        && status.ready == 0 && status.residentCacheBytes == 0 && !status.notificationPending,
          "In-flight stopped result not published, worker cache destroyed");
    receiver.Pump();
    Check(receiver.callbacks == 0 && receiver.results.empty(), "Stop leaves no late host callback");
}

void StopRemovesPostedNotifications() {
    Receiver receiver;
    receiver.Start();
    Check(receiver.queue->Submit(Request()) == S_OK, "Prepare posted result");
    receiver.Wait([&] { return receiver.queue->Status().ready == 1; });
    MSG message{};
    Check(PeekMessageW(&message, receiver.window, RenderQueue::ReadyMessage,
                      RenderQueue::ReadyMessage, PM_NOREMOVE) != FALSE, "Real notification posted");
    const auto staleCookie = message.wParam;
    Check(receiver.queue->Stop() == S_OK, "Stop before receiver destruction");
    Check(!PeekMessageW(&message, receiver.window, RenderQueue::ReadyMessage,
                       RenderQueue::ReadyMessage, PM_NOREMOVE), "Stop removes reserved posted message");
    Check(!receiver.queue->OwnsNotification(staleCookie, 0), "Stopped queue ignores stale cookie");
    receiver.queue.reset();
    receiver.Start();
    Check(!receiver.queue->OwnsNotification(staleCookie, 0), "New queue cannot accept old receiver-lifetime cookie");
    Check(receiver.queue->Submit(Request()) == S_OK, "New queue real work");
    receiver.Collect(1);
}

void ChartKeysAndCacheBudget() {
    Receiver receiver;
    RenderQueueLimits limits;
    limits.cacheBytes = 20000;
    receiver.Start(limits);
    auto request = Request(100);
    request.tile.chart = std::make_shared<const knitting::Chart>("immutable-repeat", 7, 2, 2,
        std::vector<std::uint32_t>{0, 0xff8899aa, 0xff8899aa, 0});
    Check(receiver.queue->Submit(request) == S_OK, "Chart through real renderer");
    receiver.Collect(1);
    const auto firstTile = Perimeter(receiver.results[0]).tile;
    request = Request(101);
    request.tile.chart = std::make_shared<const knitting::Chart>("immutable-repeat", 7, 2, 2,
        std::vector<std::uint32_t>{0, 0xff8899aa, 0xff8899aa, 0});
    Check(receiver.queue->Submit(request) == S_OK, "Equivalent owned chart key");
    receiver.Collect(2);
    Check(Perimeter(receiver.results[1]).tile == firstTile && receiver.queue->Status().tileGenerations == 1,
          "Semantic/content chart key, not pointer identity");
    request.token.target.hwnd = 102;
    request.tile.chart = std::make_shared<const knitting::Chart>("immutable-repeat", 7, 2, 2,
        std::vector<std::uint32_t>{0, 0xff112233, 0xff8899aa, 0});
    Check(receiver.queue->Submit(request) == S_OK, "Same semantic revision changed literal cells");
    receiver.Collect(3);
    Check(Perimeter(receiver.results[2]).tile != firstTile && receiver.queue->Status().tileGenerations == 2,
          "All literal appearance cells participate in cache key");
    Check(receiver.queue->Status().residentCacheBytes <= limits.cacheBytes, "Configured cache byte budget enforced");
    Receiver uncached;
    limits.cacheBytes = 1;
    uncached.Start(limits);
    Check(uncached.queue->Submit(Request(200)) == S_OK, "Tile larger than cache can still render");
    uncached.Collect(1);
    Check(uncached.queue->Status().residentCacheBytes == 0 && Perimeter(uncached.results[0]).tile->ByteSize() > 1,
          "Oversize tile not charged as resident cache");
}

void GpuInputsAndBudget() {
    Receiver receiver;
    receiver.Start();
    auto request = Request();
    request.frame.renderer = Renderer::Gpu;
    Check(receiver.queue->Submit(request) == S_OK, "GPU inputs through owned worker");
    receiver.Collect(1);
    const auto& p = Perimeter(receiver.results[0]);
    Check(receiver.results[0].request->frame.renderer == Renderer::Gpu,
        "Immutable selection reaches ready GPU perimeter");
    std::size_t stripBytes = 0;
    for (const auto& piece : p.pieces) {
        Check(p.assembly && piece.width && piece.height && piece.image.pixels.empty(),
            "GPU perimeter carries an assembly recipe and no CPU strip pixels");
        stripBytes += std::size_t{piece.width} * piece.height * 4;
    }
    bool rejected = false;
    try { (void)KnitRaster::Compose(p); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "CPU Compose cannot silently publish unshaded GPU inputs");
    Receiver bounded;
    RenderQueueLimits limits;
    limits.maxPerimeterBytes = stripBytes * 3 / 2;
    bounded.Start(limits);
    auto cpuRequest = request;
    cpuRequest.frame.renderer = Renderer::Cpu;
    Check(bounded.queue->Submit(cpuRequest) == S_OK, "CPU request within the same strip budget");
    bounded.Collect(1);
    Check(std::holds_alternative<std::shared_ptr<const KnitPerimeter>>(bounded.results[0].outcome),
        "CPU strips fit a budget below twice their size");
    request.token.target.hwnd = 102;
    Check(bounded.queue->Submit(request) == S_OK, "Over-budget GPU request completes with explicit error");
    bounded.Collect(2);
    const auto* error = std::get_if<RenderError>(&bounded.results[1].outcome);
    Check(error && error->kind == RenderErrorKind::ResourceQuota && error->result == Quota,
        "GPU colour and height upload textures are admitted before allocation");
    request.token.target.hwnd = 101;
    request.frame.renderer = static_cast<Renderer>(99);
    Check(receiver.queue->Submit(request) == S_OK, "Invalid renderer typed completion");
    receiver.Collect(2);
    error = std::get_if<RenderError>(&receiver.results[1].outcome);
    Check(error && error->kind == RenderErrorKind::InvalidInput && error->result == E_INVALIDARG,
        "Unknown renderer rejected without fallback");
}

void ReceiverOwnershipAndWrongThreadCalls() {
    Receiver receiver;
    receiver.Start();
    std::array<HRESULT, 7> errors{};
    std::jthread foreign([&] {
        std::unique_ptr<RenderQueue> other;
        errors[0] = RenderQueue::Create(receiver.window, {}, other);
        errors[1] = receiver.queue->Start(1, 1);
        errors[2] = receiver.queue->Submit(Request());
        errors[3] = receiver.queue->Cancel(Request().token.target);
        errors[4] = receiver.queue->Invalidate(2, 1);
        std::vector<RenderReady> results;
        errors[5] = receiver.queue->Drain(1, 0, results);
        errors[6] = receiver.queue->Stop();
    });
    foreign.join();
    Check(errors[0] == E_ACCESSDENIED, "Create rejects receiver not owned by caller thread");
    for (std::size_t i = 1; i < errors.size(); ++i)
        Check(errors[i] == RPC_E_WRONG_THREAD, "Only owned receiver thread mutates or drains");
    std::unique_ptr<RenderQueue> other;
    Check(RenderQueue::Create(GetDesktopWindow(), {}, other) == E_ACCESSDENIED,
          "Create rejects foreign process receiver");
    Check(receiver.queue->Submit(Request()) == S_OK, "Owned receiver accepts work");
    receiver.Collect(1);
    Check(!receiver.queue->OwnsNotification(receiver.lastCookie, 1)
        && !receiver.queue->OwnsNotification(receiver.lastCookie + 1, 0), "Cookie and reserved payload validated");
    std::vector<RenderReady> results;
    Check(receiver.queue->Drain(receiver.lastCookie + 1, 0, results) == E_INVALIDARG, "Cannot drain foreign cookie");
}

void StartRevalidatesReceiverAndPostFailureIsVisible() {
    {
        Receiver receiver;
        Check(RenderQueue::Create(receiver.window, {}, receiver.queue) == S_OK, "Create before receiver death");
        Check(DestroyWindow(receiver.window) != FALSE, "Destroy receiver without starting");
        receiver.window = nullptr;
        Check(receiver.queue->Start(1, 1) == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE),
              "Start revalidates current PID/thread receiver");
    }
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    RenderQueueTestHooks hooks;
    hooks.afterRender = [gate](const RenderRequest&, std::stop_token stop) { gate->Wait(stop); };
    receiver.Start({}, std::move(hooks));
    Check(receiver.queue->Submit(Request()) == S_OK, "Real job before deliberate receiver violation");
    gate->Entered();
    Check(DestroyWindow(receiver.window) != FALSE, "Destroy receiver to exercise native notification failure");
    receiver.window = nullptr;
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().state == RenderQueueState::Faulted; });
    const auto status = receiver.queue->Status();
    Check(status.notificationFailures == 1 && FAILED(status.error) && status.ready == 0
        && status.readyBytes == 0 && !status.notificationPending, "Post failure is terminal explicit status, not stuck result");
    Check(receiver.queue->Submit(Request(101)) == status.error, "Host submission observes notification failure HRESULT");
    Check(receiver.queue->Stop() == S_OK, "Faulted queue joins without leak");
}

void NativePostMessageQuotaFailureRetiresAcceptedWork() {
    auto gate = std::make_shared<Gate>();
    Receiver receiver;
    receiver.Start({}, Pinned(gate));
    gate->Entered();
    Check(receiver.queue->Submit(Request()) == S_OK, "Accept real work before native notification queue pressure");
    constexpr UINT filler = WM_APP + 77;
    DWORD postError{};
    for (std::size_t i = 0; i < 65536; ++i) {
        if (!PostMessageW(receiver.window, filler, 0, 0)) {
            postError = GetLastError();
            break;
        }
    }
    Check(postError == ERROR_NOT_ENOUGH_QUOTA, "Reach native thread PostMessage quota on owned receiver only");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().state == RenderQueueState::Faulted; });
    const auto status = receiver.queue->Status();
    Check(status.error == Quota && status.notificationFailures == 1 && status.notifications == 0
        && status.ready == 0 && status.readyBytes == 0 && status.staleCancelled == 1,
          "Native post failure explicitly retires accepted completion instead of sticking host gate");
    Check(receiver.queue->Submit(Request(101)) == Quota, "Terminal native post error returned to host");
    MSG message{};
    while (PeekMessageW(&message, receiver.window, filler, filler, PM_REMOVE)) {}
    Check(receiver.queue->Stop() == S_OK, "Native notification fault stops/join cleanly");
}

std::uint64_t FileTimeValue(FILETIME value) {
    return static_cast<std::uint64_t>(value.dwHighDateTime) << 32 | value.dwLowDateTime;
}

void IdleWorkerDoesNoCpuOrGenerations() {
    Receiver receiver;
    std::atomic<DWORD> workerId{};
    RenderQueueTestHooks hooks;
    hooks.beforeFirstDequeue = [&](std::stop_token) { workerId.store(GetCurrentThreadId()); };
    receiver.Start({}, std::move(hooks));
    receiver.Wait([&] { return workerId.load() != 0; });
    const auto thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, workerId.load());
    Check(thread != nullptr, "Query actual worker CPU");
    Sleep(20);
    FILETIME created{}, exited{}, kernelBefore{}, userBefore{}, kernelAfter{}, userAfter{};
    const BOOL before = GetThreadTimes(thread, &created, &exited, &kernelBefore, &userBefore);
    Sleep(80);
    const BOOL after = GetThreadTimes(thread, &created, &exited, &kernelAfter, &userAfter);
    CloseHandle(thread);
    Check(before && after, "Measure blocked worker CPU times");
    Check(FileTimeValue(kernelBefore) == FileTimeValue(kernelAfter)
        && FileTimeValue(userBefore) == FileTimeValue(userAfter), "Idle worker consumes zero measured CPU");
    const auto status = receiver.queue->Status();
    Check(status.renderRequests == 0 && status.renderPreparations == 0 && status.tileGenerations == 0
        && status.cacheHits == 0 && status.pending == 0 && status.ready == 0 && status.notifications == 0
        && status.transientBytes == 0 && status.residentCacheBytes == 0, "Idle has no generation, redraw or allocation");
    Check(receiver.queue->Submit(Request()) == S_OK, "Contrasting live work proves worker functional");
    receiver.Collect(1);
    Check(receiver.queue->Status().tileGenerations == 1, "Worker actually prepares requested live CPU output");
}
}

constexpr std::uint32_t SideOneStartPoll = 9;

void SupersededJobAbortsBetweenSides() {
    auto gate = std::make_shared<Gate>();
    std::atomic<std::uint32_t> oldPolls{}, newPolls{};
    Receiver receiver;
    RenderQueueTestHooks hooks;
    hooks.duringRender = [&](const RenderRequest& r, std::uint32_t poll, std::stop_token stop) {
        if (r.token.appearanceRevision != 1) { newPolls.store(poll); return; }
        oldPolls.store(poll);
        if (poll == SideOneStartPoll) gate->Wait(stop);
    };
    receiver.Start({}, std::move(hooks));
    Check(receiver.queue->Submit(Request(100, 1)) == S_OK, "Submit job to supersede");
    gate->Entered();
    Check(receiver.queue->Submit(Request(100, 2)) == S_OK, "Supersede after side zero completed");
    gate->Open();
    receiver.Collect(1);
    receiver.Wait([&] { return receiver.queue->Status().active == 0; });
    Check(receiver.results.size() == 1 && receiver.results[0].request->token.appearanceRevision == 2,
          "Only the newest perimeter publishes");
    Perimeter(receiver.results[0]);
    Check(oldPolls.load() == SideOneStartPoll, "Superseded job runs no stage after the failed poll");
    Check(newPolls.load() == 2 + 4 * 6, "Complete job polls every stage");
    const auto status = receiver.queue->Status();
    Check(status.renderPreparations == 2 && status.staleCancelled == 1 && status.ready == 0
        && status.readyBytes == 0 && status.transientBytes == 0 && status.active == 0 && status.pending == 0,
          "Aborted job leaves no charge or result");
}

void CancelledJobAbortsAndNextJobRendersFresh() {
    auto gate = std::make_shared<Gate>();
    std::atomic<std::uint32_t> cancelledPolls{};
    Receiver receiver;
    RenderQueueTestHooks hooks;
    hooks.duringRender = [&](const RenderRequest& r, std::uint32_t poll, std::stop_token stop) {
        if (r.token.target.hwnd != 100) return;
        cancelledPolls.store(poll);
        if (poll == SideOneStartPoll) gate->Wait(stop);
    };
    receiver.Start({}, std::move(hooks));
    const auto cancelled = Request(100, 1);
    Check(receiver.queue->Submit(cancelled) == S_OK, "Submit job to cancel");
    gate->Entered();
    Check(receiver.queue->Cancel(cancelled.token.target) == S_OK, "Cancel mid-render");
    gate->Open();
    receiver.Wait([&] { return receiver.queue->Status().active == 0 && receiver.queue->Status().staleCancelled == 1; });
    Check(cancelledPolls.load() == SideOneStartPoll && receiver.queue->Status().ready == 0
        && receiver.queue->Status().readyBytes == 0 && receiver.queue->Status().transientBytes == 0,
          "Cancelled job stops and publishes nothing");
    Check(receiver.queue->Submit(Request(101, 1)) == S_OK, "Queue continues after abort");
    receiver.Collect(1);
    knitting::TileCache cache;
    const auto fresh = KnitRaster::RenderPerimeter(Request().frame, Request().tile, cache);
    const auto& p = Perimeter(receiver.results[0]);
    for (std::size_t i = 0; i < 4; ++i)
        Check(p.pieces[i].image.pixels == fresh.pieces[i].image.pixels, "Reused scratch after abort is pixel-identical");
}

void ScratchReuseAndBound() {
    Receiver receiver;
    receiver.Start();
    for (std::uint64_t i = 1; i <= 3; ++i) {
        Check(receiver.queue->Submit(Request(100, i)) == S_OK, "Submit repeated identical frame");
        receiver.Collect(i);
        if (i == 1) receiver.Wait([&] { return receiver.queue->Status().active == 0; });
    }
    receiver.Wait([&] { return receiver.queue->Status().active == 0; });
    const auto status = receiver.queue->Status();
    Check(status.scratchFactories == 2 && status.scratchTargets == 4 && status.scratchUploads == 4,
          "Repeated identical renders create no new factories, targets or uploads");
    Check(status.scratchBytes > 0 && status.scratchBytes <= RenderQueueLimits{}.scratchBytes,
          "Retained scratch is counted and bounded");
    receiver.queue->Stop();
    Check(receiver.queue->Status().scratchBytes == 0, "Stopped worker releases scratch");

    Receiver tiny;
    RenderQueueLimits limits;
    limits.scratchBytes = 1;
    tiny.Start(limits);
    Check(tiny.queue->Submit(Request(100, 1)) == S_OK, "Submit with tiny scratch budget");
    tiny.Collect(1);
    tiny.Wait([&] { return tiny.queue->Status().active == 0; });
    Perimeter(tiny.results[0]);
    Check(tiny.queue->Status().scratchBytes == 0, "Over-budget scratch is not retained");
}

int main() {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 2;
    const std::array tests{
        std::pair{"superseded job aborts between sides", SupersededJobAbortsBetweenSides},
        std::pair{"cancelled job aborts, next render fresh", CancelledJobAbortsAndNextJobRendersFresh},
        std::pair{"scratch reuse and byte bound", ScratchReuseAndBound},
        std::pair{"real CPU fixture/cache reuse", RealCpuFixtureAndCacheReuse},
        std::pair{"coalescing/pending count", CoalescingAndPendingCountBound},
        std::pair{"pending input byte quota", PendingBytesBoundIncludesChart},
        std::pair{"ready count backpressure", ReadyCountBackpressure},
        std::pair{"ready byte backpressure", ReadyBytesBackpressure},
        std::pair{"accepted completion/capacity recovery", EveryAcceptedCurrentJobCompletesAndCapacityRecovers},
        std::pair{"explicit quota/invalid failures", ExplicitResourceAndInvalidErrors},
        std::pair{"exact cancellation/HWND reuse", ExactCancellationAndHandleReuse},
        std::pair{"active cancellation", ActiveCancellationBeforeRender},
        std::pair{"config/device invalidation", ActiveAndReadyInvalidation},
        std::pair{"ready cancellation/coalesced wake", ReadyCancellationAndNotificationCoalescing},
        std::pair{"active/ready appearance retirement", NewAppearanceRetiresActiveAndReadyResults},
        std::pair{"auxiliary chart counters/path quota", AuxiliaryChartTilesAndPathQuotas},
        std::pair{"dense chart actual transient peak", DenseChartTransientReservationCoversActualCorePeak},
        std::pair{"cold-start configuration revision zero", ColdStartConfigurationRevisionZero},
        std::pair{"bounded lifetime ledger", BoundedLifetimeLedger},
        std::pair{"stop pending/active", StopPendingAndActiveWithoutDetachedWork},
        std::pair{"stop notification lifetime", StopRemovesPostedNotifications},
        std::pair{"immutable chart/cache byte budget", ChartKeysAndCacheBudget},
        std::pair{"GPU immutable inputs/byte admission", GpuInputsAndBudget},
        std::pair{"owned receiver/thread", ReceiverOwnershipAndWrongThreadCalls},
        std::pair{"receiver start/post failure", StartRevalidatesReceiverAndPostFailureIsVisible},
        std::pair{"native PostMessage quota retirement", NativePostMessageQuotaFailureRetiresAcceptedWork},
        std::pair{"idle zero CPU/generations", IdleWorkerDoesNoCpuOrGenerations},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS " << name << '\n';
        }
        std::cout << tests.size() << " tests, " << assertions << " assertions passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
