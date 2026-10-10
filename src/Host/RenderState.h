#pragma once

#include "..\Geometry\Geometry.h"
#include "..\Rendering\RenderQueue.h"

namespace window_sweaters::host {

inline tracking::PhysicalRect TrackedRect(const geometry::PhysicalRect& rect) noexcept {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

inline rendering::RenderRequest MakeRenderRequest(const tracking::WorkToken& token,
    std::uint64_t revision, std::uint64_t generation, const geometry::RenderInput& input,
    knitting::TileSpec appearance = {}) {
    rendering::RenderRequest request;
    request.token = token;
    request.configRevision = revision;
    request.deviceGeneration = generation;
    request.targetBounds = TrackedRect(input.physicalTargetBounds);
    request.clipRelativeToOuter = TrackedRect(input.clipRelativeToOuter);
    request.frame.contentWidthPixels = static_cast<std::uint32_t>(input.physicalTargetBounds.Width());
    request.frame.contentHeightPixels = static_cast<std::uint32_t>(input.physicalTargetBounds.Height());
    request.frame.widthDip = static_cast<float>(input.physicalTargetBounds.Width() / input.deviceScale);
    request.frame.heightDip = static_cast<float>(input.physicalTargetBounds.Height() / input.deviceScale);
    request.frame.physicalPaddingPixels = input.physicalPaddingPixels;
    request.frame.bandDip = static_cast<float>(input.widthDip);
    request.frame.radiusDip = static_cast<float>(input.radiusDip);
    request.frame.deviceScale = static_cast<float>(input.deviceScale);
    request.tile = std::move(appearance);
    request.tile.bandDip = request.frame.bandDip;
    request.tile.deviceScale = request.frame.deviceScale;
    return request;
}

// Placement and clip are deliberately absent. A moved target can attach its
// prepared pixels at today's position without regenerating the source tile.
inline bool MatchesRenderInput(const rendering::RenderRequest& request,
    const tracking::WorkToken& token, std::uint64_t revision, std::uint64_t generation,
    const geometry::RenderInput& input) noexcept {
    return request.token == token && request.configRevision == revision &&
        request.deviceGeneration == generation &&
        request.frame.contentWidthPixels == input.physicalTargetBounds.Width() &&
        request.frame.contentHeightPixels == input.physicalTargetBounds.Height() &&
        request.frame.physicalPaddingPixels == input.physicalPaddingPixels &&
        request.frame.bandDip == static_cast<float>(input.widthDip) &&
        request.frame.radiusDip == static_cast<float>(input.radiusDip) &&
        request.frame.deviceScale == static_cast<float>(input.deviceScale);
}

inline bool SamePreparation(const rendering::RenderRequest& left,
    const rendering::RenderRequest& right) noexcept {
    return left.token == right.token && left.configRevision == right.configRevision &&
        left.deviceGeneration == right.deviceGeneration &&
        left.frame.contentWidthPixels == right.frame.contentWidthPixels &&
        left.frame.contentHeightPixels == right.frame.contentHeightPixels &&
        left.frame.physicalPaddingPixels == right.frame.physicalPaddingPixels &&
        left.frame.bandDip == right.frame.bandDip && left.frame.radiusDip == right.frame.radiusDip &&
        left.frame.deviceScale == right.frame.deviceScale;
}

struct RenderState {
    std::optional<rendering::RenderRequest> pending;
    std::optional<rendering::RenderRequest> prepared;
    std::optional<rendering::RenderRequest> failed;
    std::optional<rendering::RenderRequest> retry;
    bool retryQueued{};
    void Invalidate() noexcept {
        pending.reset(); prepared.reset(); failed.reset(); retry.reset(); retryQueued = false;
    }
};

} // namespace window_sweaters::host
