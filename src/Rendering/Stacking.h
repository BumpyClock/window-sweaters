// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>

namespace window_sweaters::rendering {

enum class StackingBand { Ordinary, Topmost };
enum class StackingAction { None, PlaceBelow, PromoteThenPlaceBelow, DemoteThenPlaceBelow };

struct StackingSnapshot {
    StackingBand targetBand{};
    StackingBand overlayBand{};
    bool immediatelyBelow{};
};

constexpr StackingAction PlanStacking(StackingSnapshot snapshot) noexcept {
    if (snapshot.targetBand != snapshot.overlayBand) {
        return snapshot.targetBand == StackingBand::Topmost
            ? StackingAction::PromoteThenPlaceBelow : StackingAction::DemoteThenPlaceBelow;
    }
    return snapshot.immediatelyBelow ? StackingAction::None : StackingAction::PlaceBelow;
}

// Call on the overlay's owning thread with an unowned top-level overlay in this process.
// Target eligibility/lifetime belongs to the tracker; owned/child targets are rejected here.
// Failure means the caller must suppress decoration until a later reconciliation succeeds.
// S_OK verifies both the current ordinary/topmost band and immediate adjacency.
HRESULT PlaceOverlayBelow(HWND overlay, HWND target) noexcept;

}
