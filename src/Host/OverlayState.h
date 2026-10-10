#pragma once

#include "..\Tracking\TrackingModel.h"

namespace window_sweaters::host {

struct OverlayState {
    explicit OverlayState(tracking::TargetId id) noexcept : target(id) {}
    const tracking::TargetId target;
    tracking::PhysicalRect pendingRect{};
    std::optional<tracking::PhysicalRect> appliedRect;
    std::uint32_t dpi{};
    std::uint64_t appearanceRevision{};
    bool requestedVisible{};
    bool topmost{};
    bool excluded{};

    bool Matches(const tracking::TargetId& id) const noexcept { return target == id; }

    bool Synchronize(const tracking::TrackingModel& model, const tracking::WorkToken& token) noexcept {
        if (!Matches(token.target)) return false;
        const auto snapshot = model.CurrentSnapshot(token);
        if (!snapshot) return false;
        pendingRect = snapshot->rect;
        dpi = snapshot->dpi;
        appearanceRevision = snapshot->appearanceRevision;
        topmost = snapshot->topmost;
        requestedVisible = snapshot->visible;
        return true;
    }

    bool ApplyValidated(const tracking::TrackingModel& model, const tracking::WorkToken& token,
                        bool queryAccepted) noexcept {
        return queryAccepted && model.Accepts(token) && Synchronize(model, token);
    }

    bool NeedsDraw() const noexcept {
        return !appliedRect || appliedRect->Width() != pendingRect.Width()
            || appliedRect->Height() != pendingRect.Height();
    }

    std::optional<tracking::PhysicalRect> GeometryUpdate(bool settled) const noexcept {
        if (excluded || (!settled && NeedsDraw())
            || (appliedRect && *appliedRect == pendingRect)) return std::nullopt;
        return pendingRect;
    }
};

int RunHostContractTests();

}
