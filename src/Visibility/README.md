# Visibility integration contract

This module implements the native and pure portion of issue #13. It does not wire
the host or establish Windows desktop/lock/sleep acceptance.

## Native owner

Compile `VisibilityPolicy.cpp` and `NativeVisibility.cpp` into the host. Link
`ole32.lib`, `uuid.lib`, `wtsapi32.lib`, `user32.lib`, and `dwmapi.lib`. The focused
test project uses C++20, v145, SDK 10.0.26100.0, Windows 11 x64, and no WinUI.

Construct `NativeVisibility` on the receiver thread. The receiver must be an
existing, hidden top-level host HWND on the interactive desktop. A message-only
window is rejected because it cannot receive the required broadcasts. The
receiver remains owned by the host.

`Start()` returns separate notification-registration and virtual-desktop HRESULTs.
Check both. A failed desktop result never authorizes decoration; observations
remain uncertain. Required WTS/session-display/suspend-resume registrations are
all-or-nothing. Default construction initializes STA COM and uses only the public
`IVirtualDesktopManager`.

Forward `WM_WTSSESSION_CHANGE` and `WM_POWERBROADCAST` to `HandleMessage`.
For handled messages, Windows message processing still uses the appropriate
return value, including `TRUE` for a handled `WM_POWERBROADCAST`.
Use `result.policy.snapshot`, or capture `Snapshot()` by value. Apply every changed
snapshot to all current target states before accepting work. A snapshot contains
private fields and has no mutable native-state references.

The initial session state comes from `WTSQuerySessionInformationW` with
`WTSSessionInfoEx`. Missing/unknown data suppresses decoration. Display state is
unknown until a valid `GUID_SESSION_DISPLAY_STATUS` notification. Suspend
invalidates the old display state; automatic resume alone does not establish that
the display is on or that the session is unlocked.

Call `RefreshUncertainSession(GetTickCount64())` from the existing owner-thread
maintenance timer. It retries only an unknown awake, unpaused session at most once
per five seconds. It does no target work while locked or suspended.
Use `RetryVirtualDesktop(GetTickCount64())` during active reconciliation.
It retries unavailable/disconnected public COM activation at most once per five
seconds. Known desktop query failures continue to suppress the target.

Route host/settings pause through `SetPaused`. Visibility never owns or rewrites
configuration, application identity, target enumeration, geometry, or rendering.

## Target and overlay sequence

Keep one `TargetVisibility` for each current tracking lifetime. Map the tracking
`TargetId` fields `hwnd`, `pid`, `processStart`, and `generation` without changing
their meaning. Remove the state only when the tracking owner ends that lifetime.
Suppression alone never ends a target or deletes its saved settings.

1. Create overlays hidden. `RegisterOverlay` accepts only this process's
   owner-thread, top-level `WindowSweaters.Overlay` windows with
   `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT`. It marks ownership
   with a property that disappears when the HWND is destroyed. Registering a
   random same-process HWND, a foreign HWND, or a stale HWND is not sufficient.
2. Consume `state.ApplyPolicy(currentSnapshot)`. Apply its `hide` effect immediately.
   Enabling policy also requires a fresh observation before showing again.
3. Obtain `token = state.BeginProbe(currentSnapshot)` and call
   `native.Observe(token, overlayHwnd)`. A missing overlay may be represented by
   `nullptr`; overlay creation remains the host's responsibility.
4. Pass the result to `state.Reconcile(currentSnapshot, observation)`.
   Reject `accepted == false`. Consume `hide` before `associate`. A normal,
   current-desktop target can show only after overlay desktop membership matches.
5. Before executing an association, revalidate the host's current target lifetime
   and `state.CanAssociate(currentSnapshot, token, desktop)`. Ensure the overlay is
   created, registered, and hidden. Call
   `native.AssociateOverlay(overlayHwnd, token, desktop)`, then
   `state.CompleteAssociation(currentSnapshot, token, desktop, SUCCEEDED(hr))`.
   Consume a resulting `show` only after the host's other eligibility, geometry,
   graphics, and stacking gates also pass.
6. Before publishing asynchronous rendering work, revalidate the current tracking
   lifetime and `state.CanPublish(currentSnapshot, token)`. Newer probes,
   policy changes, association failures, and generation changes reject old work.
