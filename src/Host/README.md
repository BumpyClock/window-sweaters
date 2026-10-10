# Native tracking host

The host compiles the issue #3 tracker directly, without Windows App SDK or a
separate lifecycle framework. Its receiver owns the tracker, every overlay, and
one shared Direct2D/DirectComposition device. Tracker shutdown removes exact
lifetimes before the graphics device or receiver is destroyed.

## CLI and supported targets

`WindowSweaters.Host.exe` without `--target` discovers supported ordinary,
captioned, unowned application windows. The tracker applies the same conservative
eligibility policy during discovery, events, work validation, and reconciliation.
Shell, popup, tool, child, host, fixture-helper and settings windows are excluded.
The tracker excludes the live host PID and known helper classes. Before allocating
an overlay, the host also checks the target PID's process creation time and helper
executable name, including settings launched later. This excludes every window
in a helper process without permanently blacklisting an external PID that could
be reused. The host does not load the settings runtime.

`--target <decimal-or-0x-HWND>` is fixture-only, with or without `--test-mode`.
It requires the exact foreign `WindowSweaters.TestFixture` class and
`WindowSweaters Fixture` title. The tracker seals the target process/thread/class
identity at startup. `--test-mode` requires `--target` and selects the separate
test single-instance mutex. The fixture's **Start test host** control remains
compatible. `--help` prints the complete CLI.

Selection captures the initial `WindowIdentity` before mutex, COM and graphics
initialization. Tracker startup requires that same identity and rechecks the
exact fixture class/title; a reused HWND cannot be resealed as a different app.

The host retains each complete `TargetId`, not just its HWND. It applies
Create/Move/Hide/Show/Remove/Reorder batches in order. Pure movement reuses the
existing surface. Actual size changes hide the border and retain pending physical
bounds; only settled Show prepares the latest size. Minimize, hidden/cloaked,
maximize/fullscreen, and temporary-query suppression retain recoverable state.
Synchronous appearance work is revalidated with immutable value `WorkToken`s;
there are no worker pointers or graphics/file work in the WinEvent callback.
After applying recursive query updates, the consumer rechecks the token against
the current model. Geometry, DPI, stacking band and visibility come from the
complete exact-token `CurrentSnapshot`, not an older batch payload or only the
latest query delta. An older Show cannot override a newer suppression observation.

Stacking remains the foundation's nonactivating, unowned, ordinary-band border
below the target. Topmost targets are hidden until issue #4 implements their band;
returning to the ordinary band can restore them. A failed ordering check hides
that border and retries bounded owner-thread work. Overlay creation or geometry
failure is logged and stops the host cleanly, rather than reporting success.

## Checks and fixture evidence

```powershell
.\scripts\test.ps1 -Configuration Debug
.\scripts\test.ps1 -Configuration Release
```

These build the host and TrackingTests, run CLI rejection checks, run the host's
`--integration-contract` scripted action/geometry assertions, and run the tracker
model plus isolated hidden-receiver contract. They never launch production
tracking or a visible fixture. The host's scripted assertions do not establish
actual DComp drawing, cross-process WinEvents, stacking, or desktop interaction.
Source-derived regressions cover nested uncertainty/minimize/destruction/DPI
changes after an accepted query, old-batch Show/Reorder after no-delta validation,
and fixture PID/process/thread/class/title changes across the startup gap.

For coordinator-controlled desktop verification, build with `-IncludeFixture`
and use the fixture's existing controls. Optional fixture-only
`--probe-output <path>` retains the original JSON fields and adds complete target
identity, visibility, creates/removes, geometry updates, draws, pure moves,
hide/show/order counts, stale actions, placement failures and tracker diagnostics.
Snapshots use atomic replacement and are sampled only while changed; shutdown
writes a final stopped/empty snapshot. `draws` counts successful size-dependent
surface preparations in the existing plain-border adapter, not yarn generation
or GPU memory. Static movement must leave this count unchanged; settled resize
must change it, and confirmed removal must clear the overlay.

Foreground pixels, live input, resizing, minimize/restore, exact native lifetime
reuse, topmost behavior and handle/resource qualification remain desktop gates.
No issue is closed by these scripted checks.
