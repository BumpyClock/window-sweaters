#include "OverlayState.h"
#include "RenderState.h"
#include "..\Rendering\Overlay.h"

#include <cstdio>
#include <stdexcept>
#include <cmath>
#include <algorithm>

namespace window_sweaters::host {
void RunHostConsumerContracts();
namespace {
using namespace tracking;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

AliveWindow Window() {
    AliveWindow window;
    window.hwnd = 123;
    window.identity = {45, 678, 90};
    window.className = L"OrdinaryApp";
    window.rect = {10, 20, 410, 320};
    window.dpi = 96;
    return window;
}

WorkToken Token(const Action& action) { return {action.target, action.appearanceRevision}; }

void GeometryAndRecovery() {
    TrackingModel model{Policy{}};
    auto window = Window();
    const auto initial = model.Observe(window, Dirty::Lifecycle, 0);
    const auto token = Token(initial[0]);
    OverlayState state{token.target};
    const PhysicalRect original{10, 20, 410, 320};
    Check(state.ApplyValidated(model, token, true), "Initial work validation");
    Check(state.GeometryUpdate(true) == original && state.NeedsDraw(), "Initial hidden preparation");
    state.appliedRect = original;
    Check(state.ApplyValidated(model, Token(initial[1]), true), "Initial show validation");
    Check(state.requestedVisible && !state.GeometryUpdate(true), "Initial show reuses preparation");

    auto stale = state.target;
    ++stale.generation;
    Check(!state.Matches(stale), "Reject reused HWND generation");
    stale = state.target; ++stale.pid;
    Check(!state.Matches(stale), "Reject reused HWND PID");
    stale = state.target; ++stale.processStart;
    Check(!state.Matches(stale), "Reject reused PID process lifetime");
    stale = state.target; ++stale.hwnd;
    Check(!state.Matches(stale) && state.Matches(token.target), "Match complete target identity");

    window.rect = {-100, -200, 300, 100};
    const auto moved = model.Observe(window, Dirty::Geometry, 10);
    Check(state.Synchronize(model, Token(moved[0])), "Move canonicalization");
    Check(state.GeometryUpdate(false) == window.rect && !state.NeedsDraw()
        && state.requestedVisible, "Pure movement reuses existing surface");
    state.appliedRect = window.rect;
    Check(!state.GeometryUpdate(false), "Duplicate movement is idle");

    window.rect.right = 400;
    const auto resizing = model.Observe(window, Dirty::Geometry, 20);
    Check(state.Synchronize(model, Token(resizing[0])), "Resize hide canonicalization");
    Check(state.NeedsDraw() && !state.GeometryUpdate(false) && !state.requestedVisible,
        "Actual resize defers drawing while hidden");
    window.rect.right = 500;
    const auto resized = model.Observe(window, Dirty::Geometry, 30);
    Check(state.Synchronize(model, Token(resized[0])), "Live resize canonicalization");
    Check(!state.GeometryUpdate(false) && state.appliedRect->right == 300,
        "Live resize does not apply intermediate surface sizes");
    const auto settled = model.Observe(window, Dirty::Settlement, 280);
    Check(state.ApplyValidated(model, Token(settled[0]), true), "Settled show validation");
    Check(state.GeometryUpdate(true) == PhysicalRect{-100, -200, 500, 100}
        && state.requestedVisible, "Settled show prepares only the latest size");
    state.appliedRect = window.rect;
    const auto hidden = model.Observe(UncertainWindow{window.hwnd}, Dirty::Lifecycle, 290);
    Check(state.Synchronize(model, Token(hidden[0])) && !state.requestedVisible,
        "Uncertain query hides without losing the lifetime");
    const auto recovered = model.Observe(window, Dirty::Lifecycle, 300);
    Check(state.ApplyValidated(model, Token(recovered[0]), true), "Uncertainty recovery validation");
    Check(state.requestedVisible && !state.NeedsDraw() && !state.GeometryUpdate(true),
        "Suppression recovery reuses settled surface");

    window.topmost = true;
    const auto topmost = model.Observe(window, Dirty::Ordering, 310);
    Check(state.ApplyValidated(model, Token(topmost[0]), true), "Topmost band canonicalization");
    Check(state.requestedVisible && !state.GeometryUpdate(true),
        "Topmost band retains current geometry and visible intent");
    window.topmost = false;
    const auto ordinary = model.Observe(window, Dirty::Ordering, 320);
    Check(state.ApplyValidated(model, Token(ordinary[0]), true), "Ordinary band canonicalization");
    Check(state.requestedVisible && !state.NeedsDraw(), "Ordinary-band recovery does not redraw");
    state.excluded = true;
    window.rect.left += 100; window.rect.right += 100;
    const auto excluded = model.Observe(window, Dirty::Geometry, 330);
    Check(state.Synchronize(model, Token(excluded[0])), "Excluded helper canonicalization");
    Check(!state.GeometryUpdate(true), "Settings/helper lifetime never prepares decoration");
}

void RecursiveValidationCannotOverridePolicy() {
    for (int transition = 0; transition < 4; ++transition) {
        TrackingModel model{Policy{}};
        auto window = Window();
        const auto initial = model.Observe(window, Dirty::Lifecycle, 0);
        const auto token = Token(initial[0]);
        OverlayState state{token.target};
        Check(state.ApplyValidated(model, token, true), "Initial validated entry");
        state.appliedRect = window.rect;
        window.rect.left += 20; window.rect.right += 20;
        const auto updates = model.Observe(window, Dirty::Geometry, 10);
        const bool acceptedBeforeRecursiveUpdates = model.Accepts(token);
        Check(acceptedBeforeRecursiveUpdates && updates.size() == 1, "Outer query accepted moved target");
        Check(state.Synchronize(model, Token(updates[0])), "Recursive move applies current bounds");
        ActionBatch nested;
        if (transition == 0) nested = model.Observe(UncertainWindow{window.hwnd}, Dirty::Lifecycle, 20);
        else if (transition == 1) {
            window.minimized = true;
            nested = model.Observe(window, Dirty::Lifecycle, 20);
        } else if (transition == 2) nested = model.Observe(DestroyedWindow{window.hwnd}, Dirty::Lifecycle, 20);
        else {
            window.dpi = 144;
            nested = model.Observe(window, Dirty::Geometry, 20);
        }
        Check(!nested.empty(), "Nested query emits a newer transition");
        if (transition != 2) Check(state.Synchronize(model, Token(nested[0])), "Nested policy synchronized");
        Check(!state.ApplyValidated(model, token, acceptedBeforeRecursiveUpdates),
            "Outer accepted result must be rejected after recursive query changes");
        if (transition < 2) Check(!state.requestedVisible, "Old outer Show cannot overwrite a newer hide");
        if (transition == 3) Check(state.dpi == 144 && state.appearanceRevision == 2,
            "Old outer work cannot overwrite a newer DPI/revision");
    }
}

void StaleBatchPayloadUsesCurrentSnapshot() {
    TrackingModel model{Policy{}};
    auto window = Window();
    const auto initialAtA = model.Observe(window, Dirty::Lifecycle, 0);
    const auto token = Token(initialAtA[0]);
    OverlayState state{token.target};
    window.rect = {-100, -200, 300, 100};
    const auto createValidationUpdates = model.Observe(window, Dirty::Geometry, 10);
    Check(createValidationUpdates.size() == 1 && model.Accepts(token), "Create validation moves A to B");
    Check(state.ApplyValidated(model, token, true) && state.pendingRect == PhysicalRect{-100, -200, 300, 100},
        "Create uses the complete current B snapshot");
    state.appliedRect = state.pendingRect;
    Check(model.Observe(window, Dirty::Lifecycle, 20).empty(), "Remaining old Show validation has no delta");
    Check(initialAtA[1].rect == PhysicalRect{10, 20, 410, 320}, "Retained batch Show still contains A");
    Check(state.ApplyValidated(model, Token(initialAtA[1]), true)
        && state.pendingRect == PhysicalRect{-100, -200, 300, 100} && !state.GeometryUpdate(true),
        "Old Show at A must retain canonical B geometry with no redundant draw");
    window.topmost = true;
    const auto band = model.Observe(window, Dirty::Ordering, 30);
    Check(!initialAtA[2].topmost && band[0].topmost, "Old Reorder contains the former band");
    Check(state.ApplyValidated(model, Token(initialAtA[2]), true) && state.topmost
        && !state.GeometryUpdate(true), "Old Reorder must use the current topmost band");
    window.dpi = 144;
    const auto dpi = model.Observe(window, Dirty::Geometry, 40);
    Check(!state.ApplyValidated(model, token, true), "Old DPI work token rejected");
    Check(state.ApplyValidated(model, Token(dpi[0]), true) && state.dpi == 144,
        "New revision uses canonical DPI");
}

geometry::WindowFacts RenderFacts() {
    return {{40, 50, 441, 351}, {0, 0, 1920, 1080}, {0, 0, 1920, 1040},
        120, 1, false, geometry::CornerPreference::Round, geometry::CornerEligibility::StandardFrame};
}

void RenderPublicationTokens() {
    const auto facts = RenderFacts();
    const auto first = geometry::BuildRenderInput(facts, {16, 8});
    const auto& a = std::get<geometry::RenderInput>(first);
    WorkToken token{{123, 45, 678, 1}, 1};
    const auto request = MakeRenderRequest(token, 7, 1, a);
    auto moved = facts;
    moved.physicalTargetBounds = {-20, -30, 381, 271};
    const auto translated = geometry::BuildRenderInput(moved, {16, 8});
    const auto& b = std::get<geometry::RenderInput>(translated);
    Check(MatchesRenderInput(request, token, 7, 1, b), "Move and monitor clipping reuse prepared pixels");
    // 20px band padding plus a 10px drop-shadow margin per side.
    Check(b.physicalOuterBounds.left == -50 && b.physicalOuterBounds.top == -60 &&
        b.physicalOuterBounds.Width() == 461 && b.physicalOuterBounds.Height() == 361,
        "GPU placement uses current B, not queued A, with authoritative physical padding");
    Check(request.frame.contentWidthPixels == 401 && request.frame.contentHeightPixels == 301 &&
        request.frame.physicalPaddingPixels == 20 && request.frame.deviceScale == 1.25f,
        "Native request preserves non-round physical dimensions and DPI");
    Check(SamePreparation(request, MakeRenderRequest(token, 7, 1, b)),
        "Pending request matching ignores stale queued placement and clip");
    Check(!MatchesRenderInput(request, token, 8, 1, b), "Reject obsolete configuration result");
    Check(!MatchesRenderInput(request, token, 7, 2, b), "Reject obsolete device result");
    auto replaced = token; ++replaced.target.generation;
    Check(!MatchesRenderInput(request, replaced, 7, 1, b), "Reject reused HWND result");
    auto newer = token; ++newer.appearanceRevision;
    Check(!MatchesRenderInput(request, newer, 7, 1, b), "Reject obsolete appearance result");
    moved.physicalTargetBounds.right++;
    const auto resized = geometry::BuildRenderInput(moved, {16, 8});
    Check(!MatchesRenderInput(request, token, 7, 1, std::get<geometry::RenderInput>(resized)),
        "Reject physical-size mismatch even with unchanged token");
    Check(!SamePreparation(request, MakeRenderRequest(token, 7, 1, std::get<geometry::RenderInput>(resized))),
        "Old ready result cannot retire a newer same-token pending-size gate");
    const auto wider = geometry::BuildRenderInput(facts, {18, 8});
    Check(!MatchesRenderInput(request, token, 7, 1, std::get<geometry::RenderInput>(wider)),
        "Reject changed width before attaching");
    moved = facts; moved.dpi = 144;
    const auto dpi = geometry::BuildRenderInput(moved, {16, 8});
    Check(!MatchesRenderInput(request, token, 7, 1, std::get<geometry::RenderInput>(dpi)),
        "Reject changed DPI before attaching");
    RenderState state;
    state.pending = request; state.prepared = request; state.failed = request;
    state.retry = request; state.retryQueued = true;
    state.Invalidate();
    Check(!state.pending && !state.prepared && !state.failed && !state.retry && !state.retryQueued,
        "Config/device/terminal invalidation retires every host gate without waiting for completion");
}

void NativePerimeterPublication() {
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    Check(SUCCEEDED(apartment.result), "Initialize native publication COM");
    WNDCLASSW registration{};
    registration.lpfnWndProc = DefWindowProcW;
    registration.hInstance = GetModuleHandleW(nullptr);
    registration.lpszClassName = L"WindowSweaters.Host.Contract";
    Check(RegisterClassW(&registration) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "Register hidden publication receiver");
    struct Receiver {
        HWND window{};
        ~Receiver() { if (window) DestroyWindow(window); }
    } receiver{CreateWindowExW(0, registration.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, registration.hInstance, nullptr)};
    Check(receiver.window != nullptr, "Create isolated hidden publication receiver");
    rendering::GraphicsDevice graphics;
    Check(SUCCEEDED(graphics.Initialize()), "Initialize actual native D2D/DComp device");
    rendering::Overlay overlay;
    Check(SUCCEEDED(overlay.Create(registration.hInstance, receiver.window, graphics)),
        "Create hidden native perimeter overlay");
    std::unique_ptr<rendering::RenderQueue> queue;
    Check(SUCCEEDED(rendering::RenderQueue::Create(receiver.window, {}, queue)) &&
        SUCCEEDED(queue->Start(7, 1)), "Start actual immutable-input MTA render worker");
    const auto initial = geometry::BuildRenderInput(RenderFacts(), {16, 8});
    const WorkToken token{{123, GetCurrentProcessId(), 678, 1}, 1};
    const auto request = MakeRenderRequest(token, 7, 1, std::get<geometry::RenderInput>(initial));
    Check(queue->Submit(request) == S_OK, "Submit actual stockinette preparation");
    std::vector<rendering::RenderReady> ready;
    const auto deadline = GetTickCount64() + 10000;
    while (ready.empty() && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, receiver.window, rendering::RenderQueue::ReadyMessage,
            rendering::RenderQueue::ReadyMessage, PM_REMOVE)) {
            if (queue->OwnsNotification(message.wParam, message.lParam))
                Check(SUCCEEDED(queue->Drain(message.wParam, message.lParam, ready)), "Drain owner-thread ready notification");
        }
    }
    Check(ready.size() == 1, "Worker delivers exactly one real prepared perimeter");
    const auto pixels = std::get<std::shared_ptr<const rendering::KnitPerimeter>>(ready[0].outcome);
    Check(pixels->width == 461 && pixels->height == 361 && pixels->contentInsetPixels == 30 &&
        pixels->shadowMarginPixels == 10,
        "Actual raster dimensions agree with authoritative native geometry");
    auto moved = RenderFacts(); moved.physicalTargetBounds = {-20, -30, 381, 271};
    const auto translated = geometry::BuildRenderInput(moved, {16, 8});
    const auto& current = std::get<geometry::RenderInput>(translated);
    Check(MatchesRenderInput(*ready[0].request, token, 7, 1, current), "Actual moved result passes publication gate");
    const RECT bounds{-50, -60, 411, 301};
    const auto& clip = current.clipRelativeToOuter;
    const auto& content = current.targetRelativeToOuter;
    Check(SUCCEEDED(overlay.Upload(pixels, bounds, {clip.left, clip.top, clip.right, clip.bottom},
        {content.left, content.top, content.right, content.bottom},
        static_cast<float>(current.radiusDip * current.deviceScale),
        static_cast<float>(current.deviceScale))), "Upload four real BGRA pieces with authoritative radius and source tuck");
    Check(EqualRect(&overlay.Bounds(), &bounds) && !IsWindowVisible(overlay.Window()),
        "Prepared upload remains hidden at today's physical position");
    Check(overlay.CommitCount() == 1, "Upload publishes with a single DComp commit");
    Check(SUCCEEDED(graphics.Composition()->WaitForCommitCompletion()), "Actual DComp upload commits");
    const auto before = queue->Status();
    const RECT next{50, 60, 511, 421};
    Check(SUCCEEDED(overlay.UpdatePlacement(next, {0, 0, 461, 361})), "Move already-prepared GPU perimeter without render submission");
    const auto after = queue->Status();
    Check(EqualRect(&overlay.Bounds(), &next) && before.tileGenerations == after.tileGenerations &&
        before.renderRequests == after.renderRequests && before.renderPreparations == after.renderPreparations,
        "Native move preserves worker request, preparation and tile-generation counts");
    Check(overlay.UpdatePlacement({0, 0, 462, 361}, {0, 0, 461, 361}) == E_INVALIDARG,
        "Native placement rejects mismatched physical dimensions");
    const auto commits = overlay.CommitCount();
    const RECT originOnly{70, 80, 531, 441};
    Check(SUCCEEDED(overlay.UpdatePlacement(originOnly, {0, 0, 461, 361})) && EqualRect(&overlay.Bounds(), &originOnly) &&
        overlay.CommitCount() == commits, "Origin-only move with unchanged clip issues no DComp commit");
    Check(SUCCEEDED(overlay.UpdatePlacement(originOnly, {1, 1, 460, 360})) && overlay.CommitCount() == commits + 1,
        "Changed clip commits exactly once");
    for (const auto scale : {1.f, 1.25f, 1.5f, 2.f}) {
        for (const auto radiusDip : {0.f, 4.f, 8.f}) {
            rendering::KnitFrameSpec frame;
            frame.widthDip = 240; frame.heightDip = 140;
            frame.contentWidthPixels = static_cast<std::uint32_t>(240 * scale);
            frame.contentHeightPixels = static_cast<std::uint32_t>(140 * scale);
            frame.bandDip = 16; frame.radiusDip = radiusDip; frame.deviceScale = scale;
            knitting::TileCache cache;
            auto perimeter = std::make_shared<const rendering::KnitPerimeter>(
                rendering::KnitRaster::RenderPerimeter(frame, {}, cache));
            const auto inset = static_cast<LONG>(perimeter->contentInsetPixels);
            const RECT contentRect{inset, inset, inset + static_cast<LONG>(frame.contentWidthPixels),
                inset + static_cast<LONG>(frame.contentHeightPixels)};
            const RECT outer{100, 100, 100 + static_cast<LONG>(perimeter->width), 100 + static_cast<LONG>(perimeter->height)};
            const RECT fullClip{0, 0, static_cast<LONG>(perimeter->width), static_cast<LONG>(perimeter->height)};
            std::array<knitting::RasterImage, 4> gpuPixels;
            Check(SUCCEEDED(overlay.Upload(perimeter, outer, fullClip, contentRect, radiusDip * scale, scale, &gpuPixels)),
                "Upload real rounded/square/round-small source pieces and read masked DComp GPU pixels");
            auto captured = *perimeter;
            for (std::size_t side = 0; side < gpuPixels.size(); ++side) captured.pieces[side].image = std::move(gpuPixels[side]);
            const auto image = rendering::KnitRaster::Compose(captured);
            bool antialiased{};
            for (const auto pixel : image.pixels) {
                const auto a = pixel >> 24;
                Check((pixel & 255) <= a && ((pixel >> 8) & 255) <= a && ((pixel >> 16) & 255) <= a,
                    "Actual GPU rounded-edge channels remain valid premultiplied BGRA");
                antialiased = antialiased || (a > 0 && a < 255);
            }
            Check(antialiased, "Actual GPU rounded ring retains antialiased edge coverage");
            auto alpha = [&](int x, int y) {
                Check(x >= 0 && y >= 0 && x < static_cast<int>(image.width) && y < static_cast<int>(image.height),
                    "Rounded corner pixel sample lies within perimeter");
                return image.pixels[static_cast<std::size_t>(y) * image.width + x] >> 24;
            };
            const float r = radiusDip * scale;
            Check(alpha(static_cast<int>(image.width / 2), inset) > 0,
                "Actual GPU retains original inward one-DIP tuck underneath verified target placement");
            for (int y = contentRect.top + 2; y < contentRect.bottom - 2; ++y) {
                for (int x = contentRect.left + 2; x < contentRect.right - 2; ++x) {
                    const float px = static_cast<float>(x) + .5f, py = static_cast<float>(y) + .5f;
                    const float holeRadius = std::max(0.f, r - scale);
                    const float holeLeft = contentRect.left + scale, holeTop = contentRect.top + scale;
                    const float holeRight = contentRect.right - scale, holeBottom = contentRect.bottom - scale;
                    const float cx = std::clamp(px, holeLeft + holeRadius, holeRight - holeRadius);
                    const float cy = std::clamp(py, holeTop + holeRadius, holeBottom - holeRadius);
                    const float distance = std::hypot(px - cx, py - cy);
                    const bool central = holeRadius > 0 ? distance < holeRadius - 1.25f
                        : px > holeLeft + 1.25f && px < holeRight - 1.25f &&
                            py > holeTop + 1.25f && py < holeBottom - 1.25f;
                    if (central) {
                        if (alpha(x, y) != 0) std::fprintf(stderr, "GPU interior radius=%g scale=%g x=%d y=%d alpha=%u\n",
                            radiusDip, scale, x - inset, y - inset, alpha(x, y));
                        Check(alpha(x, y) == 0, "Actual GPU content-interior alpha is zero");
                    }
                }
            }
            if (r > 0) {
                Check(alpha(inset, inset) > 0, "Actual GPU preserves yarn in rounded target bounding-box corner instead of rectangular erasure");
                for (int sample = 1; sample < 12; ++sample) {
                    const float angle = static_cast<float>(sample) * 1.57079632679f / 12.f;
                    const int x = static_cast<int>(std::floor(inset + r - (r + 1.5f) * std::cos(angle)));
                    const int y = static_cast<int>(std::floor(inset + r - (r + 1.5f) * std::sin(angle)));
                    Check(alpha(x, y) > 0, "Actual GPU yarn arc stays continuous outside the rounded content cutout");
                }
            } else {
                Check(alpha(inset + static_cast<int>(std::ceil(scale)) + 1,
                    inset + static_cast<int>(std::ceil(scale)) + 1) == 0,
                    "Explicit square target keeps transparent interior beyond source tuck");
            }
        }
    }
    {
        rendering::KnitFrameSpec tiny;
        tiny.widthDip = 12; tiny.heightDip = 8; tiny.bandDip = 16;
        tiny.radiusDip = 1000; tiny.deviceScale = 1.25f;
        tiny.contentWidthPixels = 15; tiny.contentHeightPixels = 10;
        knitting::TileCache tinyCache;
        const auto tinyPerimeter = std::make_shared<const rendering::KnitPerimeter>(
            rendering::KnitRaster::RenderPerimeter(tiny, {}, tinyCache));
        const auto tinyInset = static_cast<LONG>(tinyPerimeter->contentInsetPixels);
        const RECT tinyContent{tinyInset, tinyInset, tinyInset + 15, tinyInset + 10};
        const RECT tinyBounds{0, 0, static_cast<LONG>(tinyPerimeter->width), static_cast<LONG>(tinyPerimeter->height)};
        std::array<knitting::RasterImage, 4> oversized, capped;
        Check(SUCCEEDED(overlay.Upload(tinyPerimeter, tinyBounds, tinyBounds, tinyContent, 1250, 1.25f, &oversized)) &&
            SUCCEEDED(overlay.Upload(tinyPerimeter, tinyBounds, tinyBounds, tinyContent, 5, 1.25f, &capped)),
            "Actual tiny-content GPU upload accepts bounded rounded-hole geometry");
        for (std::size_t side = 0; side < capped.size(); ++side)
            Check(oversized[side].pixels == capped[side].pixels,
                "Actual GPU radius clamps to half content extent while retaining source tuck");
    }
    std::puts("PASS: actual DComp GPU readback validates rounded/square/round-small masks at 1x/1.25x/1.5x/2x, transparent client interior and continuous corner yarn");
    Check(SUCCEEDED(queue->Stop()), "Stop actual worker before destroying receiver/device");
}
}

int RunHostContractTests() {
    GeometryAndRecovery();
    RecursiveValidationCannotOverridePolicy();
    StaleBatchPayloadUsesCurrentSnapshot();
    RenderPublicationTokens();
    NativePerimeterPublication();
    RunHostConsumerContracts();
    std::puts("PASS: host action/geometry contract assertions (scripted; no desktop windows launched)");
    std::puts("PASS: recursive-validation and stale-batch snapshot regressions (scripted)");
    std::puts("PASS: immutable MTA stockinette preparation -> four-surface native DComp upload, clip/content exclusion and move reuse (hidden; desktop presentation not tested)");
    return 0;
}

} // namespace window_sweaters::host
