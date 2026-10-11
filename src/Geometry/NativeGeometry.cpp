// SPDX-License-Identifier: GPL-3.0-only
#include "NativeGeometry.h"

#include <dwmapi.h>
#include <ShellScalingApi.h>

#pragma comment(lib, "shcore.lib")

namespace window_sweaters::geometry {
namespace {
PhysicalRect Physical(const RECT& rect) noexcept {
    return {rect.left, rect.top, rect.right, rect.bottom};
}

// System-aware and unaware windows report system DPI; DWM scales them to their monitor.
UINT EffectiveDpi(HWND window, DPI_AWARENESS awareness, HMONITOR monitor) noexcept {
    if (awareness == DPI_AWARENESS_PER_MONITOR_AWARE) return GetDpiForWindow(window);
    UINT x{}, y{};
    return SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) ? x : 0;
}

struct NativeCornerFacts {
    std::uint32_t frameStyle{};
    std::uint32_t extendedStyle{};
    int regionType{};
    PhysicalRect regionBounds{};
    bool nonClientRendering{};
    bool operator==(const NativeCornerFacts&) const = default;
};

using CornerQuery = std::variant<NativeCornerFacts, QueryFailure>;

CornerQuery QueryCornerFacts(HWND window) noexcept {
    NativeCornerFacts facts;
    SetLastError(ERROR_SUCCESS);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!style && GetLastError() != ERROR_SUCCESS)
        return QueryFailure{QueryStatus::StyleFailed, GetLastError()};
    SetLastError(ERROR_SUCCESS);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!extended && GetLastError() != ERROR_SUCCESS)
        return QueryFailure{QueryStatus::StyleFailed, GetLastError()};
    facts.frameStyle = static_cast<std::uint32_t>(style)
        & (WS_CAPTION | WS_THICKFRAME | WS_CHILD | WS_MINIMIZE | WS_MAXIMIZE);
    facts.extendedStyle = static_cast<std::uint32_t>(extended)
        & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW);

    RECT region{};
    SetLastError(ERROR_SUCCESS);
    facts.regionType = GetWindowRgnBox(window, &region);
    if (facts.regionType == ERROR) {
        const auto error = GetLastError();
        if (error != ERROR_SUCCESS) return QueryFailure{QueryStatus::RegionFailed, error};
        // ERROR without an explicit error is the documented no-region result.
    } else if (facts.regionType == NULLREGION || facts.regionType == SIMPLEREGION
               || facts.regionType == COMPLEXREGION) {
        facts.regionBounds = Physical(region);
    } else {
        return QueryFailure{QueryStatus::InvalidNativeData};
    }
    BOOL enabled{};
    const auto hr = DwmGetWindowAttribute(window, DWMWA_NCRENDERING_ENABLED,
                                         &enabled, sizeof(enabled));
    if (FAILED(hr)) return QueryFailure{QueryStatus::NonClientRenderingFailed, hr};
    facts.nonClientRendering = enabled != FALSE;
    return facts;
}

CornerEligibility Eligibility(const NativeCornerFacts& facts) noexcept {
    if (facts.regionType != ERROR
        || (facts.extendedStyle & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP))
        || (facts.frameStyle & (WS_CHILD | WS_MINIMIZE)))
        return CornerEligibility::UnsupportedShape;
    const auto required = WS_CAPTION | WS_THICKFRAME;
    if ((facts.frameStyle & required) == required && facts.nonClientRendering
        && !(facts.extendedStyle & WS_EX_TOOLWINDOW))
        return CornerEligibility::StandardFrame;
    return CornerEligibility::HintOnly;
}
}

