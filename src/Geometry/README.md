# Physical geometry contract

This module is the standalone geometry portion of issue #12. It does not complete
#12, integrate the host/renderer, implement a target tracker, or qualify a physical
mixed-DPI setup.

## Integration

Link `WindowSweaters.Geometry.vcxproj`, `user32.lib`, and `dwmapi.lib`.

1. On the coordinator's Per-Monitor V2 thread, call `QueryWindowGeometry(HWND)`.
   It returns `WindowFacts` or a diagnostic `QueryFailure`. No foreign window is
   resized, restyled, or given a corner preference.
2. Call pure `BuildRenderInput(query, DipStyle, GeometryLimits)`. Its variant holds
   immutable `RenderInput`, `HiddenReason`, `GeometryFailure`, or the original
   `QueryFailure`. Hide unsafe decoration on failures; preserve target identity
   and settings, then reconcile through the existing tracker.
3. Place the overlay at `physicalOuterBounds`, using integer physical dimensions.
   Render with `widthDip`, `radiusDip`, and `deviceScale`; do not scale the DWM
   rectangle. `physicalPaddingPixels = ceil(widthDip * deviceScale)` encloses the
   continuous fractional band. `frameOriginDip` is its leading transparent gutter.
   Pass physical content dimensions to the perimeter renderer rather than
   round-tripping them through float DIP dimensions.
   Pass `physicalPaddingPixels` as the renderer's authoritative padding override
   when supported. This prevents float style conversion from changing the integer
   envelope near a rounding boundary. Validate the returned perimeter's dimensions
   and content inset against this input before attaching its pieces.
4. Intersect composition with `clipRelativeToOuter`, then subtract the content
   shape bounded by `targetRelativeToOuter`, with physical radius
   `radiusDip * deviceScale` clamped to half its shorter extent.
   Both rectangles are half-open **local physical pixels**.
   Convert to DIP only if the compositor's clip API requires DIP. The subtraction
   excludes caption/client content without cutting off rounded exterior corners.
   Do not subtract a square rectangle for a rounded target. The upstream inward
   tuck may remain below the rounded target's content through correct stacking.
   Retain four narrow perimeter pieces; do not allocate a whole-window backing image.
5. Requery after movement, display/work-area/DPI changes, and failed queries. The
   caller owns lifetime/appearance/geometry generations, stale-work rejection,
   scale-specific tile caching, stacking, and reconciliation. `monitorIdentity`
   is a snapshot, not a durable identity or a handle safe to use after removal.

The width contract is finite `[2, 256]` DIP. Radius is finite and nonnegative.
Supported DPI is `[96, 768]`; scale is `dpi / 96`. Native queries reject targets
that are not per-monitor aware: `GetDpiForWindow` returns 96 or system DPI for
unaware/system-aware targets, which is not a measurement of their monitor's scale.
Those targets require separately designed host support; their geometry is not
reported as successful.

## Conservative corner and clipping policy

- A public DWM preference is a **hint**, not outline discovery or an actual-radius
  or Snap-state query. `WindowFacts.cornerEligibility` separates that hint from
  the acquired facts that permit the policy. Omitted eligibility is `Unverified`,
  which conservatively stays square, even with an explicit round hint.
- `StandardFrame` permits `DefaultRoundPolicy`: the requested target radius capped
  at **8 DIP** for an ordinary restored Windows 11 top-level window. The native
  adapter requires both `WS_CAPTION` and `WS_THICKFRAME`, enabled DWM non-client
  rendering, no child/minimized style, no `WS_EX_TOOLWINDOW`, no layering or
  no-redirection-bitmap style, and no reported window region. This fixes ordinary
  automatically rounded windows without treating every `DWMWCP_DEFAULT` window
  as either square or proven rounded.
- `HintOnly` covers other non-layered, non-region top-level frames that do not
  qualify for default rounding. Only explicit round/small-round hints permit the
  requested radius, capped at **8/4 DIP**. Default stays square. `UnsupportedShape`
  covers any reported region (including a single rectangle or an empty region),
  all `WS_EX_LAYERED` windows (a conservative superset of per-pixel-alpha windows),
  `WS_EX_NOREDIRECTIONBITMAP`, child, and minimized windows. Those shapes stay
  square even with a round hint. Unsupported corner capability and explicit
  `DWMWCP_DONOTROUND` also stay square.
- All rounded radii clamp to half the target's physical shorter side. These are
  rendering policies, not measured OS radii. Custom non-client handling, invisible
  shadow windows, VM/remote sessions, or other DWM heuristics can still defeat
  rounding. Public attributes cannot discover an exact outline for every custom,
  layered, or region window. The host still owns visibility/shape qualification.
  Do not use this policy as pixel evidence or mutate a foreign HWND to make it fit.
- Alignment of any target edge within one physical pixel of the selected
  monitor's work-area edge forces square corners, including when DWM reports a
  round hint. This deliberately includes ordinary edge-aligned windows and is
  not proof that Snap is active. No private API or forced tiling gap is used.
- The default clip is the selected monitor's work area. Explicit `Display`
  clipping is available only when the caller's policy permits display-edge
  exterior space. Negative monitor origins remain signed coordinates.