7. Call `UnregisterOverlay` before destroying/replacing an overlay. At shutdown,
   consume suppression, stop work, destroy overlays, and call `Stop()` on the
   owner thread before destroying the receiver. `Stop` and duplicate transitions
   are idempotent. Destroy the adapter on that same thread.

   STA desktop COM calls can pump messages and reenter the adapter. Each native
   operation retains its own manager reference and shutdown-lifetime generation.
   `Stop` invalidates policy and registration authority immediately, but releases the
   active manager and uninitializes COM only after all operations unwind. It is safe
   to call `Stop` reentrantly, not to delete the adapter while an operation is active.
   Restart is rejected while stopped operations are still unwinding. Every desktop
   query rechecks authority after each COM call; stale observations and association
   completions return `E_PENDING`. Association checks current policy, lifetime,
   overlay ownership, target eligibility, and the token revision immediately before
   the only mutating COM call.

   Each overlay registration has a distinct epoch. Operations capture it before
   queries and verify it after pumping queries, before moves, and before successful
   completion. Unregistering, destroying, or unregistering/re-registering the same
   HWND invalidates the old operation even when the global policy is unchanged.

Only `AssociateOverlay` calls `MoveWindowToDesktop`. It passes a hidden, registered
own overlay, never the target. It checks PID/process creation time and target
desktop membership before moving, then checks target facts and both desktop
memberships again. Failure leaves the overlay suppressed; it is not a safe
fallback. The host must use its current lifetime registry before calling native
operations because HWND/PID/process-time queries cannot identify same-process
HWND reuse by themselves.

## Bounded reconciliation

Use `PollGate::Due(now, snapshot)` with a round-robin queue of at most `PollBudget`
(32) current targets per batch, no more than once per `PollIntervalMs` (250 ms).
Do not restart that queue on every focus event. Cap registered overlays at
`MaxOverlays` (512). Reconcile on policy reactivation and tracking signals too;
moving a target between desktops does not necessarily change keyboard focus.

Inactive policy disables target queries and rendering work. A stable successful
poll has no hide/show/association effects and does not request a redraw. Native
queries compare repeated desktop ID/current-membership results and suppress
observed drift. There are no private desktop notifications or forced switches.

Public polling and separate HWNDs do not provide atomic compositor attachment or
frame-perfect desktop animation. With 512 targets and the stated batch limits,
a full polling round takes about four seconds before native call cost. A desktop
transition can occur immediately after the last public query. Native desktop,
fixture-move, lock/unlock, sleep/resume, fullscreen, and animation tests are still
required on the wired host.

## Focused checks

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild '.\tests\Visibility\VisibilityTests.vcxproj' /nologo /m /verbosity:minimal /p:Configuration=Debug /p:Platform=x64
& '.\out\x64\Debug\VisibilityTests.exe'
& $msbuild '.\tests\Visibility\VisibilityTests.vcxproj' /nologo /m /verbosity:minimal /p:Configuration=Release /p:Platform=x64
& '.\out\x64\Release\VisibilityTests.exe'
```

The tests cover pure transitions, uncertainty, off-desktop return/association,
fullscreen and other suppression gates, pause, sleep/resume, duplicate and late
work, generation reuse, and bounded polling. Native tests create only invisible
in-process receiver/overlay fixtures, register/unregister notifications, query
public current-session state, and use a fake documented desktop COM interface.
They never activate the shell manager, move a real window between desktops, show
desktop UI, or change OS session/display/power state. Active native query checks
report a skip if the test runner's session is not observed interactive.

The focused project alone defines `WINDOW_SWEATERS_VISIBILITY_TESTING` to inject
synthetic target facts for invisible fixtures. Production builds have no facts
override. Reentrancy tests inject stop, pause, session lock, and pause/unpause at
each of the 12 query boundaries in observation and association. They verify
immediate suppression, deferred COM release, rejection of stale results and
pre-move authorization, and restart only after operations unwind. A separate fake
move reenters shutdown after an authorized invocation and verifies its completion
is discarded. No test calls the shell's `MoveWindowToDesktop`.
An additional 108 query-boundary cases inject overlay unregistration, same-HWND
re-registration, and direct destruction, including association's final confirming
observation. Stale registration incarnations cannot return matching/show authority.