WindowQuery QueryWindowGeometry(HWND window) noexcept {
    if (!window || !IsWindow(window)) return QueryFailure{QueryStatus::InvalidWindow};
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                     DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return QueryFailure{QueryStatus::CallerNotPerMonitorV2};
    const auto awareness = GetWindowDpiAwarenessContext(window);
    if (!awareness) return QueryFailure{QueryStatus::InvalidWindow};
    const auto targetAwareness = GetAwarenessFromDpiAwarenessContext(awareness);
    if (targetAwareness == DPI_AWARENESS_INVALID) return QueryFailure{QueryStatus::InvalidWindow};
    DWORD process{};
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!thread || !process) return QueryFailure{QueryStatus::InvalidWindow};
    const auto cornerFacts = QueryCornerFacts(window);
    if (const auto failure = std::get_if<QueryFailure>(&cornerFacts)) return *failure;

    RECT bounds{};
    HRESULT hr = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
    if (FAILED(hr)) return QueryFailure{QueryStatus::FrameBoundsFailed, hr};
    const auto target = Physical(bounds);
    if (target.Width() <= 0 || target.Height() <= 0)
        return QueryFailure{QueryStatus::InvalidNativeData};
    const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL);
    if (!monitor) return QueryFailure{QueryStatus::MonitorUnavailable};
    const UINT dpi = EffectiveDpi(window, targetAwareness, monitor);
    if (!dpi) return QueryFailure{QueryStatus::DpiFailed};
    MONITORINFO info{sizeof(info)};
    SetLastError(ERROR_SUCCESS);
    if (!GetMonitorInfoW(monitor, &info))
        return QueryFailure{QueryStatus::MonitorInfoFailed, GetLastError()};
    WINDOWPLACEMENT placement{sizeof(placement)};
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowPlacement(window, &placement))
        return QueryFailure{QueryStatus::PlacementFailed, GetLastError()};

    DWM_WINDOW_CORNER_PREFERENCE nativePreference{};
    const HRESULT preferenceResult = DwmGetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &nativePreference, sizeof(nativePreference));
    CornerPreference preference{};
    if (preferenceResult == E_INVALIDARG || preferenceResult == E_NOTIMPL) {
        // Only known capability absence permits conservative square geometry.
        preference = CornerPreference::Unsupported;
    } else if (FAILED(preferenceResult)) {
        return QueryFailure{QueryStatus::CornerPreferenceFailed, preferenceResult};
    } else {
        switch (nativePreference) {
        case DWMWCP_DEFAULT: preference = CornerPreference::Default; break;
        case DWMWCP_DONOTROUND: preference = CornerPreference::Square; break;
        case DWMWCP_ROUND: preference = CornerPreference::Round; break;
        case DWMWCP_ROUNDSMALL: preference = CornerPreference::RoundSmall; break;
        default: return QueryFailure{QueryStatus::InvalidNativeData};
        }
    }

    RECT finalBounds{};
    hr = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &finalBounds, sizeof(finalBounds));
    if (FAILED(hr)) return QueryFailure{QueryStatus::FrameBoundsFailed, hr};
    const UINT finalDpi = EffectiveDpi(window, targetAwareness, monitor);
    if (!finalDpi) return QueryFailure{QueryStatus::DpiFailed};
    MONITORINFO finalInfo{sizeof(finalInfo)};
    SetLastError(ERROR_SUCCESS);
    if (!GetMonitorInfoW(monitor, &finalInfo))
        return QueryFailure{QueryStatus::MonitorInfoFailed, GetLastError()};
    WINDOWPLACEMENT finalPlacement{sizeof(finalPlacement)};
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowPlacement(window, &finalPlacement))
        return QueryFailure{QueryStatus::PlacementFailed, GetLastError()};
    DWM_WINDOW_CORNER_PREFERENCE finalPreference{};
    const HRESULT finalPreferenceResult = DwmGetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &finalPreference, sizeof(finalPreference));
    if (FAILED(finalPreferenceResult) && finalPreferenceResult != E_INVALIDARG
        && finalPreferenceResult != E_NOTIMPL)
        return QueryFailure{QueryStatus::CornerPreferenceFailed, finalPreferenceResult};
    const auto finalAwareness = GetWindowDpiAwarenessContext(window);
    if (!finalAwareness) return QueryFailure{QueryStatus::InvalidWindow};
    DWORD finalProcess{};
    const DWORD finalThread = GetWindowThreadProcessId(window, &finalProcess);
    if (!finalThread || !finalProcess) return QueryFailure{QueryStatus::InvalidWindow};
    const auto finalCornerFacts = QueryCornerFacts(window);
    if (const auto failure = std::get_if<QueryFailure>(&finalCornerFacts)) return *failure;
    if (thread != finalThread || process != finalProcess || Physical(finalBounds) != target
        || dpi != finalDpi || monitor != MonitorFromRect(&finalBounds, MONITOR_DEFAULTTONULL)
        || Physical(info.rcMonitor) != Physical(finalInfo.rcMonitor)
        || Physical(info.rcWork) != Physical(finalInfo.rcWork)
        || placement.showCmd != finalPlacement.showCmd
        || preferenceResult != finalPreferenceResult
        || (SUCCEEDED(preferenceResult) && nativePreference != finalPreference)
        || std::get<NativeCornerFacts>(cornerFacts) != std::get<NativeCornerFacts>(finalCornerFacts)
        || !AreDpiAwarenessContextsEqual(awareness, finalAwareness))
        return QueryFailure{QueryStatus::ChangedDuringQuery};

    WindowFacts facts{target, Physical(info.rcMonitor), Physical(info.rcWork), dpi,
        reinterpret_cast<std::uintptr_t>(monitor), placement.showCmd == SW_SHOWMAXIMIZED, preference,
        Eligibility(std::get<NativeCornerFacts>(cornerFacts))};
    const auto checked = BuildRenderInput(facts);
    if (std::holds_alternative<GeometryFailure>(checked)) {
        const auto error = std::get<GeometryFailure>(checked);
        if (error == GeometryFailure::InvalidTargetBounds || error == GeometryFailure::InvalidDisplayBounds
            || error == GeometryFailure::InvalidWorkArea || error == GeometryFailure::InvalidDpi)
            return QueryFailure{QueryStatus::InvalidNativeData};
    }
    return facts;
}

}
