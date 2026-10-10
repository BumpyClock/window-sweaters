// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace window_sweaters::stacking_fixture {

enum class MenuCaptureStatus { Inactive, AwaitingVisibleMenu, Capturing, Captured, NotCaptured };

struct MenuCapture {
    MenuCaptureStatus status{};

    constexpr void Begin() noexcept { status = MenuCaptureStatus::AwaitingVisibleMenu; }
    constexpr bool ObserveIdle(bool menuIdle, bool visibleOwnedMenu) noexcept {
        if (status != MenuCaptureStatus::AwaitingVisibleMenu || !menuIdle || !visibleOwnedMenu) return false;
        status = MenuCaptureStatus::Capturing;
        return true;
    }
    constexpr void Complete(bool measuredVisiblePixel) noexcept {
        if (status == MenuCaptureStatus::Capturing)
            status = measuredVisiblePixel ? MenuCaptureStatus::Captured : MenuCaptureStatus::NotCaptured;
    }
    constexpr void End() noexcept {
        if (status == MenuCaptureStatus::AwaitingVisibleMenu || status == MenuCaptureStatus::Capturing)
            status = MenuCaptureStatus::NotCaptured;
    }
};

}
