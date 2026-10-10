# Port parity and qualification

This ledger tracks [issue #1](https://github.com/BumpyClock/window-sweaters/issues/1) against the pinned upstream commit `a49dff52483dafdbe27f972bd271cd28d37cef1c`.

"Implemented" describes code, not release qualification. Model tests, native integration tests, upstream image comparisons, and clean-machine execution are separate evidence. Missing required evidence keeps the corresponding acceptance gate open.

| Issue | Contract | Implementation | Qualification |
| --- | --- | --- | --- |
| [#2](https://github.com/BumpyClock/window-sweaters/issues/2) | Native tray host and noninteractive exterior border | Implemented and independently reviewed | Clean-source Debug/Release builds, Computer Use targeted input, caption controls, and graceful shutdown verified; foreground pixel, resize-handle, and taskbar/Alt-Tab qualification pending |
| [#3](https://github.com/BumpyClock/window-sweaters/issues/3) | Target discovery, lifetime safety, move/resize settlement | Integrated and independently reviewed | Debug/Release host regressions and nine tracker scenarios pass; controlled desktop lifecycle qualification pending |
| [#4](https://github.com/BumpyClock/window-sweaters/issues/4) | Below-target stacking, dialogs, topmost bands | Pending | Pending visible ordering and input evidence |
| [#5](https://github.com/BumpyClock/window-sweaters/issues/5) | Faithful stockinette, shading, oriented edges, corners | Source math and native perimeter raster independently reviewed; live upload pending | Debug/Release: 131 checks pass; corrected same-chart texture comparison MAE 3.86/255; complete live parity pending |
| [#6](https://github.com/BumpyClock/window-sweaters/issues/6) | Native settings, host-owned persistence, secure revisioned IPC | Native WinUI and revisioned IPC/storage independently reviewed; host binding pending | Debug/Release: 412 real Session/IPC/storage assertions pass; integrated UI-host verification pending |
| [#7](https://github.com/BumpyClock/window-sweaters/issues/7) | Original stitches and all 37 selectable designs | Exact 37-theme, 47-chart source data ported; material and settings integration pending | C17/C++20 data tests and pinned-source verifier pass; visual parity pending |
| [#8](https://github.com/BumpyClock/window-sweaters/issues/8) | Persistent application identity and exclusions | Pending | Pending identity and relaunch tests |
| [#9](https://github.com/BumpyClock/window-sweaters/issues/9) | Verified Windows mappings and By App precedence | Pending | Pending resolver and native app evidence |
| [#10](https://github.com/BumpyClock/window-sweaters/issues/10) | Icon-derived colors, stable fallback, shared Zigzag | Pending | Pending synthetic icons and stale-work tests |
| [#11](https://github.com/BumpyClock/window-sweaters/issues/11) | Personal colorway rules and validated PNG charts | Pending | Pending imports, precedence, and restart tests |
| [#12](https://github.com/BumpyClock/window-sweaters/issues/12) | Physical geometry, mixed DPI, Snap, clipping | Geometry module implemented; host integration pending | Debug/Release: 290 deterministic checks pass; physical monitor matrix pending |
| [#13](https://github.com/BumpyClock/window-sweaters/issues/13) | Desktop, session, and power visibility | Pending | Pending transitions and conservative recovery |
| [#14](https://github.com/BumpyClock/window-sweaters/issues/14) | Shared-device loss and generation-safe recovery | Pending | Pending fault injection; real driver reset separate |
| [#15](https://github.com/BumpyClock/window-sweaters/issues/15) | Bounded perimeter rendering and idle/movement invariants | Pending | Pending named resource fixture and measured budgets |
| [#16](https://github.com/BumpyClock/window-sweaters/issues/16) | Reversible packaged startup and resilient tray/shutdown | Pending | Pending isolated StartupTask and operation tests |
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
