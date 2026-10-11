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
| [#10](https://github.com/BumpyClock/window-sweaters/issues/10) | Icon-derived colors, stable fallback, shared Zigzag | Host worker/resolver integration and distinct persisted mode independently reviewed | Clean Debug/Release: 558 Host, 619 adapter, and 1,279 configuration checks pass; positive native sampling and two Shared Zigzag restarts verified; earlier selection rejection remains unexplained |
| [#11](https://github.com/BumpyClock/window-sweaters/issues/11) | Personal colorway rules and validated PNG charts | Host-owned storage, renderer/resolver and redesigned Settings integrated and independently reviewed | Final Debug/Release: 1,625 configuration assertions; 95 import scenarios and 641 hidden Host assertions; direct Computer Use verified selected personal-cu preview and live border |
| [#12](https://github.com/BumpyClock/window-sweaters/issues/12) | Physical geometry, mixed DPI, Snap, clipping | Geometry and GPU corner policy integrated and independently reviewed | 1,645 focused geometry checks and actual GPU rounded-mask readback pass; normal fixture corners inspected at 100%; physical mixed-monitor matrix pending |
| [#13](https://github.com/BumpyClock/window-sweaters/issues/13) | Desktop, session, and power visibility | Public-API visibility and host wiring independently reviewed | Ten native/model groups pass; current-desktop tool-window publication verified; actual switching/lock/sleep transitions pending |
| [#14](https://github.com/BumpyClock/window-sweaters/issues/14) | Shared-device loss and generation-safe recovery | Bounded shared-device recovery integrated and independently reviewed | Injected real D3D/DComp reconstruction verified through Computer Use, with stable target and reused CPU yarn; actual driver reset remains unverified |
| [#15](https://github.com/BumpyClock/window-sweaters/issues/15) | Bounded perimeter rendering and idle/movement invariants | Aggregate retained CPU/estimated-surface admission integrated and independently reviewed | Sixteen 8192×4096 width-64 hidden GPU targets stay inside 128/256 MiB admission caps; owner-refresh release and waiter recovery pass; physical compositor memory remains unqualified |
| [#16](https://github.com/BumpyClock/window-sweaters/issues/16) | Reversible packaged startup and resilient tray/shutdown | Startup preferences, OS-state reporting and bounded TaskbarCreated recovery integrated and independently reviewed | Debug/Release configuration 1,660 and fake startup 20,193 checks pass; actual installed startup and Explorer restart pending |
| [#17](https://github.com/BumpyClock/window-sweaters/issues/17) | Native MSIX and Microsoft Store preparation | x64/ARM64 build and versioned package pipeline independently reviewed | Frozen 0.2.0.0 unsigned dual-architecture bundle validates; identical corresponding source archives and per-architecture runtime/PRI checks pass; identity deferred |
| [#18](https://github.com/BumpyClock/window-sweaters/issues/18) | Cross-feature parity and final qualification | Pending | Not release-qualified |

## Deliberate Windows policies

The proposed first-release behavior keeps decoration below its target. It hides decoration on minimized, hidden, cloaked, maximized, fullscreen, off-desktop, locked, or uncertain targets. Pure movement reuses yarn; actual live resize suppresses decoration until geometry settles.

Settings are separate from the tray host, which is the only configuration writer.
The user subsequently selected Microsoft Store distribution, replacing the
original ZIP-only first-delivery policy. The package contains both native
processes and declares opt-in startup disabled by default. Until the reserved
Store app's identity is supplied, build output uses a clearly marked development
identity. No package has been submitted or published.
The user subsequently requested x64 and ARM64 in one bundle, then chose unsigned
output and skipped local signing, certificate trust and installation. ARM64
cross-compilation and payload inspection do not establish execution on ARM64.

## Dual-architecture delivery gate

The package pipeline binds each architecture's original native input hashes and
archived corresponding-source inventory to its current build receipt, before and
after bundling. Staged EXE debug-path normalization is recorded separately.
Contract tests reject stale packages beside a newer same-version native build,
wrong architectures, missing bundle entries and inconsistent output roots.

Outputs are version-scoped under
`packaging\artifacts\Development\0.2.0.0\<Platform>`. The combined bundle sits in
the version folder, preserving older artifacts outside its input set.
The generated bundle contains exactly one x64 and one ARM64 application package,
with development identity and version 0.2.0.0. Per-architecture validation covers
42 x64 and 41 ARM64 binaries, matching CRT/runtime architectures, unchanged
application PRI, no private paths, and no test executables or signing files.
Both corresponding-source archives are identical and contain 240 source files.
Fresh x64 Release native tests pass; ARM64 Host, Settings and Fixture cross-compile.

The runtime now reserves shared retained CPU/chart/tile ownership and estimated
four-surface BGRA backing before upload, including replacement overlap and upload
transient storage. Hidden GPU pressure fixtures cover 16 large targets and
capacity retries. These limits are not physical driver/compositor measurements.
The actual machine has one 1920×1200 display, Windows 11 Enterprise build 26300,
AMD Ryzen AI 7 PRO 350 and Radeon 860M. Mixed physical DPI, minimum-build clean
execution and ARM64 hardware execution are unavailable in this environment.

Direct Computer Use inspected the final x64 build's actual native Settings and
knitted preview. The unpackaged startup row was disabled and reported Unavailable
with saved Off. The single fixture retained 474,728 CPU bytes and 396,304 estimated
surface bytes with no quota or rendering error. Closing Settings returned exit
zero while Host continued; stopping Host released the ledger to zero and all
test processes exited.

A requested slider value of 20 was not verified by the accessibility tool;
the host actually saved 16.221574783325195. The resize tool reported completion
without changing the observed 1000×720 window, so compact-layout qualification
remains open. These observations are unpackaged x64 evidence, not installed
MSIX execution, ARM64 hardware verification, WACK or Store certification.

Native perimeters add ring lighting that upstream lacks. Each band is lit as a
padded tube under one upper-left screen-space light (`ws_knit_shade_ring`), so
crowns brighten, flanks fall off, and the inner flank darkens where yarn tucks
under the target. Lighting scales channels uniformly, so stitch colour stays
authored. Stitch relief uses the same light: tiles carry unlit albedo plus a
periodic stitch height map, the renderer samples that height through the same
orientation, clip and brush transforms as colour, and
`ws_knit_shade_ring_relief` tilts the tube normal by the height gradient per
screen pixel. Legs therefore face the upper left on all four sides and corners,
where upstream bakes its light in tile space and rotates it with each edge.
Source plates bake a weak tile-space light, so luminance cannot be compared
pixel for pixel. Source-plate parity compares chromaticity (uniform channel
scaling cancels): yarn-colour MAE, chroma edge energy, plus mean brightness
(production within 0.9-1.15x of the ring-lit plate; measured 1.01-1.07x) and
luminance spread (0.85-1.8x; measured 1.07-1.41x, deliberately plumper relief).
Light direction is covered by a screen-space test on all four sides.

The same light casts a soft black drop shadow (`ws_knit_ring_drop_shadow`, peak
0.3 alpha) down and to the right onto the desktop outside the band, so the ring
reads as a raised tube. Upstream has no such shadow. The overlay reserves a
transparent margin of `ceil(padding / 2)` physical pixels on each side for it
(`ShadowMarginPixels`). Windows still covers the inner tuck, so there is no
contact shadow on window content. A shadow that would show only in that margin,
with the band off the clip, stays hidden.

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

## Current local working-source build

The earlier original-catalogue build did not include the Windows-authored
Store, Zed, and GitHub Copilot designs. The current source contains 55
designs and an exact measured standalone GitHub Copilot path mapping.
Resolver checks pass 604 assertions, including those mappings and literal palettes;
application identity checks pass 728 assertions. These are model checks, not a
live demonstration of every additional Windows design.

The first local build overlapped other source edits and is not a frozen-source
qualification. A transient renderer compilation failure involved the new ring
shading variable; the current source has the corrected `tube` name. The requested
updated artifact includes that new tube lighting. Its pixel changes require fresh
visual qualification; earlier upstream-image metrics describe the older renderer.

A restored Microsoft Store frame was rejected because its ordinary
captioned `ApplicationFrameWindow` includes `WS_POPUP`. A narrow eligibility
correction is committed as `f0ba646`, without admitting arbitrary popup windows
or weakening the application-owner metadata gate.

## Personal imports and Settings redesign

Schema 5 and protocol 6 add bounded personal import metadata and a host-owned
stored catalogue. Atomically stored PNG contents, exact application bindings and
the catalogue revision high-water mark survive restart and final-chart removal.
The native UI supports local rules files/text, chart add/select/replace/remove,
and explicit application colors. Personal PNG colors remain literal.

Review identified and corrected startup snapshot inconsistency, acknowledged
edit-chain loss after selected-PNG clearing/reset, and unnecessary rerendering
of unrelated manually styled owners. Retained review cleared those corrections.
Final Debug/Release configuration checks pass 1,625 assertions; the corrected
hidden Host checks pass 641 assertions. Model/hidden checks remain separate from
desktop evidence.

The Fluent-native redesign places status and preview before appearance,
applications and personal designs. Stable containers adapt inside one scrolling
root without reparenting controls. Imported previews identify the asset and
revision; automatic-mode previews stay representative. A reviewed RulesDraft
guard preserves unsaved rule text across unrelated import saves and newer edits
across older acknowledgments.

Direct Computer Use in a child agent inspected the exact reviewed Release
Settings and matching import Host. Selecting personal-cu and Use selected chart
committed revision 4 with asset ID
`imported:sha256:32c2b9a728047b7e11e6f06d8c3983e1b731d29fa01b3d85b3e91c033db8f0f3`.
The preview and live border both showed the literal pink/green chart, with zero
appearance/render errors. The wide 1000×720 and maximized layouts were observed.
Compact resizing was interrupted by user input. Alternate themes, physical input,
and this manual import's restart persistence remain unverified.

The direct tool reported a successful Close click without closing the Settings
window. No WM_CLOSE receipt was established, so this is not proof of an
application shutdown defect. The native system-menu attempt also exposed no
Close action. The user then closed the isolated Settings window; process
observation confirmed all test apps exited. No process kill or shell-input
workaround was used.

## Native application exclusions

The exclusion slice introduced schema 3 and protocol 4 for default eligibility and persistent
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

## Icon yarn integration

Schema 4 and protocol 5 add an explicit Shared Zigzag selection while preserving
the previous mode values and application exceptions. Valid schemas 1–3 migrate
in memory without startup writes. The live endpoint rejects older protocols;
legacy decoding is limited to disk migration.

Computer Use selected By app and Shared Zigzag in the native Settings app.
Mapped Word kept its authored palette in By app. Unknown packaged Notepad's
icon returned an explicit monochrome fallback. Shared Zigzag applied its
generated motif with provisional app-specific colors; Word's icon also returned
an explicit unsupported-source fallback. The host reported no appearance errors,
and the shared selection was durably saved as schema 4. These observations prove
the native mode and fallback flow, not a successful live icon sample.

The ordinary native fixture now embeds the existing known-color test icon.
Production PE sampling checks its exact `0xffc04b4b` base and `0xfff6f0de`
contrast. Computer Use then verified actual red/cream knitted publication with
`icon_sampled=true`, no fallback, and no appearance error. Shared Zigzag retained
that sampled palette and saved its distinct mode.

A new fixture process restored the saved By app mode and sampled the same color.
A subsequent same-fixture restart after saving Shared Zigzag exited with code 3
before configuration loading or overlay creation. Two explicit starts produced
the same pre-selection rejection. The previous stopped probe was not evidence
of a newly started host. The normal all-app host continued running independently.

The exact FixtureOnly selector now exposes bounded native-stage/error diagnostics.
Its read-only inspector accepted the same fixture. Two subsequent user-driven
starts with the diagnostic candidate restored Shared Zigzag revision 30,
resampled the exact red yarn, and published a visible border without appearance
errors. The earlier rejection did not reproduce; its cause remains unknown.
The diagnostic change preserves eligibility and query semantics rather than
substituting an unsafe fallback. No claim is made that it fixed the unseen cause.
