# Native knitted-border host

The host compiles the issue #3 tracker directly, without Windows App SDK or a
separate lifecycle framework. Its receiver owns the tracker, every overlay, and
one shared Direct2D/DirectComposition device, and one render queue. The host also
owns the configuration writer and authenticated Settings endpoint. It does not
import Windows App SDK, WinUI, XAML, or the Settings runtime.
The host and its configuration-library variant use the static CRT (`/MT`).
Settings keeps its separate `/MD` configuration build. The two projects compile
the same configuration sources into different object paths; they do not share
CRT-owned C++ allocations across application boundaries.

## CLI and supported targets

`WindowSweaters.Host.exe` without `--target` discovers supported ordinary,
captioned, unowned application windows. The tracker applies the same conservative
eligibility policy during discovery, events, work validation, and reconciliation.
Shell, popup, tool, child, host, fixture-helper and settings windows are excluded.
The tracker excludes the live host PID and known helper classes. Before allocating
an overlay, an MTA metadata worker validates the full target/process birth and
supported window-owner properties. Verified executable metadata classifies helper
names, including settings launched later. This excludes every window
in a helper process without permanently blacklisting an external PID that could
be reused. The host does not load the settings runtime.

`--target <decimal-or-0x-HWND>` is fixture-only, with or without `--test-mode`.
It requires the exact foreign `WindowSweaters.TestFixture` class and
`WindowSweaters Fixture` title. The tracker seals the target process/thread/class
identity at startup. `--test-mode` requires `--target` and isolated storage and
IPC. Supply both `--storage-dir <absolute-path>` and `--endpoint-suffix <safe-name>`,
or supply `--probe-output` in an existing user-owned directory. The latter derives
the `fixture-configuration` storage directory and a stable endpoint suffix from
the probe parent. Test paths cannot overlap production storage; canonical directory
ownership and the exclusive writer lock are checked before loading configuration.
Override switches are rejected outside test mode. The fixture's **Start test host**
control remains compatible and cannot write the real user's configuration.
`--help` and `--integration-contract` return before opening configuration storage.

Selection captures the initial `WindowIdentity` before configuration, COM and graphics
initialization. Tracker startup requires that same identity and rechecks the
exact fixture class/title; a reused HWND cannot be resealed as a different app.

The host retains each complete `TargetId`, not just its HWND. It applies
Create/Move/Hide/Show/Remove/Reorder batches in order. Pure movement reuses the
existing four-piece surface set. Actual size changes hide the border and retain
pending physical bounds; only settled Show prepares the latest size. Minimize, hidden/cloaked,
maximize/fullscreen, and temporary-query suppression retain recoverable state.
Asynchronous appearance work is revalidated with immutable value `WorkToken`s;
there are no worker pointers or graphics/file work in the WinEvent callback.
After applying recursive query updates, the consumer rechecks the token against
the current model. Geometry, DPI, stacking band and visibility come from the
complete exact-token `CurrentSnapshot`, not an older batch payload or only the
latest query delta. An older Show cannot override a newer suppression observation.

Stacking uses the issue #4 helper for a nonactivating, unowned overlay directly
below the target in its current ordinary or topmost band. It never reorders the
foreign target. A failed ordering check hides the border and retries bounded
owner-thread work. Invalid or temporarily unavailable native geometry hides the
border; it does not invent a rectangle or display a placeholder.

## Render publication

The decoration is source-derived knitting, not a uniformly colored placeholder.
Committed configuration selects yarn color, stitch rows, stockinette/rib/garter,
plain/raised material, width, pause, and each semantic catalogue design.
Original charts retain their authored flags, corners, cuffs, palettes, and material
normalization; they are not recolored or exaggerated for visibility. Fine plain
yarn may look nearly solid from a distance.

The host and Settings preview use the same production knitting core. In By-app
mode, each validated owner resolves through the reviewed Windows identity
mappings. Authored charts retain their literal palettes and finishing flags.
Unmatched owners initially use the resolver's stable identity-based Wool color
and plain pattern, then may acquire locally sampled colors and a stable
Zigzag, Picnic, or Twinkle motif. Explicit plain or original design selection
preserves the saved manual appearance instead of applying app rules.
The Settings Finder image remains a labelled example, not the selected owner's
actual appearance.

