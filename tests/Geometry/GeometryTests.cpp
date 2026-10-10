// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Geometry\Geometry.h"
#include "..\..\src\Geometry\NativeGeometry.h"

#include <dwmapi.h>

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
            96, 1, false, CornerPreference::Default, CornerEligibility::StandardFrame};
}

void DpiTransforms() {
    struct Case {
        std::uint32_t dpi;
        double scale;
        std::uint32_t padding;
        PhysicalRect outer;
    };
    constexpr std::array cases{
        Case{96, 1.0, 10, {85, 85, 755, 515}},
        Case{120, 1.25, 13, {80, 80, 760, 520}},
        Case{144, 1.5, 15, {77, 77, 763, 523}},
        Case{192, 2.0, 20, {70, 70, 770, 530}}
    };
    for (const auto& item : cases) {
        const auto inset = item.padding + ShadowMarginPixels(item.padding);
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
        Near(render.radiusDip * render.deviceScale, item.dpi == 96 ? 8.0
             : item.dpi == 120 ? 10.0 : item.dpi == 144 ? 12.0 : 16.0,
             "default round policy gives physical content-mask radius at each DPI");
        Check(!render.squareCorners && render.cornerDecision == CornerDecision::DefaultRoundPolicy,
              "ordinary default windows preserve rounded exterior corners");
        Check(render.clipRelativeToOuter == PhysicalRect{0, 0,
              static_cast<std::int32_t>(640 + inset * 2),
              static_cast<std::int32_t>(400 + inset * 2)}, "unclipped outer uses local pixels");
        Check(render.targetRelativeToOuter.left == static_cast<std::int32_t>(inset),
              "content exclusion follows integer padding and shadow margin");
        Near(render.frameOriginDip, (inset - 10 * item.scale) / item.scale,
             "fractional gutter and shadow margin do not shift target");
    }
}

void MixedMonitorTransfers() {
    const WindowFacts left{{-1800, -300, -1160, 100}, {-1920, -600, 0, 480},
                          {-1920, -560, 0, 480}, 120, 17, false, CornerPreference::Round,
                          CornerEligibility::StandardFrame};
    const WindowFacts right{{100, 100, 740, 500}, {0, 0, 2560, 1440},
                           {0, 0, 2560, 1400}, 192, 29, false, CornerPreference::Round,
                           CornerEligibility::StandardFrame};
    const auto leftResult = BuildRenderInput(left);
    const auto rightResult = BuildRenderInput(right);
    const auto backResult = BuildRenderInput(left);
    const auto& a = Result<RenderInput>(leftResult, "negative-origin monitor produces geometry");
    const auto& b = Result<RenderInput>(rightResult, "transfer to high DPI produces geometry");
    const auto& c = Result<RenderInput>(backResult, "transfer back produces geometry");
    Check(a.physicalOuterBounds == PhysicalRect{-1820, -320, -1140, 120},
          "negative origins are physical signed coordinates");
    Check(b.physicalOuterBounds == PhysicalRect{70, 70, 770, 530}, "high-DPI transfer uses new scale");
    Check(c.physicalOuterBounds == PhysicalRect{-1820, -320, -1140, 120},
          "reverse transfer does not retain stale scale");
    Check(a.monitorIdentity == 17 && b.monitorIdentity == 29 && c.monitorIdentity == 17,
          "snapshot records monitor transfer both directions");
    Near(a.deviceScale, 1.25, "left monitor scale");
    Near(b.deviceScale, 2, "right monitor scale");
    Near(c.deviceScale, 1.25, "reverse monitor scale");
    Check(a.physicalTargetBounds == left.physicalTargetBounds
          && b.physicalTargetBounds == right.physicalTargetBounds, "transfer never resizes target");

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
    Check(render.physicalOuterBounds == PhysicalRect{85, 85, 755, 515},
          "9.375px exterior width uses ten-pixel envelope plus five-pixel shadow margin");
    Check(render.physicalPaddingPixels == 10, "padding excludes the shadow margin");
    Check(render.targetRelativeToOuter == PhysicalRect{15, 15, 655, 415},
          "fractional band keeps exact physical content bounds");
    Near(render.widthDip * render.deviceScale, 9.375, "band is not rounded in style space");
    Near(render.frameOriginDip, 4.5, "leading gutter is half a DIP plus a four-DIP shadow margin");
    Near(render.frameOriginDip * render.deviceScale + render.widthDip * render.deviceScale,
         15.0, "continuous ring reaches exact target edge");
    Near(render.radiusDip, 6.25, "fractional radius remains in DIP");

    facts.dpi = 192;
    const auto two = BuildRenderInput(facts, {2.25, 0});
    const auto& narrow = Result<RenderInput>(two, "narrow fractional width at 200 percent");
    Check(narrow.physicalOuterBounds == PhysicalRect{92, 92, 748, 508},
          "4.5px band has five-pixel envelope plus three-pixel shadow margin");
    Near(narrow.frameOriginDip, 1.75, "200 percent gutter");
    Check(narrow.squareCorners && narrow.radiusDip == 0, "zero requested radius is an actual square decision");
}

