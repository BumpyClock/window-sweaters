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
4. Intersect composition with `clipRelativeToOuter`, then subtract
   `targetRelativeToOuter`. Both rectangles are half-open **local physical pixels**.
   Convert to DIP only if the compositor's clip API requires DIP. The subtraction
   excludes caption/client content, including the upstream renderer's inward tuck.
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

- A public DWM preference is a **hint**, not an actual-radius or Snap-state query.
  Unknown/unsupported/default preferences use square corners. Explicit no-round
  also uses square corners. Explicit round/small-round hints permit the requested
  radius, capped by policy at 8/4 DIP and half the target's physical shorter side.
  These caps are not measured OS radii.
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
placement, awareness, and corner preference twice and rejects changes during the
bounded query. It cannot make a foreign HWND or multi-API snapshot atomic.
Only known unsupported corner-attribute
errors (`E_INVALIDARG`/`E_NOTIMPL`) allow the explicit square fallback; other DWM
errors remain failures.

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
Tests cover 100/125/150/200% transforms, transfers both directions, negative origins,
fractional DIP width/gutters, all edge clips, work-area/display clipping, Snap
fallbacks, uncertain/removed-monitor inputs, tiny/oversized geometry, finite values,
coordinate overflow, and byte budgets. Native smoke tests cover invalid HWNDs
without opening or driving a desktop window.

Missing evidence: #4 stacking integration, #5 live renderer integration, native
successful-window query qualification, actual mixed-DPI transfers both ways,
physical Snap layouts/monitor-edge pixels, and physical monitor removal. Pure
simulations and invalid-HWND calls do not establish any of those results.

## API evidence

- [GetWindowRect / unscaled DWM extended bounds](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getwindowrect)
- [GetDpiForWindow and target awareness](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getdpiforwindow)
- [Public DWM corner preference and hint limitations](https://learn.microsoft.com/windows/apps/desktop/modernize/ui/apply-rounded-corners)
- [MonitorFromRect](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-monitorfromrect)
- [GetMonitorInfo](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-getmonitorinfow)
