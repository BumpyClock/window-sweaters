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

HRESULT PlaceOverlayBelow(HWND overlay, HWND target) noexcept;

}
