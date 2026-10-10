# Window lifetime tracking

This module implements the issue #3 tracking contract. It does not own overlays,
renderer resources, stacking placement, settings, or persisted application identity.
Issue #3 remains open until the coordinator integrates the host and verifies the
separate-process desktop fixtures.

## Host integration

Compile `TrackingModel.cpp` and `WindowTracker.cpp` as native C++20. Link
`user32.lib` and `dwmapi.lib`. The focused project uses v145, Windows SDK
10.0.26100.0, x64, with minimum platform version 10.0.22000.0.

Construct `window_sweaters::tracking::WindowTracker` on the thread that owns the host
receiver HWND. That same thread must call every member and destroy the tracker.
Use one tracker per receiver. The receiver must belong to this process and remain
alive until `Stop()` returns.

1. Keep initial `--target HWND` and `--test-mode` fixture-isolated. Set
   `TrackerConfig.mode = TrackingMode::FixtureOnly` and `fixtureTarget` to the
   selected foreign HWND. A missing target or a target in this process makes
   `Start()` fail. Capture its `WindowIdentity` before host initialization and
   pass it as `initialFixtureIdentity`; a missing initial seal makes Start fail.
   Startup verifies that identity plus exact `WindowSweaters.TestFixture` class
   and `WindowSweaters Fixture` title, rather than resealing whatever now owns
   the numeric HWND.
2. The coordinator may enable `TrackingMode::Production` after integrating the
   action consumer. Production discovers eligible existing and future app windows.
   Pass settings/helper process IDs in `excludedPids`, and additional known host
   or helper classes in `excludedClasses`. Do not use unrestricted production
   mode as the implementation of `--test-mode`.
3. Call `bool Start(ActionBatch& initialActions)` after creating the receiver.
   Check the result. Apply the initial batch synchronously on that UI thread.
4. Forward messages to
   `bool HandleMessage(UINT, WPARAM, LPARAM, ActionBatch&)`. When it returns true,
   apply the returned batch and return zero from the host window procedure.
   No client data is stored in message `WPARAM` or `LPARAM`.
5. Before destroying the receiver or graphics device, call `Stop()` and apply
   its `Remove` batch. A second `Stop()` is harmless. The destructor removes
   subscriptions but cannot dispose host-owned overlays.

### Owned messages and limits

| Constant | Value | Host requirement |
| --- | --- | --- |
| `TrackerDispatchMessage` | `WM_APP + 20` | Reserve for coalesced event drains |
| `TrackerTimerId` | `0x5753` | Reserve this receiver's `WM_TIMER` ID |
| `TrackerTimerPeriodMs` | 100 ms | Tracker installs/kills the timer |
| `TrackerReconcilePeriodMs` | 2000 ms | Minimum interval between completed reconciliation passes |
| `TrackerTargetLimit` | 512 | Upper cap for `config.maxTargets` |
| `TrackerQueueLimit` | 1024 | Distinct queued HWNDs; reserve capacity for known targets |
| `TrackerDispatchBudget` | 64 | Maximum window queries in one drain |
| `TrackerReconcileBudget` | 32 | Reconciliation candidates queued per timer tick |
| `TrackerEnumerationLimit` | 4096 | Candidates in a reconciliation pass, including known targets |

Only `WM_APP + 20` is used. `WM_APP + 21` through `WM_APP + 29` are not currently
allocated. The tracker does not use the foundation's low message IDs.
Enumeration collects HWND values, not window snapshots, and each candidate is
queried when drained. Known targets are included even when absent from
enumeration. A pass over a very large desktop spans timer ticks; it is not a
promise that every candidate is queried every two seconds. Stable reconciliation
produces no rendering actions. Queue overflow requests another bounded pass.
`Stats()` exposes coalescing, overflows, uncertain queries, reconciliation
counts, truncations, and the last startup/posting/allocation error.

### Action consumer

Every action includes a value `TargetId { hwnd, pid, processStart, generation }`,
physical desktop `rect`, target-window `dpi`, `appearanceRevision`, and the
observed `topmost` band. Treat IDs and work tokens as immutable values. Keep the
complete ID with each host-owned overlay. A map indexed by HWND is acceptable
only if every operation also compares the full ID.

| Action | Consumer behavior |
| --- | --- |
| `Create` | Allocate a hidden overlay for this full ID; prepare initial appearance |
| `Move` | Update physical geometry and DPI; preserve visibility |
| `Hide` | Hide this lifetime's overlay; retain its resources/preferences |
| `Show` | Restore visibility; rebuild appearance only when requested |
| `Remove` | Dispose only the overlay with the matching full ID |
| `Reorder` | Ask the stacking owner to check this overlay's placement/band |

Apply batch order. Initial creation emits `Create`, `Show`, `Reorder`.
`Move.rebuildAppearance == false` forbids regenerating yarn on pure movement.
Size changes emit `Hide` before `Move` and defer regeneration until a settled
`Show` with `rebuildAppearance == true`. `Move` with rebuild set also covers DPI
or explicitly invalidated appearance when the rect is unchanged. Hidden movement
must never cause the overlay adapter to show itself.

`Reorder` is an action contract, not implementation of issue #4. It conveys the
target and band but does not compute neighboring windows. The stacking owner
must move only this host's overlays and must not activate, reorder, restyle,
reparent, or mutate a foreign HWND. A failed overlay creation requires host error
handling and an orderly stop/restart; this reducer does not acknowledge renderer
allocation failures.