- A straddling window is conservatively clipped to the monitor selected by
  `MonitorFromRect(..., MONITOR_DEFAULTTONULL)`. This module does not invent
  continuity across other monitors, desktop holes, or disconnected displays.
  A removed monitor/no intersection is query uncertainty, not nearest-monitor
  safe success.
- Maximized or display-covering fullscreen facts hide the border. A clip wholly
  inside the target has no exterior space and also hides it. Adjacent-window
  occlusion is handled by stacking, not by this module.

Native querying reads DWM extended frame bounds, DPI, monitor information,
placement, awareness, corner preference, relevant style bits, region type/bounds,
and DWM non-client rendering twice and rejects changes during the bounded query.
It cannot make a foreign HWND or multi-API snapshot atomic. `GetWindowRgnBox`
documents the same `ERROR` return for no region and query errors: the adapter
clears last-error first, preserves any explicit nonzero error as `RegionFailed`,
and treats `ERROR` with no explicit error only as **no region reported**, not proof
of a rectangular pixel outline. No region rectangle is invented in that case.
Style and non-client failures remain `StyleFailed`/`NonClientRenderingFailed`
with their native error. Only known unsupported corner-attribute errors
(`E_INVALIDARG`/`E_NOTIMPL`) allow the explicit square capability fallback; other
DWM errors remain failures.

`RenderInput.radiusDip` is the **target/content** radius, not the outer border
radius. For rounded geometry, physical target radius is `radiusDip * deviceScale`;
continuous outer radius is `(radiusDip + widthDip) * deviceScale`, independent of
integer padding rounding. For the default 10-DIP band:

| DPI / scale | Round/default target → outer (px) | Small target → outer (px) |
| --- | --- | --- |
| 96 / 100% | 8 → 18 | 4 → 14 |
| 120 / 125% | 10 → 22.5 | 5 → 17.5 |
| 144 / 150% | 12 → 27 | 6 → 21 |
| 192 / 200% | 16 → 36 | 8 → 28 |

An inward tuck inset uses `max(0, radiusDip - tuckDip) * deviceScale`, further
clamped to half the inset content extent; it does not reuse the outer radius.
Square decisions keep the content and exterior square. Renderer/compositor
integration owns actual masks and antialiasing.

Before returning render geometry, checks bound coordinates, outer dimensions
(default/hard maximum 32768 pixels), finite style/DPI, and the four-piece BGRA
allocation estimate (default 64 MiB). The estimate includes corner, antialias, tuck,
and fractional-edge depth. It is not a GPU-memory, cache, or full-frame allocation
guarantee; those allocations remain bounded by their owners.

## Focused verification

From a VS 2026 native x64 developer command prompt:

```powershell
msbuild tests\Geometry\WindowSweaters.Geometry.Tests.vcxproj /m /v:minimal /p:Configuration=Debug /p:Platform=x64
.\tests\Geometry\build\x64\Debug\WindowSweaters.Geometry.Tests.exe
msbuild tests\Geometry\WindowSweaters.Geometry.Tests.vcxproj /m /v:minimal /p:Configuration=Release /p:Platform=x64
.\tests\Geometry\build\x64\Release\WindowSweaters.Geometry.Tests.exe
```

The standalone projects pin C++20, v145, SDK 10.0.26100.0, and Windows 11 x64.
Tests cover default eligible/ineligible facts, unsupported/square/round/small
preferences, their target/outer/tuck radius relationships at 100/125/150/200%,
transfers both directions, negative origins, fractional DIP width/gutters,
all edge clips, work-area/display clipping, Snap fallbacks, uncertain/removed-monitor
inputs, tiny/oversized geometry, finite values, coordinate overflow, and byte budgets.
The native contract creates only its own hidden HWNDs and reads documented
bounds/DPI/eligibility facts. It covers ordinary styles, missing caption/thick frame,
disabled non-client rendering, tool/layered/no-redirection styles, and a custom
region with all four preferences. Only these owned hidden fixtures are mutated;
no user-app pixels are read and no window is shown. The test alone links `gdi32.lib`
for its owned region. Thread PMv2 awareness is restored after fixture cleanup.

Measured focused Debug/Release runs: 1645 checks each on v145 / SDK 10.0.26100.0.
These checks establish geometry policy and native fact acquisition, not actual
corner outlines. Missing evidence: #4 stacking integration, #5 live renderer
integration, user-screenshot corner comparison after the GPU mask update, actual
mixed-DPI transfers both ways, physical Snap layouts/monitor-edge pixels, and
physical monitor removal. Pure simulations and hidden-HWND fact observations
do not establish any of those results.

## API evidence

- [GetWindowRect / unscaled DWM extended bounds](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getwindowrect)
- [GetDpiForWindow and target awareness](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getdpiforwindow)
- [Public DWM corner preference and hint limitations](https://learn.microsoft.com/windows/apps/desktop/modernize/ui/apply-rounded-corners)
- [MonitorFromRect](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-monitorfromrect)
- [GetMonitorInfo](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getmonitorinfow)
- [Style queries and zero/error distinction](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getwindowlongptrw)
- [Window-region query and ambiguous ERROR return](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getwindowrgnbox)
- [DWM non-client rendering enabled](https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