Known-folder roots are queried once outside target rendering. Each entry retains
its resolved immutable appearance. Changed owner facts or effective appearance
retire that entry's render epoch before queue publication; unchanged owner facts
and unrelated eligibility changes do not regenerate its yarn. Resolution errors
are reported and suppress unsafe publication, rather than becoming a successful
plain fallback.

## Icon yarn and shared Zigzag

Shared Zigzag is a distinct persisted mode. It samples every eligible owner,
including mapped apps, and applies one shared motif with per-app colors.
Ordinary By app mode does not sample mapped owners or alter their authored
palettes. The resolver derives shared contrast from the winning base yarn;
its personal-rule precedence remains available, but personal import persistence
and UI are not integrated yet.

The host supplies kind-prefixed canonical UTF-8 identity bytes. Executable keys
use invariant uppercase; packaged AUMIDs remain exact. Icon input uses a verified
transient process-image source, never a basename, window title, or foreign
application content. A hosted application's missing source does not fall back
to ApplicationFrameHost's icon.

One completion gate and one bounded preparation worker belong to the Host.
Owner-thread draining checks the full target lifetime, process birth, mode,
eligibility, source, icon appearance epoch, and device generation. Only a changed
resolved TileSpec advances the affected target's rendering. Closing, excluding,
or superseding a target retires its subscription before late work can publish.
Worker saturation receives at most four targeted retries; invalid/stopped work
and unavailable or insufficient-color icons retain explicit fallback reasons.
Positive and negative results stay retained by each current entry, so cache
eviction alone cannot schedule new sampling or redraws.

Local PE icon reads retain the module's 64 MiB executable, 256 KiB icon,
500 ms cooperative deadline, and aggregate queue/cache bounds. They do not
load executables as code or invoke shell handlers. There is no file watcher:
a changed source path invalidates the sample, while same-path replacement
requires Host restart. These limits can reject valid large or unusual app icons.

## Application eligibility

Ordinary-window discovery is independent of enabledness. Resource-free tracked
entries remain present while disabled or awaiting metadata. One `EligibleNow`
predicate gates preparation, movement, visibility reconciliation, retries,
render completion, and final show. Unknown/unverified owners fail closed and
remain non-mutable snapshot diagnostics; hosted AUMIDs never collapse into
ApplicationFrameHost's executable identity.

Metadata requests and completions use the complete `TargetId`, process birth,
configuration revision and request sequence. Metadata is queried only on the
worker, not geometry/WinEvent callbacks. Configuration changes retire pending
metadata admission; accepted owner facts remain target-lifetime scoped. Stop
joins the worker and retires every returned cancellation on its owner gate.
Live metadata retention is bounded to 256 KiB, independent of the worker cache.
Owner-thread metadata polling submits at most eight requests per pass and
refreshes accepted/failure facts after 30 seconds using the existing worker/cache
contract. A changed same-lifetime window-owner property retires that target's
render epoch and re-evaluates the current preference; retained stale metadata is
not treated as a permanent identity.

The queue's shared revision advances only for global appearance changes.
Each target has an effective render epoch separate from its tracking appearance
token. Application-policy changes retire only affected target pending/retry/
prepared gates and advance those epochs. They do not cancel unrelated queued
renders or hide/regenerate other owners. A later re-enable submits a new epoch
on the same lifetime; obsolete completion cannot satisfy its pending gate.
Destruction still cancels exactly the complete target lifetime. Pure movement
continues to reuse pixels. Global width/pause/material changes can invalidate
the shared queue as before.
The sole configuration writer also retains bounded effective-eligibility
revisions for currently observed identities. These runtime pins are not saved
and disappear only when that owner has no running windows. Every durable
effective change advances only its identity's revision, including a disable/
re-enable pair between Host polls. The central gate refreshes current
configuration before publication and checks this per-owner fence; an unrelated
configuration revision cannot retire another owner's queued result.

