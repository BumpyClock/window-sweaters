// SPDX-License-Identifier: GPL-3.0-only
#include "Geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace window_sweaters::geometry {
namespace {
constexpr bool Valid(const PhysicalRect& r) noexcept {
    return r.Width() > 0 && r.Height() > 0;
}

constexpr bool Contains(const PhysicalRect& outer, const PhysicalRect& inner) noexcept {
    return outer.left <= inner.left && outer.top <= inner.top
        && outer.right >= inner.right && outer.bottom >= inner.bottom;
}

constexpr PhysicalRect Intersect(const PhysicalRect& a, const PhysicalRect& b) noexcept {
    return {std::max(a.left, b.left), std::max(a.top, b.top),
            std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

bool AlignedWithWorkArea(const PhysicalRect& target, const PhysicalRect& work) noexcept {
    // One physical pixel allows DWM edge rounding. This is not proof of Snap.
    return std::abs(static_cast<std::int64_t>(target.left) - work.left) <= 1
        || std::abs(static_cast<std::int64_t>(target.top) - work.top) <= 1
        || std::abs(static_cast<std::int64_t>(target.right) - work.right) <= 1
        || std::abs(static_cast<std::int64_t>(target.bottom) - work.bottom) <= 1;
}

PhysicalRect RelativeTo(const PhysicalRect& rect, const PhysicalRect& outer) noexcept {
    return {static_cast<std::int32_t>(static_cast<std::int64_t>(rect.left) - outer.left),
            static_cast<std::int32_t>(static_cast<std::int64_t>(rect.top) - outer.top),
            static_cast<std::int32_t>(static_cast<std::int64_t>(rect.right) - outer.left),
            static_cast<std::int32_t>(static_cast<std::int64_t>(rect.bottom) - outer.top)};
}
}

GeometryResult BuildRenderInput(const WindowQuery& query, DipStyle style,
                               GeometryLimits limits) noexcept {
    if (const auto failure = std::get_if<QueryFailure>(&query)) return *failure;
    const auto& facts = std::get<WindowFacts>(query);
    const auto& target = facts.physicalTargetBounds;
    const auto& display = facts.physicalDisplayBounds;
    const auto& work = facts.physicalWorkArea;
    if (!Valid(target)) return GeometryFailure::InvalidTargetBounds;
    if (!Valid(display)) return GeometryFailure::InvalidDisplayBounds;
    if (!Valid(work) || !Contains(display, work)) return GeometryFailure::InvalidWorkArea;
    if (facts.dpi < 96 || facts.dpi > 768) return GeometryFailure::InvalidDpi;
    if (!std::isfinite(style.widthDip) || !std::isfinite(style.radiusDip)
        || style.widthDip < 2.0 || style.widthDip > 256.0 || style.radiusDip < 0.0)
        return GeometryFailure::InvalidStyle;
    if (!limits.maxDimensionPixels || limits.maxDimensionPixels > 32768
        || !limits.maxPerimeterBytes
        || (limits.clipPolicy != ClipPolicy::WorkArea && limits.clipPolicy != ClipPolicy::Display))
        return GeometryFailure::InvalidLimits;
    switch (facts.cornerEligibility) {
    case CornerEligibility::Unverified:
    case CornerEligibility::StandardFrame:
    case CornerEligibility::HintOnly:
    case CornerEligibility::UnsupportedShape:
        break;
    default:
        return GeometryFailure::InvalidCornerEligibility;
    }

    CornerDecision cornerDecision{};
    bool square{};
    double radius = style.radiusDip;
    switch (facts.cornerPreference) {
    case CornerPreference::Unsupported:
        square = true;
        cornerDecision = CornerDecision::ConservativeFallback;
        break;
    case CornerPreference::Default:
        square = facts.cornerEligibility != CornerEligibility::StandardFrame;
        radius = std::min(radius, 8.0);
        cornerDecision = square ? CornerDecision::ConservativeFallback
                                : CornerDecision::DefaultRoundPolicy;
        break;
    case CornerPreference::Square:
        square = true;
        cornerDecision = CornerDecision::ExplicitSquare;
        break;
    case CornerPreference::Round:
        radius = std::min(radius, 8.0);
        cornerDecision = CornerDecision::RoundHint;
        break;
    case CornerPreference::RoundSmall:
        radius = std::min(radius, 4.0);
        cornerDecision = CornerDecision::SmallRoundHint;
        break;
    default:
        return GeometryFailure::InvalidCornerPreference;
    }
    if (facts.cornerPreference != CornerPreference::Square
        && (facts.cornerEligibility == CornerEligibility::Unverified
            || facts.cornerEligibility == CornerEligibility::UnsupportedShape)) {
        square = true;
        cornerDecision = CornerDecision::ConservativeFallback;
    }
    if (facts.maximized) return HiddenReason::Maximized;
    if (Contains(target, display)) return HiddenReason::Fullscreen;
    if (AlignedWithWorkArea(target, work)) {
        square = true;
        cornerDecision = CornerDecision::WorkAreaAligned;
    }

    const double scale = static_cast<double>(facts.dpi) / 96.0;
    radius = square ? 0.0 : std::min(radius,
        static_cast<double>(std::min(target.Width(), target.Height())) / (2.0 * scale));
    square = square || radius == 0.0;
    const auto padding = static_cast<std::int64_t>(std::ceil(style.widthDip * scale));
    const auto margin = static_cast<std::int64_t>(ShadowMarginPixels(static_cast<std::uint32_t>(padding)));
    const auto inset = padding + margin;
    const std::int64_t left = static_cast<std::int64_t>(target.left) - inset;
    const std::int64_t top = static_cast<std::int64_t>(target.top) - inset;
    const std::int64_t right = static_cast<std::int64_t>(target.right) + inset;
    const std::int64_t bottom = static_cast<std::int64_t>(target.bottom) + inset;
    constexpr auto minCoordinate = std::numeric_limits<std::int32_t>::min();
    constexpr auto maxCoordinate = std::numeric_limits<std::int32_t>::max();
    if (left < minCoordinate || top < minCoordinate || right > maxCoordinate || bottom > maxCoordinate)
        return GeometryFailure::CoordinateOverflow;
    const auto width = static_cast<std::uint64_t>(right - left);
    const auto height = static_cast<std::uint64_t>(bottom - top);
    if (width > limits.maxDimensionPixels || height > limits.maxDimensionPixels)
        return GeometryFailure::DimensionLimit;
    const PhysicalRect outer{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right), static_cast<std::int32_t>(bottom)};
    const auto& area = limits.clipPolicy == ClipPolicy::WorkArea ? work : display;
    const PhysicalRect band{static_cast<std::int32_t>(left + margin), static_cast<std::int32_t>(top + margin),
        static_cast<std::int32_t>(right - margin), static_cast<std::int32_t>(bottom - margin)};
    const auto clip = Intersect(outer, area);
    if (!Valid(Intersect(band, area))) return HiddenReason::OutsideClip;
    if (Contains(target, clip)) return HiddenReason::NoExteriorSpace;

    // Bound every factor before multiplying, including the byte stride.
    const auto depth = static_cast<std::uint64_t>(std::ceil((style.widthDip + radius + 2.0) * scale)) + 1
        + static_cast<std::uint64_t>(margin);
    const auto bytes = 4ull * (2ull * width * std::min(depth, height)
                             + 2ull * height * std::min(depth, width));
    if (bytes > limits.maxPerimeterBytes) return GeometryFailure::AllocationLimit;
    const auto physicalPadding = static_cast<std::uint32_t>(padding);
    return RenderInput{target, outer, RelativeTo(clip, outer), RelativeTo(target, outer),
        style.widthDip, radius, scale, (static_cast<double>(inset) - style.widthDip * scale) / scale,
        physicalPadding, facts.dpi, facts.monitorIdentity, square, cornerDecision, bytes};
}

}
