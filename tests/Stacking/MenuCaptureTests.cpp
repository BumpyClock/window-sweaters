// SPDX-License-Identifier: GPL-3.0-only
#include "MenuCapture.h"

using namespace window_sweaters::stacking_fixture;

namespace {
constexpr bool VisibleMenuIsCapturedOnce() {
    MenuCapture capture;
    if (capture.ObserveIdle(true, true)) return false;
    capture.Begin();
    if (capture.ObserveIdle(true, false) || capture.ObserveIdle(false, true)) return false;
    if (!capture.ObserveIdle(true, true) || capture.ObserveIdle(true, true)) return false;
    capture.Complete(true);
    capture.End();
    return capture.status == MenuCaptureStatus::Captured && !capture.ObserveIdle(true, true);
}

constexpr bool DismissedMenuCannotBecomeEvidence() {
    MenuCapture capture;
    capture.Begin();
    capture.End();
    capture.Complete(true);
    return capture.status == MenuCaptureStatus::NotCaptured && !capture.ObserveIdle(true, true);
}

constexpr bool InvalidPixelDoesNotClaimCapture() {
    MenuCapture capture;
    capture.Begin();
    if (!capture.ObserveIdle(true, true)) return false;
    capture.Complete(false);
    capture.End();
    if (capture.status != MenuCaptureStatus::NotCaptured || capture.ObserveIdle(true, true)) return false;
    capture.Begin();
    return capture.ObserveIdle(true, true);
}

static_assert(VisibleMenuIsCapturedOnce(), "Capture only a visible menu during its active loop, once.");
static_assert(DismissedMenuCannotBecomeEvidence(), "An unobserved or dismissed menu is not capture evidence.");
static_assert(InvalidPixelDoesNotClaimCapture(), "A failed measurement is not evidence; a new menu resets the gate.");
}