## Isolated real-application preview

`--inspect-preview <HWND>` performs a read-only metadata and authored-resolution
check. Its token includes HWND, PID, process birth, and a native thread/class
fingerprint. It does not launch, focus, move, or change the application.

A preview launch accepts at most two `--preview-target <token>` selections and
requires `--test-mode`, an explicit `--storage-dir`, `--endpoint-suffix`, and
`--probe-output`. Native facts are revalidated at startup. The tracker applies
the bounded selection before enumeration, event queuing, and admission.
Suppressed selected windows receive their original tracking generations before
first rendering. Destruction or replacement permanently retires a selection;
later HWND reuse cannot authorize a new window.

Preview settings and exceptions stay in the supplied isolated directory. No
unselected application enters its registry. Use the sibling Settings process
with the same test directory and endpoint; its ordinary controls update the
preview host. The probe's `targets` array records each admitted lifetime,
resolved chart/color, render epoch, metadata/error state, and overlay visibility.
These diagnostics do not establish physical input, desktop-transition, or
Store-installed qualification.

The Host publishes accepted target-owner metadata to the configuration binding.
The server derives current snapshots from committed preferences: disabled
running apps, saved non-running exceptions and non-mutable unavailable owners
remain distinguishable. Observation generation tracks metadata and window-count
changes independently from persisted revision.
`ConfigurationHost::Revision()` performs a scalar, allocation-free committed
revision check for final publication. Unchanged polling does not copy the
variable-size preference map; full snapshots remain available to consumers.

Native geometry supplies physical content dimensions, scale, rounded-corner
policy, outer bounds, authoritative physical padding, monitor clip, and the
content-exclusion rectangle. Each immutable queue request includes the complete
target identity, appearance revision, committed configuration revision, and device
generation. Generation starts at 1 and advances before shared GPU recovery.

One MTA worker owns the CPU/D2D/WIC preparation and tile cache. The UI thread submits
only appearance, settled-size, and DPI changes. Movement does not submit jobs.
The worker posts a cookie-bearing `WM_APP + 30`; it never calls UI callbacks or
reads/writes configuration. The host drains results, queries the current exact
tracker snapshot and native geometry, and rejects obsolete configuration,
appearance, device, lifetime, or physical dimensions before GPU attachment.
Movement between submission and completion attaches at current bounds, not the
queued position.

`Overlay::Upload(shared_ptr<const KnitPerimeter>, physicalOuterBounds,
clipRelativeToOuter, contentExclusion, radiusPixels, tuckPixels)` uploads premultiplied BGRA into four narrow
DirectComposition surfaces. `physicalOuterBounds` is the target plus band padding
plus a `ShadowMarginPixels(padding)` drop-shadow margin per side, which equals the
perimeter's `contentInsetPixels`. The overlay is therefore half a band larger
than the band on each side. It stays click-through, and the margin is
transparent apart from the soft shadow. Four child visuals retain the perimeter pieces.
There is no full-window backing surface. The root clips to the current monitor
region; the bitmap upload subtracts the matching rounded client-interior geometry.
Its inner rectangle is inset by the source's one-DIP tuck and its radius is the
physical window radius minus tuck, clamped for small content. The inward source
yarn remains under the nonactivating overlay's verified below-target placement;
the central client interior stays transparent. Explicit square targets keep a
square interior. `UpdatePlacement` repositions and updates clipping without
regenerating textures. Unsafe or unprepared decoration stays hidden.

Quota rejection and worker/native preparation failures are logged and keep the
affected decoration hidden. Queue-capacity rejection retains an exact current-input
retry gate instead of permanently poisoning that appearance. Consumed completions
and exact-target cancellation retry at most eight queued targets per pass, in FIFO
order; they do not run a static polling or yarn-regeneration loop. Invalid input
and native preparation failures remain failed until their render key changes.
Removal cancels the exact `TargetId`. Configuration invalidation and
terminal queue failure retire host pending gates explicitly; they do not wait
for a cancelled completion.

