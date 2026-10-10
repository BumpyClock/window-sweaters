// SPDX-License-Identifier: GPL-3.0-only
#include "Stacking.h"

namespace window_sweaters::rendering {
namespace {

constexpr UINT PlacementFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
constexpr HRESULT Retry = HRESULT_FROM_WIN32(ERROR_RETRY);

HRESULT LastError() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

HRESULT ReadStyle(HWND window, int index, LONG_PTR& style) noexcept {
    SetLastError(ERROR_SUCCESS);
    style = GetWindowLongPtrW(window, index);
    return style || GetLastError() == ERROR_SUCCESS ? S_OK : LastError();
}

HRESULT Query(HWND overlay, HWND target, StackingSnapshot& snapshot) noexcept {
    DWORD process{};
    const DWORD thread = GetWindowThreadProcessId(overlay, &process);
    if (!thread || process != GetCurrentProcessId() || thread != GetCurrentThreadId() ||
        !target || overlay == target || !IsWindow(target)) return E_INVALIDARG;

    LONG_PTR overlayStyle{}, targetStyle{}, overlayExtended{}, targetExtended{};
    HRESULT result = ReadStyle(overlay, GWL_STYLE, overlayStyle);
    if (FAILED(result)) return result;
    result = ReadStyle(target, GWL_STYLE, targetStyle);
    if (FAILED(result)) return result;
    if ((overlayStyle | targetStyle) & WS_CHILD ||
        GetWindow(overlay, GW_OWNER) || GetWindow(target, GW_OWNER)) return E_INVALIDARG;
    result = ReadStyle(overlay, GWL_EXSTYLE, overlayExtended);
    if (FAILED(result)) return result;
    result = ReadStyle(target, GWL_EXSTYLE, targetExtended);
    if (FAILED(result)) return result;

    snapshot.targetBand = targetExtended & WS_EX_TOPMOST ? StackingBand::Topmost : StackingBand::Ordinary;
    snapshot.overlayBand = overlayExtended & WS_EX_TOPMOST ? StackingBand::Topmost : StackingBand::Ordinary;
    snapshot.immediatelyBelow = GetWindow(target, GW_HWNDNEXT) == overlay;
    return S_OK;
}

}

HRESULT PlaceOverlayBelow(HWND overlay, HWND target) noexcept {
    StackingSnapshot snapshot{};
    HRESULT result = Query(overlay, target, snapshot);
    if (FAILED(result)) return result;

    const StackingAction action = PlanStacking(snapshot);
    if (action == StackingAction::None) return S_OK;
    if (action == StackingAction::PromoteThenPlaceBelow ||
        action == StackingAction::DemoteThenPlaceBelow) {
        const StackingBand expectedBand = snapshot.targetBand;
        // An unowned overlay cannot drag a foreign owner through a band transition.
        const HWND band = expectedBand == StackingBand::Topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
        if (!SetWindowPos(overlay, band, 0, 0, 0, 0, PlacementFlags)) return LastError();
        result = Query(overlay, target, snapshot);
        if (FAILED(result)) return result;
        if (snapshot.targetBand != expectedBand || snapshot.overlayBand != expectedBand) return Retry;
    }
    if (!snapshot.immediatelyBelow &&
        !SetWindowPos(overlay, target, 0, 0, 0, 0, PlacementFlags)) return LastError();

    result = Query(overlay, target, snapshot);
    if (FAILED(result)) return result;
    return PlanStacking(snapshot) == StackingAction::None ? S_OK : Retry;
}

}
