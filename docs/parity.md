# Port parity and qualification

This ledger tracks [issue #1](https://github.com/BumpyClock/window-sweaters/issues/1) against the pinned upstream commit `a49dff52483dafdbe27f972bd271cd28d37cef1c`.

"Implemented" describes code, not release qualification. Model tests, native integration tests, upstream image comparisons, and clean-machine execution are separate evidence. Missing required evidence keeps the corresponding acceptance gate open.

| Issue | Contract | Implementation | Qualification |
| --- | --- | --- | --- |
| [#2](https://github.com/BumpyClock/window-sweaters/issues/2) | Native tray host and noninteractive exterior border | Implemented and independently reviewed | Clean-source Debug/Release builds, Computer Use targeted input, caption controls, and graceful shutdown verified; foreground pixel, resize-handle, and taskbar/Alt-Tab qualification pending |
| [#3](https://github.com/BumpyClock/window-sweaters/issues/3) | Target discovery, lifetime safety, move/resize settlement | Integrated and independently reviewed | Eleven tracker scenarios pass; real Computer Use move/resize/minimize/restore fixture completes with stable lifetime and no pure-move redraw |
| [#4](https://github.com/BumpyClock/window-sweaters/issues/4) | Below-target stacking, dialogs, topmost bands | Helper and host wiring independently reviewed | 2,296 private-desktop checks pass per configuration; Computer Use captured modal occlusion and band changes; final live-host matrix pending |
| [#5](https://github.com/BumpyClock/window-sweaters/issues/5) | Faithful stockinette, shading, oriented edges, corners | Source math, native perimeter raster, and live host publication independently reviewed | Actual knitted border inspected through Computer Use; corrected same-chart texture comparison MAE 3.86/255; complete live parity pending |
| [#6](https://github.com/BumpyClock/window-sweaters/issues/6) | Native settings, host-owned persistence, secure revisioned IPC | Native WinUI, revisioned IPC/storage, and host binding independently reviewed | Real Session/IPC/storage checks pass; Computer Use verified live width/pause, restart persistence, Settings close with host running, and explicit host quit without automatic restart |
| [#7](https://github.com/BumpyClock/window-sweaters/issues/7) | Original stitches and all 37 selectable designs | Catalogue rendering, host appearance wiring, and persisted WinUI controls independently reviewed | 971 renderer checks and 43 production appearance comparisons pass; real Spotify selection, live width/pause, preview, persistence and restart verified through Computer Use |
| [#8](https://github.com/BumpyClock/window-sweaters/issues/8) | Persistent application identity and exclusions | Host-owned persistence and native Apps controls integrated and independently reviewed | Computer Use verified per-app changes, default-off overrides, persistence across new PID/HWND, all-off before first rendering, and retained exact-control focus without the former viewport jump |
| [#9](https://github.com/BumpyClock/window-sweaters/issues/9) | Verified Windows mappings and By App precedence | Per-target host resolution and bounded preview tracking integrated and independently reviewed | Clean Debug/Release: 487 Host checks, 616 adapter checks, 43 appearance comparisons, and 12 tracking scenarios plus hidden native tracking; Computer Use verified distinct Word/Excel sweaters and manual override |
| [#10](https://github.com/BumpyClock/window-sweaters/issues/10) | Icon-derived colors, stable fallback, shared Zigzag | Source palette math and bounded icon preparation independently reviewed; host integration pending | Synthetic and own-executable icon checks pass; cross-feature runtime qualification pending |
| [#11](https://github.com/BumpyClock/window-sweaters/issues/11) | Personal colorway rules and validated PNG charts | Native import model independently reviewed; host/UI persistence pending | Debug/Release: 1,618 checks pass, including removal/reimport revision safety; real restart qualification pending |
| [#12](https://github.com/BumpyClock/window-sweaters/issues/12) | Physical geometry, mixed DPI, Snap, clipping | Geometry and GPU corner policy integrated and independently reviewed | 1,645 focused geometry checks and actual GPU rounded-mask readback pass; normal fixture corners inspected at 100%; physical mixed-monitor matrix pending |
| [#13](https://github.com/BumpyClock/window-sweaters/issues/13) | Desktop, session, and power visibility | Public-API visibility and host wiring independently reviewed | Ten native/model groups pass; current-desktop tool-window publication verified; actual switching/lock/sleep transitions pending |
| [#14](https://github.com/BumpyClock/window-sweaters/issues/14) | Shared-device loss and generation-safe recovery | Bounded shared-device recovery integrated and independently reviewed | Injected real D3D/DComp reconstruction verified through Computer Use, with stable target and reused CPU yarn; actual driver reset remains unverified |
| [#15](https://github.com/BumpyClock/window-sweaters/issues/15) | Bounded perimeter rendering and idle/movement invariants | Bounded preparation independently reviewed; four-surface host publication implemented | 23 native tests, 382 assertions pass per configuration; aggregate live compositor budgets pending |
| [#16](https://github.com/BumpyClock/window-sweaters/issues/16) | Reversible packaged startup and resilient tray/shutdown | Native StartupTask controller independently reviewed; settings/tray persistence pending | State/cancellation checks pass; installed-package startup and Explorer restart pending |
| [#17](https://github.com/BumpyClock/window-sweaters/issues/17) | Native MSIX and Microsoft Store preparation | In progress under the user's Store scope change | Development identity only; Partner Center identity, packaged execution, and Store submission pending |
| [#18](https://github.com/BumpyClock/window-sweaters/issues/18) | Cross-feature parity and final qualification | Pending | Not release-qualified |

## Deliberate Windows policies

The proposed first-release behavior keeps decoration below its target. It hides decoration on minimized, hidden, cloaked, maximized, fullscreen, off-desktop, locked, or uncertain targets. Pure movement reuses yarn; actual live resize suppresses decoration until geometry settles.

Settings are separate from the tray host, which is the only configuration writer.
The user subsequently selected Microsoft Store distribution, replacing the
original ZIP-only first-delivery policy. The package contains both native
processes and declares opt-in startup disabled by default. Until the reserved
Store app's identity is supplied, build output uses a clearly marked development
identity. No package has been submitted or published.

These policies do not promise exact foreign-compositor animation synchronization,
forced tiling gaps, or simultaneous operation in multiple logon sessions.

## Evidence still required

Visual parity requires actual pinned-upstream output with stated tolerances. Windows-renderer output cannot serve as its own upstream baseline. A clean-machine check must use the produced Release artifact on an environment without development dependencies. Neither check is implied by compilation or model tests.

## First native slice

Measured on Windows 11 Enterprise build 26300, x64: the Release fixture started
the companion host, received targeted center and exterior-receiver clicks through
Computer Use, scrolled its list, accepted ten characters, and completed a
maximize/restore round trip. The host quit with exit code zero and both its
notification HWND and overlay HWND were destroyed.

Independent review found that assigning the hidden host as an overlay owner could
prevent below-target placement. The overlay is now an unowned tool window with
explicit host-managed destruction; positioning verifies its z-order postcondition.
Debug and Release rebuilt after this fix, and the same reviewer found no
significant issue in the correction.

The background overlay capture shows its colored exterior. The read-only screen
probe refused foreground pixel sampling because another application covered the
sample locations. Targeted Computer Use delivery is not proof of physical mouse
hit testing on an unobscured desktop. That check, resize handles, and taskbar/Alt-Tab
absence remain open. No user application was
selected by the fixture host.

The committed native host and fixture subsequently built in Debug and Release
from an isolated `git archive` of commit `c870b51`. CLI checks passed from that
clean source tree. This establishes clean-source build reproducibility on the
development machine, not clean-machine distribution qualification.

## Source texture port

The knitting core retains the source quadratic stitch paths, ridge layers,
periodic blur, and directional shading. Four narrow perimeter raster pieces
share immutable cached yarn tiles. Independent review found a vertical-buffer
orientation mismatch that changed the yarn highlights, plus a tiny-scale
coordinate overflow. Both were corrected and independently re-reviewed.

The same-chart, same-palette Spotify detail now has a measured mean absolute
channel difference of 3.86/255 against the pinned renderer-generated collection
crop, versus 16.81 before the orientation correction. Its measured edge-energy
ratio is 0.923. These crop metrics support texture fidelity; they do not establish
all catalogue variants, full-frame pixel identity, or live desktop parity.

## Native runtime corrections

Computer Use exposed a fixture redraw loop that model/build checks did not catch.
The fixture previously rewrote status while handling paint and read messages.
It now changes cached text only when state changes; native idle-query evidence
records zero status writes, and the rebuilt fixture's controls remain accessible.

Real Settings activation then failed in `Microsoft.UI.Xaml.dll`. Stage diagnostics
identified the missing application resource index:
`Cannot locate resource from 'ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml'`.
The resource existed in the framework PRI, but no canonical application
`resources.pri` connected the lookup. Native MakePRI now builds that index.
Computer Use subsequently inspected the actual native Settings window, including
the appearance controls and production preview. Host-connected mutations still
have separate qualification from activation. Subsequent native checks selected
Spotify, saved width and pause changes, restored the selected sweater after
restart, and closed Settings without stopping decoration.

## Native corners and device recovery

The Windows 11 default-corner policy now distinguishes eligible standard frames
from layered/custom-region or unsupported shapes. Normal eligible frames use an
8-DIP target-radius policy; small rounded hints use 4 DIP. Explicit square and
Snap-edge policies remain square. These values are documented policies, not
universal discovery of every application's custom outline.

GPU publication previously subtracted a rectangular content area, removing the
rounded corner yarn produced by the knitting core. It now uses the rounded hole
and retained one-DIP tuck beneath the verified target order. Actual GPU readback
checks cover square, normal, small-radius and fractional-scale masks,
premultiplied alpha, transparent central content, and continuous corner arcs.
Computer Use inspected the rebuilt normal fixture instead of relying only on
offscreen artwork.

A fixture-only injected graphics failure also rebuilt the actual shared
D3D/DirectComposition resources. The device generation advanced, the old overlay
was replaced, the full target lifetime stayed unchanged, the current sweater
returned, and CPU yarn-generation count remained constant. This does not claim
an actual driver reset.

## Native application exclusions

Schema 3 and protocol 4 carry the host-owned default eligibility and persistent
application exceptions. Older supported configuration files migrate in memory
without a startup rewrite. Running owners remain in the Apps list when disabled;
saved non-running choices remain distinguishable from running owners. Inaccessible
owner metadata does not admit a provisional border.

Computer Use verified a disabled running application, an explicit enabled
exception under default-off, and persistence after the fixture acquired a new
PID and HWND. An approved isolated All-off action cleared exceptions. On the next
launch, the host created no overlay and accepted no rendering work before an
explicit enable. Re-enabling reused the CPU yarn tile.

Native checks exposed a viewport jump caused by rebuilding controls and
reprojecting unchanged appearance. Apps controls now retain their identity and
exact focused subcontrol. The subsequent first-snapshot candidate crashed after
window activation. Initial Apps presentation now waits for an attached, measured
WinUI root, and queued presentation failures report their stage and HRESULT
rather than escaping the dispatcher. The rebuilt application presented the
initial snapshot and remained responsive. Off/on changes retained the focused
Remove saved choice control and unchanged top-of-page coordinates. This proves
the observed startup and top-of-page flow; it does not identify the old native
exception's precise cause or establish every nonzero-scroll position.

An archive of the staged source, excluding parallel catalogue/mapping changes
and the next resolver adapter, built all six native Release projects. It passed
1,248 configuration assertions, 123 ownership checks, 727 application checks,
43 original-catalogue appearance comparisons, and the hidden Host integration
contract. This establishes that the exclusion slice builds and runs against its
committed 37-design prerequisites. It is not clean-machine or Store qualification.

## Authored Windows app resolution

The clean 37-design candidate resolved the exact default Microsoft Office Word
and Excel executable identities. Computer Use launched an isolated preview host
admitting only those two inspected lifetimes, then selected By app in native
Settings. Word received its blue authored wave chart; Excel received its green
authored check chart. Both overlays were visible, metadata was available, and
appearance errors were zero.

Selecting Spotify manually applied the same saved chart and base yarn to both
owners. Returning to By app restored their distinct authored charts without
increasing yarn-generation count beyond the manual selection's count. The
Settings preview continued to label By app as a Finder example.

Independent review found that late preview filtering could admit a replacement
of an initially suppressed HWND and let unrelated windows exhaust tracking
capacity. Selected-only tracking now seals suppressed lifetimes at discovery,
filters before queuing/admission, and permanently retires destroyed selections.
Model tests include 600 unrelated owners and same-process HWND reuse. A hidden
native child-process test covers suppressed discovery and destruction. Retained
review cleared both corrections.

Twelve static observations of the two-app host retained draws at 4, GPU uploads
at 4, yarn generations at 3, private bytes at 53,649,408, and native handles at
1,167. This is a bounded two-window observation, not a total compositor-memory
budget, many-window leak test, or universal zero-CPU claim.

Closing Settings left both decorations running. After the user selected Quit
from the preview host's tray menu, its process exited and the final probe recorded
`stopped=true`, zero tracked entries, and an empty target array. No foreign
application was closed or edited during this preview.
Native persisted restart of this two-app preview remains unverified; the
identity/relaunch and configuration persistence checks are separate model and
IPC evidence.
