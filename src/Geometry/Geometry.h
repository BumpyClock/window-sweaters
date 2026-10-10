// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <variant>

namespace window_sweaters::geometry {

// Half-open physical desktop pixels; DWM bounds are never DPI-scaled again.
struct PhysicalRect {
    std::int32_t left{}, top{}, right{}, bottom{};
    constexpr std::int64_t Width() const noexcept {
        return static_cast<std::int64_t>(right) - left;
    }
    constexpr std::int64_t Height() const noexcept {
        return static_cast<std::int64_t>(bottom) - top;
    }
    bool operator==(const PhysicalRect&) const = default;
};

struct DipStyle {
    double widthDip{10.0};
    double radiusDip{8.0};
};

enum class CornerPreference : std::uint8_t {
    Unsupported, Default, Square, Round, RoundSmall
};

// Eligibility for a policy, not discovery of the actual window outline.
enum class CornerEligibility : std::uint8_t {
    Unverified, StandardFrame, HintOnly, UnsupportedShape
};

struct WindowFacts {
    PhysicalRect physicalTargetBounds;
    PhysicalRect physicalDisplayBounds;
    PhysicalRect physicalWorkArea;
    std::uint32_t dpi{96};
    std::uintptr_t monitorIdentity{};
    bool maximized{};
    CornerPreference cornerPreference{CornerPreference::Unsupported};
    CornerEligibility cornerEligibility{CornerEligibility::Unverified};
};

enum class QueryStatus : std::uint8_t {
    InvalidWindow, CallerNotPerMonitorV2, TargetNotPerMonitorAware,
    FrameBoundsFailed, DpiFailed, MonitorUnavailable, MonitorInfoFailed,
    PlacementFailed, CornerPreferenceFailed, InvalidNativeData, ChangedDuringQuery,
    StyleFailed, RegionFailed, NonClientRenderingFailed
};

struct QueryFailure {
    QueryStatus status;
    // Raw HRESULT for DWM errors; raw GetLastError for Win32 errors; zero if absent.
    std::int64_t nativeError{};
    bool operator==(const QueryFailure&) const = default;
};

using WindowQuery = std::variant<WindowFacts, QueryFailure>;

enum class ClipPolicy : std::uint8_t { WorkArea, Display };

struct GeometryLimits {
    std::uint32_t maxDimensionPixels{32768};
    std::uint64_t maxPerimeterBytes{64ull * 1024 * 1024};
    ClipPolicy clipPolicy{ClipPolicy::WorkArea};
};

enum class CornerDecision : std::uint8_t {
    ConservativeFallback, ExplicitSquare, WorkAreaAligned, RoundHint, SmallRoundHint,
    DefaultRoundPolicy
};

enum class HiddenReason : std::uint8_t {
    Maximized, Fullscreen, OutsideClip, NoExteriorSpace
};

enum class GeometryFailure : std::uint8_t {
    InvalidTargetBounds, InvalidDisplayBounds, InvalidWorkArea, InvalidDpi,
    InvalidStyle, InvalidLimits, InvalidCornerPreference, CoordinateOverflow,
    DimensionLimit, AllocationLimit, InvalidCornerEligibility
};

struct RenderInput {
    const PhysicalRect physicalTargetBounds;
    const PhysicalRect physicalOuterBounds;
    const PhysicalRect clipRelativeToOuter;
    // Subtract the content shape using radiusDip * deviceScale, not a square
    // rectangle for rounded targets; retain the exterior corner/tuck below content.
    const PhysicalRect targetRelativeToOuter;
    const double widthDip;
    // Target/content radius; rounded outer radius is radiusDip + widthDip.
    const double radiusDip;
    const double deviceScale;
    // Fractional leading gutter: render the continuous ring at this DIP offset.
    const double frameOriginDip;
    const std::uint32_t physicalPaddingPixels;
    const std::uint32_t dpi;
    const std::uintptr_t monitorIdentity;
    const bool squareCorners;
    const CornerDecision cornerDecision;
    // Upper bound for four retained BGRA perimeter pieces, not a full-window image.
    const std::uint64_t perimeterBytesUpperBound;
};

using GeometryResult = std::variant<RenderInput, HiddenReason, GeometryFailure, QueryFailure>;

// Pure; preserves query failures as failures, never as guessed renderable geometry.
GeometryResult BuildRenderInput(const WindowQuery& query, DipStyle style = {},
                               GeometryLimits limits = {}) noexcept;

} // namespace window_sweaters::geometry
