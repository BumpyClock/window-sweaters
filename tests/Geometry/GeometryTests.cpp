// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Geometry\Geometry.h"
#include "..\..\src\Geometry\NativeGeometry.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>

using namespace window_sweaters::geometry;

namespace {
int checks{};

void Check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void Near(double value, double expected, std::string_view message) {
    Check(std::abs(value - expected) < 1e-9, message);
}

template<class T>
const T& Result(const GeometryResult& result, std::string_view message) {
    Check(std::holds_alternative<T>(result), message);
    return std::get<T>(result);
}

void Failure(WindowQuery query, GeometryFailure expected, DipStyle style = {},
             GeometryLimits limits = {}) {
    const auto result = BuildRenderInput(query, style, limits);
    Check(Result<GeometryFailure>(result, "invalid input has no render geometry") == expected,
          "failure identifies the invalid input");
}

void Hidden(WindowQuery query, HiddenReason expected, GeometryLimits limits = {}) {
    const auto result = BuildRenderInput(query, {}, limits);
    Check(Result<HiddenReason>(result, "hidden input has no render geometry") == expected,
          "hidden reason follows policy");
}

WindowFacts Ordinary() {
    return {{100, 100, 740, 500}, {0, 0, 1920, 1080}, {0, 0, 1920, 1040},
            96, 1, false, CornerPreference::Round};
}

void DpiTransforms() {
    struct Case {
        std::uint32_t dpi;
        double scale;
        std::uint32_t padding;
        PhysicalRect outer;
    };
    constexpr std::array cases{
        Case{96, 1.0, 10, {90, 90, 750, 510}},
        Case{120, 1.25, 13, {87, 87, 753, 513}},
        Case{144, 1.5, 15, {85, 85, 755, 515}},
        Case{192, 2.0, 20, {80, 80, 760, 520}}
    };
    for (const auto& item : cases) {
        auto facts = Ordinary();
        facts.dpi = item.dpi;
        const auto result = BuildRenderInput(facts);
        const auto& render = Result<RenderInput>(result, "each DPI produces geometry");
        Check(render.physicalTargetBounds == PhysicalRect{100, 100, 740, 500},
              "DWM physical target stays unchanged across DPI");
        Check(render.physicalOuterBounds == item.outer, "DPI scales only exterior style");
        Check(render.physicalPaddingPixels == item.padding, "padding rounds outwards");
        Near(render.deviceScale, item.scale, "renderer receives actual scale");
        Near(render.widthDip, 10, "width remains DIP");
        Near(render.radiusDip, 8, "radius remains DIP");
        Check(render.clipRelativeToOuter == PhysicalRect{0, 0,
              static_cast<std::int32_t>(640 + item.padding * 2),
              static_cast<std::int32_t>(400 + item.padding * 2)}, "unclipped outer uses local pixels");
        Check(render.targetRelativeToOuter.left == static_cast<std::int32_t>(item.padding),
              "content exclusion follows integer padding");
        Near(render.frameOriginDip, item.dpi == 120 ? 0.4 : 0.0, "fractional gutter does not shift target");
    }
}

void MixedMonitorTransfers() {
    const WindowFacts left{{-1800, -300, -1160, 100}, {-1920, -600, 0, 480},
                          {-1920, -560, 0, 480}, 120, 17, false, CornerPreference::Round};
    const WindowFacts right{{100, 100, 740, 500}, {0, 0, 2560, 1440},
                           {0, 0, 2560, 1400}, 192, 29, false, CornerPreference::Round};
    const auto leftResult = BuildRenderInput(left);
    const auto rightResult = BuildRenderInput(right);
    const auto backResult = BuildRenderInput(left);
    const auto& a = Result<RenderInput>(leftResult, "negative-origin monitor produces geometry");
    const auto& b = Result<RenderInput>(rightResult, "transfer to high DPI produces geometry");
    const auto& c = Result<RenderInput>(backResult, "transfer back produces geometry");
    Check(a.physicalOuterBounds == PhysicalRect{-1813, -313, -1147, 113},
          "negative origins are physical signed coordinates");
    Check(b.physicalOuterBounds == PhysicalRect{80, 80, 760, 520}, "high-DPI transfer uses new scale");
    Check(c.physicalOuterBounds == PhysicalRect{-1813, -313, -1147, 113},
          "reverse transfer does not retain stale scale");
    Check(a.monitorIdentity == 17 && b.monitorIdentity == 29 && c.monitorIdentity == 17,
          "snapshot records monitor transfer both directions");
    Near(a.deviceScale, 1.25, "left monitor scale");
    Near(b.deviceScale, 2, "right monitor scale");
    Near(c.deviceScale, 1.25, "reverse monitor scale");
    Check(a.physicalTargetBounds == left.physicalTargetBounds
          && b.physicalTargetBounds == right.physicalTargetBounds, "transfer never resizes target");

    // Repeated transfers are pure snapshots, not a target registry or generation cache.
    for (int i = 0; i < 40; ++i) {
        const auto result = BuildRenderInput(i % 2 ? left : right);
        const auto& render = Result<RenderInput>(result, "rapid transfer still produces current snapshot");
        Near(render.deviceScale, i % 2 ? 1.25 : 2.0, "no accumulated or doubled scaling");
    }
}

void FractionalWidth() {
    auto facts = Ordinary();
    facts.dpi = 120;
    const auto result = BuildRenderInput(facts, {7.5, 6.25});
    const auto& render = Result<RenderInput>(result, "fractional DIP width is accepted");
    Check(render.physicalOuterBounds == PhysicalRect{90, 90, 750, 510},
          "9.375px exterior width uses ten-pixel envelope");
    Check(render.targetRelativeToOuter == PhysicalRect{10, 10, 650, 410},
          "fractional band keeps exact physical content bounds");
    Near(render.widthDip * render.deviceScale, 9.375, "band is not rounded in style space");
    Near(render.frameOriginDip, 0.5, "fractional leading gutter is half a DIP");
    Near(render.frameOriginDip * render.deviceScale + render.widthDip * render.deviceScale,
         10.0, "continuous ring reaches exact target edge");
    Near(render.radiusDip, 6.25, "fractional radius remains in DIP");

    facts.dpi = 192;
    const auto two = BuildRenderInput(facts, {2.25, 0});
    const auto& small = Result<RenderInput>(two, "narrow fractional width at 200 percent");
    Check(small.physicalOuterBounds == PhysicalRect{95, 95, 745, 505}, "4.5px band has five-pixel envelope");
    Near(small.frameOriginDip, 0.25, "200 percent gutter");
    Check(small.squareCorners && small.radiusDip == 0, "zero requested radius is an actual square decision");
}

void EdgeClippingAndSnapFallback() {
    struct Case {
        PhysicalRect target;
        PhysicalRect outer;
        PhysicalRect clip;
    };
    constexpr std::array cases{
        Case{{0, 100, 640, 500}, {-10, 90, 650, 510}, {10, 0, 660, 420}},
        Case{{100, 0, 740, 400}, {90, -10, 750, 410}, {0, 10, 660, 420}},
        Case{{1280, 100, 1920, 500}, {1270, 90, 1930, 510}, {0, 0, 650, 420}},
        Case{{100, 640, 740, 1040}, {90, 630, 750, 1050}, {0, 0, 660, 410}}
    };
    for (const auto& item : cases) {
        auto facts = Ordinary();
        facts.physicalTargetBounds = item.target;
        const auto result = BuildRenderInput(facts);
        const auto& render = Result<RenderInput>(result, "edge-aligned window retains available perimeter");
        Check(render.physicalOuterBounds == item.outer, "edge alignment never adds gaps or changes target");
        Check(render.clipRelativeToOuter == item.clip, "each monitor/work-area edge clips locally");
        Check(render.squareCorners && render.radiusDip == 0
              && render.cornerDecision == CornerDecision::WorkAreaAligned,
              "Snap fallback overrides even a public round hint");
    }
    auto facts = Ordinary();
    facts.physicalTargetBounds = {1, 100, 641, 500};
    const auto result = BuildRenderInput(facts);
    Check(Result<RenderInput>(result, "one-pixel alignment produces geometry").squareCorners,
          "one-pixel DWM tolerance chooses square corners");
    facts.physicalTargetBounds = {2, 100, 642, 500};
    const auto insetResult = BuildRenderInput(facts);
    Check(!Result<RenderInput>(insetResult, "two-pixel inset produces geometry").squareCorners,
          "fallback is bounded, not broad snap-state guessing");

    facts.physicalTargetBounds = {100, 640, 740, 1040};
    const auto displayResult = BuildRenderInput(facts, {}, {32768, 64ull * 1024 * 1024, ClipPolicy::Display});
    Check(Result<RenderInput>(displayResult, "explicit display clipping produces geometry").clipRelativeToOuter
          == PhysicalRect{0, 0, 660, 420}, "display clipping differs from work-area default");
    facts.physicalTargetBounds = {100, 1040, 740, 1080};
    const auto crossing = BuildRenderInput(facts);
    Check(Result<RenderInput>(crossing, "work-area crossing has only available top exterior").clipRelativeToOuter
          == PhysicalRect{0, 0, 660, 10}, "taskbar pixels excluded");

    facts = {{-1920, -600, -1280, -200}, {-1920, -600, 0, 480},
             {-1920, -600, 0, 440}, 120, 17, false, CornerPreference::Round};
    const auto negative = BuildRenderInput(facts);
    const auto& clipped = Result<RenderInput>(negative, "negative monitor edges clip");
    Check(clipped.physicalOuterBounds == PhysicalRect{-1933, -613, -1267, -187},
          "negative outer geometry is not clamped to zero");
    Check(clipped.clipRelativeToOuter == PhysicalRect{13, 13, 666, 426},
          "negative monitor corner uses outer-relative clip");

    facts = Ordinary();
    facts.physicalTargetBounds = {1800, 100, 2100, 500};
    const auto straddle = BuildRenderInput(facts);
    Check(Result<RenderInput>(straddle, "straddling window uses selected-monitor clip").clipRelativeToOuter
          == PhysicalRect{0, 0, 130, 420}, "no imaginary second-monitor exterior");
}

void CapabilityPolicyAndHiddenStates() {
    auto facts = Ordinary();
    for (const auto preference : {CornerPreference::Unsupported, CornerPreference::Default,
                                  CornerPreference::Square}) {
        facts.cornerPreference = preference;
        const auto result = BuildRenderInput(facts);
        const auto& render = Result<RenderInput>(result, "conservative capability fallback produces geometry");
        Check(render.squareCorners && render.radiusDip == 0, "unknown/default/no-round shapes stay square");
        Check(render.cornerDecision == (preference == CornerPreference::Square
              ? CornerDecision::ExplicitSquare : CornerDecision::ConservativeFallback),
              "fallback is explicit and distinguishable");
    }
    facts.cornerPreference = CornerPreference::RoundSmall;
    const auto small = BuildRenderInput(facts);
    Near(Result<RenderInput>(small, "small round preference produces geometry").radiusDip, 4.0,
         "small round hint uses policy cap, not claimed measured radius");
    facts.cornerPreference = CornerPreference::Round;
    const auto round = BuildRenderInput(facts, {10, 1000});
    Near(Result<RenderInput>(round, "large requested radius is constrained").radiusDip, 8.0,
         "public round hint has conservative eight-DIP cap");
    facts.maximized = true;
    Hidden(facts, HiddenReason::Maximized);
    facts.maximized = false;
    facts.physicalTargetBounds = {0, 0, 1920, 1080};
    Hidden(facts, HiddenReason::Fullscreen);
    facts.physicalTargetBounds = {-10, -10, 1930, 1090};
    Hidden(facts, HiddenReason::Fullscreen);
    facts.physicalTargetBounds = {0, 0, 1920, 1040};
    Hidden(facts, HiddenReason::NoExteriorSpace);
    facts.physicalTargetBounds = {2500, 100, 3140, 500};
    Hidden(facts, HiddenReason::OutsideClip);
    facts.physicalTargetBounds = {100, 1090, 740, 1490};
    Hidden(facts, HiddenReason::OutsideClip, {32768, 64ull * 1024 * 1024, ClipPolicy::Display});
}

void TinyAndBoundedGeometry() {
    auto facts = Ordinary();
    facts.physicalTargetBounds = {100, 100, 101, 101};
    const auto result = BuildRenderInput(facts, {2.25, 8});
    const auto& render = Result<RenderInput>(result, "one-pixel window has bounded exterior");
    Check(render.physicalOuterBounds == PhysicalRect{97, 97, 104, 104}, "tiny target envelope");
    Check(render.targetRelativeToOuter == PhysicalRect{3, 3, 4, 4}, "tiny target is excluded, never filled");
    Near(render.radiusDip, 0.5, "radius clamps to half physical target size converted to DIP");
    Check(render.perimeterBytesUpperBound == 672, "tiny four-surface byte ceiling is bounded");

    facts = Ordinary();
    const auto defaultResult = BuildRenderInput(facts);
    const auto& normal = Result<RenderInput>(defaultResult, "ordinary geometry has byte ceiling");
    Check(normal.perimeterBytesUpperBound == 181440, "budget counts four-byte BGRA surfaces, not just pixels");
    Failure(facts, GeometryFailure::AllocationLimit, {}, {32768, 181439});
    const auto exact = BuildRenderInput(facts, {}, {32768, 181440});
    Check(std::holds_alternative<RenderInput>(exact), "exact perimeter-byte limit is accepted");
    Failure(facts, GeometryFailure::DimensionLimit, {}, {659, 64ull * 1024 * 1024});
    const auto dimension = BuildRenderInput(facts, {}, {660, 64ull * 1024 * 1024});
    Check(std::holds_alternative<RenderInput>(dimension), "exact outer maximum dimension accepted");

    facts = {{100, 100, 32849, 200}, {0, 0, 40000, 1080},
             {0, 0, 40000, 1040}, 96, 1, false, CornerPreference::Round};
    Failure(facts, GeometryFailure::DimensionLimit);
    facts.physicalTargetBounds.right = 32848;
    const auto large = BuildRenderInput(facts);
    Check(Result<RenderInput>(large, "32768px outer width remains perimeter-bounded").physicalOuterBounds.Width()
          == 32768, "maximum dimension checked after exterior expansion");
    facts.physicalTargetBounds = {100, 100, 28000, 200};
    facts.dpi = 768;
    Failure(facts, GeometryFailure::AllocationLimit, {256, 8});

    facts = Ordinary();
    facts.physicalTargetBounds.left = std::numeric_limits<std::int32_t>::min();
    facts.physicalTargetBounds.right = facts.physicalTargetBounds.left + 1;
    Failure(facts, GeometryFailure::CoordinateOverflow);
    facts = Ordinary();
    facts.physicalTargetBounds.right = std::numeric_limits<std::int32_t>::max();
    facts.physicalTargetBounds.left = facts.physicalTargetBounds.right - 1;
    Failure(facts, GeometryFailure::CoordinateOverflow);
    facts = Ordinary();
    facts.physicalTargetBounds.top = std::numeric_limits<std::int32_t>::min();
    facts.physicalTargetBounds.bottom = facts.physicalTargetBounds.top + 1;
    Failure(facts, GeometryFailure::CoordinateOverflow);
    facts = Ordinary();
    facts.physicalTargetBounds.bottom = std::numeric_limits<std::int32_t>::max();
    facts.physicalTargetBounds.top = facts.physicalTargetBounds.bottom - 1;
    Failure(facts, GeometryFailure::CoordinateOverflow);
    facts = Ordinary();
    facts.physicalTargetBounds = {std::numeric_limits<std::int32_t>::min() + 10, 100,
                                 std::numeric_limits<std::int32_t>::max() - 10, 500};
    Failure(facts, GeometryFailure::DimensionLimit);
}

void InvalidInputsAndQueryUncertainty() {
    auto facts = Ordinary();
    facts.physicalTargetBounds.right = facts.physicalTargetBounds.left;
    Failure(facts, GeometryFailure::InvalidTargetBounds);
    facts.physicalTargetBounds.right = facts.physicalTargetBounds.left - 1;
    Failure(facts, GeometryFailure::InvalidTargetBounds);
    facts = Ordinary();
    facts.physicalDisplayBounds.bottom = 0;
    Failure(facts, GeometryFailure::InvalidDisplayBounds);
    facts = Ordinary();
    facts.physicalWorkArea.left = -1;
    Failure(facts, GeometryFailure::InvalidWorkArea);
    facts = Ordinary();
    facts.physicalWorkArea.bottom = 0;
    Failure(facts, GeometryFailure::InvalidWorkArea);
    for (const std::uint32_t dpi : {0u, 95u, 769u, std::numeric_limits<std::uint32_t>::max()}) {
        facts = Ordinary();
        facts.dpi = dpi;
        Failure(facts, GeometryFailure::InvalidDpi);
    }
    facts = Ordinary();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    for (const double width : {nan, infinity, -infinity, -1.0, 0.0, 1.99, 256.01}) {
        Failure(facts, GeometryFailure::InvalidStyle, {width, 8});
    }
    for (const double radius : {nan, infinity, -infinity, -1.0}) {
        Failure(facts, GeometryFailure::InvalidStyle, {10, radius});
    }
    Failure(facts, GeometryFailure::InvalidLimits, {}, {0, 1});
    Failure(facts, GeometryFailure::InvalidLimits, {}, {32769, 1});
    Failure(facts, GeometryFailure::InvalidLimits, {}, {32768, 0});
    Failure(facts, GeometryFailure::InvalidLimits, {}, {32768, 1, static_cast<ClipPolicy>(99)});
    facts.cornerPreference = static_cast<CornerPreference>(99);
    Failure(facts, GeometryFailure::InvalidCornerPreference);

    for (const auto status : {QueryStatus::MonitorUnavailable, QueryStatus::MonitorInfoFailed,
                             QueryStatus::DpiFailed, QueryStatus::CornerPreferenceFailed,
                             QueryStatus::ChangedDuringQuery, QueryStatus::InvalidWindow}) {
        const QueryFailure failure{status, 1234};
        const auto result = BuildRenderInput(failure);
        Check(Result<QueryFailure>(result, "query uncertainty is not safe success") == failure,
              "monitor removal/query error survives with diagnostic code");
    }
    const auto recovered = BuildRenderInput(Ordinary());
    Check(std::holds_alternative<RenderInput>(recovered), "fresh facts after uncertainty recover without stored state");

    const auto invalid = QueryWindowGeometry(nullptr);
    Check(std::holds_alternative<QueryFailure>(invalid), "native invalid HWND is explicit failure");
    Check(std::get<QueryFailure>(invalid).status == QueryStatus::InvalidWindow,
          "native invalid HWND status is not monitor fallback");
    const auto invalidHandle = QueryWindowGeometry(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1)));
    Check(std::holds_alternative<QueryFailure>(invalidHandle), "native non-window HWND is explicit failure");
}
} // namespace

int main() {
    static_assert(!std::is_copy_assignable_v<RenderInput>);
    static_assert(std::is_copy_constructible_v<RenderInput>);
    DpiTransforms();
    MixedMonitorTransfers();
    FractionalWidth();
    EdgeClippingAndSnapFallback();
    CapabilityPolicyAndHiddenStates();
    TinyAndBoundedGeometry();
    InvalidInputsAndQueryUncertainty();
    std::cout << "PASS: " << checks << " geometry checks (deterministic; native invalid-HWND checks only)\n";
    return EXIT_SUCCESS;
}