void EdgeClippingAndSnapFallback() {
    struct Case {
        PhysicalRect target;
        PhysicalRect outer;
        PhysicalRect clip;
    };
    constexpr std::array cases{
        Case{{0, 100, 640, 500}, {-15, 85, 655, 515}, {15, 0, 670, 430}},
        Case{{100, 0, 740, 400}, {85, -15, 755, 415}, {0, 15, 670, 430}},
        Case{{1280, 100, 1920, 500}, {1265, 85, 1935, 515}, {0, 0, 655, 430}},
        Case{{100, 640, 740, 1040}, {85, 625, 755, 1055}, {0, 0, 670, 415}}
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
              "Snap fallback overrides the default round policy");
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
          == PhysicalRect{0, 0, 670, 430}, "display clipping differs from work-area default");
    facts.physicalTargetBounds = {100, 1040, 740, 1080};
    const auto crossing = BuildRenderInput(facts);
    Check(Result<RenderInput>(crossing, "work-area crossing has only available top exterior").clipRelativeToOuter
          == PhysicalRect{0, 0, 670, 15}, "taskbar pixels excluded");

    facts = {{-1920, -600, -1280, -200}, {-1920, -600, 0, 480},
             {-1920, -600, 0, 440}, 120, 17, false, CornerPreference::Round,
             CornerEligibility::StandardFrame};
    const auto negative = BuildRenderInput(facts);
    const auto& clipped = Result<RenderInput>(negative, "negative monitor edges clip");
    Check(clipped.physicalOuterBounds == PhysicalRect{-1940, -620, -1260, -180},
          "negative outer geometry is not clamped to zero");
    Check(clipped.clipRelativeToOuter == PhysicalRect{20, 20, 680, 440},
          "negative monitor corner uses outer-relative clip");

    facts = Ordinary();
    facts.physicalTargetBounds = {1800, 100, 2100, 500};
    const auto straddle = BuildRenderInput(facts);
    Check(Result<RenderInput>(straddle, "straddling window uses selected-monitor clip").clipRelativeToOuter
          == PhysicalRect{0, 0, 135, 430}, "no imaginary second-monitor exterior");
}

