#include "OverlayState.h"

#include <cstdio>
#include <stdexcept>

namespace window_sweaters::host {
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
        "Unsupported topmost band retains intent without rendering");
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
}

int RunHostContractTests() {
    GeometryAndRecovery();
    RecursiveValidationCannotOverridePolicy();
    StaleBatchPayloadUsesCurrentSnapshot();
    std::puts("PASS: host action/geometry contract assertions (scripted; no desktop windows launched)");
    std::puts("PASS: recursive-validation and stale-batch snapshot regressions (scripted)");
    return 0;
}

} // namespace window_sweaters::host