## Settings and lifetime

The production host holds the exclusive canonical per-user configuration writer
lock before loading settings or decorating. A second owner exits with code 4;
invalid options exit with code 2. Settings remains a separate trusted sibling
application. The tray offers **Settings**, **Pause** or **Resume**, and **Quit**.
Settings launch uses the binding's exact sibling path and `CreateProcessW`, not a
shell, PATH lookup, or an arbitrary foreground application. Test launch passes
the host's isolated storage/endpoint binding.

Tray mutations are bounded asynchronous configuration submissions. Queued is not
saved. The receiver timer drains local completion futures and polls the newest
committed snapshot. Only saved revisions change active appearance: the host hides
overlays, invalidates queued work, and asks the tracker for new appearance tokens.
Pause keeps every overlay hidden; Resume prepares the current committed width.
The tracker's stationary rebuilding `Move` action also prepares and publishes:
width changes and Resume do not depend on a later move, resize, or Show event.
Pure-position `Move` actions continue to reuse the existing textures.
The WinEvent callback does not poll settings or perform storage work.

Closing Settings does not stop the host. Quit stops IPC, joins the render worker,
removes hooks and exact overlay lifetimes, releases graphics, and destroys the
receiver. Settings is not terminated and remains disconnected instead of restarting
an already-connected host.

## Desktop, session and power gates

The host owns one native visibility adapter on its hidden top-level receiver,
not a message-only window. It registers WTS/session-display/suspend notifications,
uses the public `IVirtualDesktopManager`, and keeps one visibility state per exact
tracking lifetime. Unknown session/display/desktop information suppresses decoration.
Lock, suspend, off-desktop, cloak and pause cannot authorize preparation or Show.
Resume requires fresh display/session evidence rather than reusing an old token.

Overlays are created hidden and registered once with owner-thread/PID/property
and registration-epoch checks. Before preparing, moving prepared pixels, accepting
a worker result or finally showing, the host rechecks current visibility authority.
An overlay on a different desktop is hidden before association. Only its registered
own overlay can be moved; the host never moves the foreign target or switches the
user's desktop. Removing a target revokes overlay registration before destruction.
Current-desktop authority uses repeated public current-membership checks and a
stable target desktop identity. Tool overlays may not expose a desktop GUID;
`CurrentDesktopVerified` is an explicit capability, not a fabricated GUID match.
If an off-current own overlay cannot provide association identity, the host
revokes registration, destroys/recreates only that hidden overlay, retires its
prepared work, and requires a fresh probe before preparing again.

Maintenance polls at most 32 round-robin lifetimes per 250 ms interval. Stable
visibility polling performs no GPU upload or yarn regeneration. Reactivation uses
the current tracker/configuration/geometry gates and can reuse prepared surfaces.
Probe output records query/failure counts, own-overlay association count, policy
revision/reason, and final visibility authorization.

Public desktop COM can pump shutdown messages. Shutdown calls `Stop` immediately
to revoke visibility policy and registrations, but retains the adapter object until
the outer Host scope unwinds. After a pumping query the host re-finds the exact
lifetime and checks stopped/current policy instead of using a stale Entry pointer.
Synthetic-policy and fake-public-manager tests exercise these consumer paths
without changing Windows desktop, lock or power state. Real OS desktop/lock/sleep
acceptance remains a separate desktop gate.

## Shared graphics recovery

