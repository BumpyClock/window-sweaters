// SPDX-License-Identifier: GPL-3.0-only
#include "NativeGeometry.h"

#include <dwmapi.h>

namespace window_sweaters::geometry {
namespace {
PhysicalRect Physical(const RECT& rect) noexcept {
    return {rect.left, rect.top, rect.right, rect.bottom};
}
} // namespace

WindowQuery QueryWindowGeometry(HWND window) noexcept {
    if (!window || !IsWindow(window)) return QueryFailure{QueryStatus::InvalidWindow};
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                     DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return QueryFailure{QueryStatus::CallerNotPerMonitorV2};
    // GetDpiForWindow reports 96/system DPI for unaware/system-aware windows, not
    // their monitor's actual scale. Do not silently use that as physical render DPI.
    const auto awareness = GetWindowDpiAwarenessContext(window);
    if (!awareness) return QueryFailure{QueryStatus::InvalidWindow};
    if (GetAwarenessFromDpiAwarenessContext(awareness) != DPI_AWARENESS_PER_MONITOR_AWARE)
        return QueryFailure{QueryStatus::TargetNotPerMonitorAware};
    DWORD process{};
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!thread || !process) return QueryFailure{QueryStatus::InvalidWindow};

    RECT bounds{};
    HRESULT hr = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
    if (FAILED(hr)) return QueryFailure{QueryStatus::FrameBoundsFailed, hr};
    const auto target = Physical(bounds);
    if (target.Width() <= 0 || target.Height() <= 0)
        return QueryFailure{QueryStatus::InvalidNativeData};
    const UINT dpi = GetDpiForWindow(window);
    if (!dpi) return QueryFailure{QueryStatus::DpiFailed};
    const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL);
    if (!monitor) return QueryFailure{QueryStatus::MonitorUnavailable};
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
    const UINT finalDpi = GetDpiForWindow(window);
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
    if (thread != finalThread || process != finalProcess || Physical(finalBounds) != target
        || dpi != finalDpi || monitor != MonitorFromRect(&finalBounds, MONITOR_DEFAULTTONULL)
        || Physical(info.rcMonitor) != Physical(finalInfo.rcMonitor)
        || Physical(info.rcWork) != Physical(finalInfo.rcWork)
        || placement.showCmd != finalPlacement.showCmd
        || preferenceResult != finalPreferenceResult
        || (SUCCEEDED(preferenceResult) && nativePreference != finalPreference)
        || !AreDpiAwarenessContextsEqual(awareness, finalAwareness))
        return QueryFailure{QueryStatus::ChangedDuringQuery};

    WindowFacts facts{target, Physical(info.rcMonitor), Physical(info.rcWork), dpi,
        reinterpret_cast<std::uintptr_t>(monitor), placement.showCmd == SW_SHOWMAXIMIZED, preference};
    // Validate native coordinates before they leave the adapter.
    const auto checked = BuildRenderInput(facts);
    if (std::holds_alternative<GeometryFailure>(checked)) {
        const auto error = std::get<GeometryFailure>(checked);
        if (error == GeometryFailure::InvalidTargetBounds || error == GeometryFailure::InvalidDisplayBounds
            || error == GeometryFailure::InvalidWorkArea || error == GeometryFailure::InvalidDpi)
            return QueryFailure{QueryStatus::InvalidNativeData};
    }
    return facts;
}

} // namespace window_sweaters::geometry