For asynchronous rendering, capture only a `WorkToken { TargetId,
appearanceRevision }` plus immutable rendering input, never an entry, overlay,
or tracker pointer. Deliver the result to the owner UI thread. Call
`ValidateWork(token, updates)`, apply `updates`, and attach the result only if
validation succeeded and the host overlay still has that exact ID/revision.
Validation queries ownership/eligibility again and consumes any queued creation
hint before accepting work. `InvalidateAppearance()` schedules explicit style
changes without conflating them with movement or ordering.

Recursive application of validation updates can produce newer observations.
Recheck `Model().Accepts(token)` after consuming those updates. Use
`Model().CurrentSnapshot(token)` for canonical geometry, DPI, band and visibility;
it rejects a different lifetime/revision and retains suppressed snapshots with
`visible == false`. A no-delta validation does not make an older action payload
current. `MatchesFixtureSelection` is the shared class/title/initial-identity
predicate used by selection and Start.

## Eligibility and lifecycle policy

`Evaluate(Observation, Policy)` is the only eligibility gate for discovery,
notification processing, settlement, work revalidation, and reconciliation.
The pure reducer accepts either a complete `AliveWindow`, a
`DestroyedWindow`, or an `UncertainWindow`. Observations are synchronous
owner-thread samples, not delayed worker query results.

Supported targets are ordinary, captioned, unowned top-level windows. A `WS_POPUP`
root qualifies only as an `ApplicationFrameWindow` or with a full system frame
(`WS_SYSMENU | WS_THICKFRAME`), as Steam's `SDL_app` window has.
Child, tool, no-activate, owned popup/dialog, menu, tooltip, shell desktop/taskbar,
host/overlay/settings, excluded-PID/class, and production fixture-helper windows
are unsupported. Inaccessible or incomplete identity/geometry queries are
uncertain, not evidence of death. Hidden, minimized, cloaked, maximized, and
monitor-covering fullscreen windows are suppressed.

The adapter reads physical DWM extended frame bounds and checks monitor coverage
using physical outer bounds. Its scoped per-monitor-v2 thread context is restored
after queries. It does not change the target's DPI context or any target property.
PID plus `GetProcessTimes` creation time and an observed thread/class fingerprint
are revalidated. Identity changes remove the old generation before discovery of
the new one. Generations continue increasing across `Stop()`/restart.

An out-of-context event is a hint to take a fresh observation. In particular, a
late `DESTROY` notification cannot delete a live reused HWND. A later observed
`CREATE` timestamp also invalidates same-process/same-class reuse; duplicate and
older creation hints are ignored. Windows exposes no independent HWND creation
time here. Same-thread/class reuse with no observed absence and no distinguishable
creation hint cannot be proven from PID/process creation time alone; do not
represent this fingerprint as a persistent settings identity.

Uncertainty hides existing decoration, preserves the lifetime, and rejects work.
It never writes settings or removes preferences. Only confirmed absence,
confirmed identity replacement, or explicit shutdown removes an existing
overlay. A supported window becoming unsupported remains hidden for possible
recovery rather than being declared destroyed.

Move/size-loop notifications alone do not hide decoration. Actual width/height
changes start resize suppression; unchanged position movement does not. A fresh,
eligible, stable sample after 250 ms of size quiet restores the overlay, even
without a matching loop-end notification. A loop-end hint does not prematurely
expose changing dimensions. Settlement is timer-backed and reuses the same gate.

Location/move-size event drains also schedule three finite delayed observations
at least 100 ms apart. DWM's first extended-frame sample can lag behind the native
position event; waiting only for the two-second reconciliation could miss a
short-lived move before a fixture resets it. Delayed queries run on the owner
timer, coalesce per full lifetime, preserve the first deadline during bursts, and
admit at most 32 candidates per tick into the existing 64-query drain budget.
They never renew themselves, never render directly, reject replaced lifetimes,
and are cleared on Stop. Stable samples produce no actions or regeneration.

Callbacks filter object/child IDs, fixture scope, own/excluded PIDs, and desktop
roots, then merge dirty bits and post a drain. They do not render, query files,
extract icons, run a user callback, or mutate windows. `WINEVENT_SKIPOWNPROCESS`,
the PID filter, and ignoring desktop-root ordering notices prevent the host's
overlay operations from recursively scheduling themselves.

## Verification

From the repository root, with VS 2026 C++ tools installed:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe' `
  .\tests\Tracking\TrackingTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
.\out\x64\Debug\TrackingTests.exe --native-contract
```

The focused project inherits output/intermediate paths from root
`Directory.Build.props`: `out\x64\<Configuration>` and
`out\obj\TrackingTests\x64\<Configuration>`.

Repeat with `Release`. Measured during implementation: both configurations
compiled under v145/SDK 10.0.26100.0 with warnings as errors; seven deterministic
contract scenarios and the isolated native receiver/query rejection check passed.
The scripted scenarios cover discovery without CREATE, duplicate/missing/reordered
hints, suppression/recovery, query uncertainty, same/different-process HWND reuse,
stale work and revisions, movement versus resizing, settlement without END, DPI,
fixture isolation, and registry capacity.

The optional `--native-contract` check creates and destroys only its own hidden
receiver. It is not cross-process WinEvent, overlay, interaction, fullscreen,
cloaking, live-resize, or desktop-lifetime verification. Those remain the
coordinator's opt-in issue #3 fixture/integration gate.