Only recoverable HRESULTs from the UI-thread GPU adapter start device recovery:
`D2DERR_RECREATE_TARGET`, `DXGI_ERROR_DEVICE_REMOVED`, `DXGI_ERROR_DEVICE_RESET`,
`DXGI_ERROR_DEVICE_HUNG`, and `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. CPU/WIC worker
preparation failures remain distinct; they do not reset the shared device.

The first current-generation fault hides every border, revokes owned-overlay
registration, retires render publication/retry gates, advances the device
generation, invalidates the queue, then releases all overlay targets and the
shared device. Later per-window faults coalesce into that one recovery. Tracker
lifetimes, committed configuration and worker-owned CPU tile cache are retained.

One receiver timer attempts shared initialization at most three times with
100/500/2000 ms delays. Success rebuilds only current eligible targets through
fresh tracking, configuration, geometry and visibility checks. Pause and removed
lifetimes do not publish. Old-generation worker results are rejected even if
their former target is still alive. Exhaustion leaves borders hidden with a
bounded diagnostic and requires explicit Host restart; there is no automatic
endless retry. Shutdown revokes the attempt and timer before releasing resources.

Command `1005` (**SimulateGraphicsFailure**) is accepted only in fixture test mode.
It injects a recoverable GPU-origin failure into the same shared recovery path;
it does not reset the OS driver or run in production. Probe output reports recovery
state/error/attempt count, injection count and current device generation. Hidden
contracts reconstruct actual D3D/DComp resources, preserve identities/cache, and
exercise late work, destruction, pause/config changes, failed attempts and Stop.
These injection checks are not evidence of an actual graphics-driver reset.

The hidden native contract optionally reads the actual masked DComp upload back
from its D2D GPU bitmap. It checks square, RoundSmall and round masks at
100/125/150/200 percent DPI, continuous rounded-corner yarn, retained inward tuck,
and zero central-interior alpha. This narrow-piece staging/readback is used only
when the hidden fixture requests it; normal rendering allocates no readback bitmap.

## Checks and fixture evidence

```powershell
.\scripts\test.ps1 -Configuration Debug
.\scripts\test.ps1 -Configuration Release
```

These build native Host and Settings plus focused tracking, configuration/session,
configuration ownership, knitting and render-queue test projects. They run CLI
rejection checks and the host's `--integration-contract`. That contract includes
actual stockinette preparation on the production MTA queue and a four-surface
DComp upload/commit into a hidden overlay, plus pure token and move-reuse checks.
The actual Host action consumer also exercises stationary width changes and
Pause/Resume through the real tracker's rebuilding-Move output, then drives a
64-target burst through the production pending32 queue until every current
target publishes. Synthetic window facts keep this test hidden; publication still
uses the real worker and four-surface upload.
They never launch production tracking, Settings UI, or a visible fixture. Hidden
publication does not establish desktop presentation, cross-process WinEvents,
stacking, or input interaction.
Source-derived regressions cover nested uncertainty/minimize/destruction/DPI
changes after an accepted query, old-batch Show/Reorder after no-delta validation,
and fixture PID/process/thread/class/title changes across the startup gap.

For coordinator-controlled desktop verification, build with `-IncludeFixture`
and use the fixture's existing controls. Optional fixture-only
`--probe-output <path>` retains the original JSON fields and adds complete target
identity, visibility, creates/removes, geometry updates, draws, pure moves,
hide/show/order counts, stale actions, placement failures and tracker diagnostics.
It also records committed revision, width, pause, device generation, worker
requests/preparations, cache generations/hits, pending/ready counts, GPU uploads,
quota rejection and render errors. `color_rgb` identifies the base yarn, not a
uniform pixel-test color: knitted pixels vary.
Appearance fields, selected chart ID, and the By-app fallback flag are also
reported, so a rendered design can be distinguished from an unconsumed UI setting.
Settings diagnostics report launch-attempt count, API error, last successfully
created child PID, whether it exited, and its exit code when exited. A successful
CreateProcess call establishes process creation, not window readiness. The
receiver polls the retained child nonblockingly and logs nonzero exits; it does
not terminate Settings or restart it on failure.
Snapshots use atomic replacement and are sampled only while changed; shutdown
writes a final stopped/empty snapshot. `draws` and `gpu_uploads` count successful
four-piece publications, not tile generation or GPU memory. Pure movement must
leave those counts and worker preparation counts unchanged; settled resize must
change the publication count, and confirmed removal must clear the overlay.

Foreground pixels, live input, resizing, minimize/restore, exact native lifetime
reuse, topmost behavior and handle/resource qualification remain desktop gates.
No issue is closed by these scripted checks.