void CapabilityPolicyAndHiddenStates() {
    auto facts = Ordinary();
    for (const auto preference : {CornerPreference::Unsupported, CornerPreference::Square}) {
        facts.cornerPreference = preference;
        const auto result = BuildRenderInput(facts);
        const auto& render = Result<RenderInput>(result, "conservative capability fallback produces geometry");
        Check(render.squareCorners && render.radiusDip == 0, "unsupported/no-round shapes stay square");
        Check(render.cornerDecision == (preference == CornerPreference::Square
              ? CornerDecision::ExplicitSquare : CornerDecision::ConservativeFallback),
              "fallback is explicit and distinguishable");
    }
    facts.cornerPreference = CornerPreference::Default;
    const auto defaultResult = BuildRenderInput(facts);
    const auto& defaultRound = Result<RenderInput>(defaultResult, "ordinary default window produces geometry");
    Check(!defaultRound.squareCorners
          && defaultRound.cornerDecision == CornerDecision::DefaultRoundPolicy,
          "default differs from unavailable corner capability");
    Near(defaultRound.radiusDip, 8, "ordinary Windows 11 default uses eight-DIP radius policy");
    const auto cappedDefault = BuildRenderInput(facts, {10, 1000});
    Near(Result<RenderInput>(cappedDefault, "large default radius request produces geometry").radiusDip,
         8, "default policy caps radius independently of requested value");
    const auto narrowDefault = BuildRenderInput(facts, {10, 3.5});
    Near(Result<RenderInput>(narrowDefault, "fractional default radius request produces geometry").radiusDip,
         3.5, "default policy preserves smaller fractional requested radius");
    const auto squareDefault = BuildRenderInput(facts, {10, 0});
    Check(Result<RenderInput>(squareDefault, "zero radius default produces geometry").squareCorners,
          "zero requested radius remains square under default policy");
    facts.cornerPreference = CornerPreference::RoundSmall;
    const auto smallRound = BuildRenderInput(facts);
    Near(Result<RenderInput>(smallRound, "small round preference produces geometry").radiusDip, 4.0,
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
    Check(render.physicalOuterBounds == PhysicalRect{95, 95, 106, 106}, "tiny target envelope");
    Check(render.targetRelativeToOuter == PhysicalRect{5, 5, 6, 6}, "tiny target is excluded, never filled");
    Near(render.radiusDip, 0.5, "radius clamps to half physical target size converted to DIP");
    Check(render.perimeterBytesUpperBound == 1408, "tiny four-surface byte ceiling is bounded");

    facts = Ordinary();
    const auto defaultResult = BuildRenderInput(facts);
    const auto& normal = Result<RenderInput>(defaultResult, "ordinary geometry has byte ceiling");
    Check(normal.perimeterBytesUpperBound == 228800, "budget counts four-byte BGRA surfaces, not just pixels");
    Failure(facts, GeometryFailure::AllocationLimit, {}, {32768, 228799});
    const auto exact = BuildRenderInput(facts, {}, {32768, 228800});
    Check(std::holds_alternative<RenderInput>(exact), "exact perimeter-byte limit is accepted");
    Failure(facts, GeometryFailure::DimensionLimit, {}, {669, 64ull * 1024 * 1024});
    const auto dimension = BuildRenderInput(facts, {}, {670, 64ull * 1024 * 1024});
    Check(std::holds_alternative<RenderInput>(dimension), "exact outer maximum dimension accepted");

    facts = {{100, 100, 32839, 200}, {0, 0, 40000, 1080},
             {0, 0, 40000, 1040}, 96, 1, false, CornerPreference::Round,
             CornerEligibility::StandardFrame};
    Failure(facts, GeometryFailure::DimensionLimit);
    facts.physicalTargetBounds.right = 32838;
    const auto large = BuildRenderInput(facts);
    Check(Result<RenderInput>(large, "32768px outer width remains perimeter-bounded").physicalOuterBounds.Width()
          == 32768, "maximum dimension checked after exterior expansion");
    facts.physicalTargetBounds = {100, 100, 20000, 200};
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
    facts.physicalTargetBounds = {std::numeric_limits<std::int32_t>::min() + 15, 100,
                                 std::numeric_limits<std::int32_t>::max() - 15, 500};
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
    facts = Ordinary();
    facts.cornerEligibility = static_cast<CornerEligibility>(99);
    Failure(facts, GeometryFailure::InvalidCornerEligibility);

    for (const auto status : {QueryStatus::MonitorUnavailable, QueryStatus::MonitorInfoFailed,
                             QueryStatus::DpiFailed, QueryStatus::CornerPreferenceFailed,
                             QueryStatus::StyleFailed, QueryStatus::RegionFailed,
                             QueryStatus::NonClientRenderingFailed,
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

void CornerEligibilityAndRadiusRelationships() {
    struct PreferenceCase {
        CornerPreference preference;
        double radius;
        CornerDecision decision;
    };
    constexpr std::array preferences{
        PreferenceCase{CornerPreference::Default, 8, CornerDecision::DefaultRoundPolicy},
        PreferenceCase{CornerPreference::Square, 0, CornerDecision::ExplicitSquare},
        PreferenceCase{CornerPreference::Round, 8, CornerDecision::RoundHint},
        PreferenceCase{CornerPreference::RoundSmall, 4, CornerDecision::SmallRoundHint},
        PreferenceCase{CornerPreference::Unsupported, 0, CornerDecision::ConservativeFallback}
    };
    struct DpiCase {
        std::uint32_t dpi;
        double scale;
        double roundInner, roundOuter, smallInner, smallOuter;
    };
    constexpr std::array dpis{
        DpiCase{96, 1, 8, 18, 4, 14}, DpiCase{120, 1.25, 10, 22.5, 5, 17.5},
        DpiCase{144, 1.5, 12, 27, 6, 21}, DpiCase{192, 2, 16, 36, 8, 28}
    };
    for (const auto eligibility : {CornerEligibility::Unverified, CornerEligibility::StandardFrame,
                                  CornerEligibility::HintOnly, CornerEligibility::UnsupportedShape}) {
        for (const auto& preference : preferences) {
            for (const auto& dpi : dpis) {
                auto facts = Ordinary();
                facts.cornerEligibility = eligibility;
                facts.cornerPreference = preference.preference;
                facts.dpi = dpi.dpi;
                facts.physicalTargetBounds = {-1800, -300, -1160, 100};
                facts.physicalDisplayBounds = {-1920, -600, 0, 480};
                facts.physicalWorkArea = {-1920, -560, 0, 480};
                const bool canRound = eligibility == CornerEligibility::StandardFrame
                    || (eligibility == CornerEligibility::HintOnly
                        && preference.preference != CornerPreference::Default);
                const double radius = canRound ? preference.radius : 0;
                const auto result = BuildRenderInput(facts);
                const auto& render = Result<RenderInput>(result, "eligibility/DPI/preference matrix renders");
                Near(render.radiusDip, radius, "default requires standard facts; hints require supported shape");
                Check(render.squareCorners == (radius == 0), "square classification matches effective radius");
                const auto expectedDecision = preference.preference == CornerPreference::Square
                    ? CornerDecision::ExplicitSquare
                    : canRound ? preference.decision : CornerDecision::ConservativeFallback;
                Check(render.cornerDecision == expectedDecision, "policy decisions retain their diagnostic meaning");
                if (radius > 0) {
                    const bool smallRound = preference.preference == CornerPreference::RoundSmall;
                    Near(render.radiusDip * render.deviceScale, smallRound ? dpi.smallInner : dpi.roundInner,
                         "target radius converts DIP to physical pixels exactly once");
                    Near((render.radiusDip + render.widthDip) * render.deviceScale,
                         smallRound ? dpi.smallOuter : dpi.roundOuter,
                         "rounded outer radius is target radius plus continuous band width");
                    Near((render.radiusDip - 1.0) * render.deviceScale,
                         smallRound ? 3.0 * dpi.scale : 7.0 * dpi.scale,
                         "one-DIP inward tuck subtracts from target radius, not outer radius");
                }
                for (const PhysicalRect edge : {PhysicalRect{-1920, -300, -1280, 100},
                                                PhysicalRect{-1800, -560, -1160, -160},
                                                PhysicalRect{-640, -300, 0, 100},
                                                PhysicalRect{-1800, 40, -1160, 480}}) {
                    facts.physicalTargetBounds = edge;
                    const auto aligned = BuildRenderInput(facts);
                    const auto& square = Result<RenderInput>(aligned, "negative work-edge retains available border");
                    Check(square.squareCorners && square.radiusDip == 0
                          && square.cornerDecision == CornerDecision::WorkAreaAligned,
                          "every work-edge/Snap fallback overrides all hints and eligibility");
                }
            }
        }
    }
    WindowFacts unverified{{100, 100, 740, 500}, {0, 0, 1920, 1080}, {0, 0, 1920, 1040},
                           96, 1, false, CornerPreference::Default};
    const auto unknown = BuildRenderInput(unverified);
    Check(Result<RenderInput>(unknown, "legacy facts without eligibility still render conservatively").squareCorners,
          "omitted eligibility never invents default rounding");
}

void HiddenNativeFixtureContract() {
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(previous != nullptr, "native fixture sets only its own thread PMv2 context");
    constexpr auto name = L"WindowSweaters.Geometry.HiddenFixture";
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance;
    cls.lpszClassName = name;
    Check(RegisterClassW(&cls) != 0, "own hidden native class registers");
    struct FixtureCase {
        DWORD style, extended;
        bool disableNonClient;
        CornerEligibility expected;
        bool customRegion{};
    };
    constexpr std::array fixtures{
        FixtureCase{WS_OVERLAPPEDWINDOW, 0, false, CornerEligibility::StandardFrame},
        FixtureCase{WS_POPUP | WS_THICKFRAME, 0, false, CornerEligibility::HintOnly},
        FixtureCase{WS_CAPTION, 0, false, CornerEligibility::HintOnly},
        FixtureCase{WS_OVERLAPPEDWINDOW, 0, true, CornerEligibility::HintOnly},
        FixtureCase{WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, false, CornerEligibility::HintOnly},
        FixtureCase{WS_OVERLAPPEDWINDOW, WS_EX_LAYERED, false, CornerEligibility::UnsupportedShape},
        FixtureCase{WS_OVERLAPPEDWINDOW, WS_EX_NOREDIRECTIONBITMAP, false, CornerEligibility::UnsupportedShape},
        FixtureCase{WS_OVERLAPPEDWINDOW, 0, false, CornerEligibility::UnsupportedShape, true}
    };
    for (const auto& fixture : fixtures) {
        const auto window = CreateWindowExW(fixture.extended, name, L"Geometry contract", fixture.style,
                                            100, 100, 640, 400, nullptr, nullptr, instance, nullptr);
        Check(window != nullptr && !IsWindowVisible(window), "fixture HWND belongs to test and is never shown");
        if (fixture.customRegion) {
            const auto region = CreateRectRgn(0, 0, 100, 100);
            Check(region != nullptr, "own fixture allocates a simple custom region");
            if (!SetWindowRgn(window, region, FALSE)) {
                DeleteObject(region);
                Check(false, "own hidden fixture takes ownership of the custom region");
            }
        }
        if (fixture.disableNonClient) {
            const auto policy = DWMNCRP_DISABLED;
            Check(SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy))),
                  "only own hidden fixture disables nonclient rendering");
        }
        for (const auto preference : {DWMWCP_DEFAULT, DWMWCP_DONOTROUND, DWMWCP_ROUND, DWMWCP_ROUNDSMALL}) {
            Check(SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
                                                 &preference, sizeof(preference))),
                  "only own hidden fixture sets documented corner preference");
            const auto query = QueryWindowGeometry(window);
            if (const auto failure = std::get_if<QueryFailure>(&query)) {
                std::cerr << "Native fixture query status=" << static_cast<int>(failure->status)
                          << " error=" << failure->nativeError << '\n';
            }
            Check(std::holds_alternative<WindowFacts>(query), "stable hidden native fixture returns queried facts");
            const auto& facts = std::get<WindowFacts>(query);
            Check(facts.cornerEligibility == fixture.expected, "eligibility follows documented styles/nonclient facts");
            RECT raw{};
            Check(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &raw, sizeof(raw))),
                  "own native extended bounds are observable without pixels");
            Check(facts.physicalTargetBounds == PhysicalRect{raw.left, raw.top, raw.right, raw.bottom},
                  "adapter preserves physical DWM bounds exactly");
            Check(facts.dpi == GetDpiForWindow(window), "adapter preserves documented PMv2 native DPI");
            const auto result = BuildRenderInput(query);
            const auto& render = Result<RenderInput>(result, "own hidden fixture facts drive pure policy");
            const bool rounded = fixture.expected != CornerEligibility::UnsupportedShape
                && (preference == DWMWCP_ROUND || preference == DWMWCP_ROUNDSMALL
                    || (preference == DWMWCP_DEFAULT && fixture.expected == CornerEligibility::StandardFrame));
            Near(render.radiusDip, !rounded ? 0 : preference == DWMWCP_ROUNDSMALL ? 4 : 8,
                 "native preference and eligibility produce policy radii, not measured pixels");
            Check(!IsWindowVisible(window), "native query does not show or mutate target");
        }
        Check(DestroyWindow(window) != FALSE, "own hidden fixture is destroyed");
    }
    Check(UnregisterClassW(name, instance) != FALSE, "own fixture class is unregistered");
    Check(SetThreadDpiAwarenessContext(previous) != nullptr, "fixture restores its thread awareness context");
}
}

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
    CornerEligibilityAndRadiusRelationships();
    HiddenNativeFixtureContract();
    std::cout << "PASS: " << checks << " geometry checks (deterministic and own hidden native facts; no pixel claims)\n";
    return EXIT_SUCCESS;
}
