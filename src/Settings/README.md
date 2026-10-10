# Native appearance settings

The native WinUI app includes pause, width, every catalogue application design,
plain/by-app/shared-Zigzag selection, stockinette/rib/garter, pattern/raised material, stitch
rows, and a native opaque-yarn `ColorPicker`. Selecting an original design loads
its authored base color; later color edits replace that base yarn without
changing the chart's contrast yarns. Selecting another design preserves width,
gauge, and retained plain stitch/material preferences.

Fresh settings and explicit reset default to raised stockinette. Valid legacy
schema-1 settings acquire that appearance default on read-only migration.
Existing schema-2 material choices, including explicit Pattern knit, stay unchanged.

Original charts use their authored material/stitches/corners/cuffs. Those plain
controls are disabled while an original/by-app/shared-Zigzag design is selected. Raised plain
material uses stockinette; returning to Pattern knit restores the retained plain
stitch. By-app preview is explicitly a **Finder example**, not an application
resolver. The host owns actual app resolution and application of these settings.
Shared Zigzag has its own persisted selection and a fixed representative-yarn
preview. The live host samples each eligible app's local executable icon.
Mapped By app owners keep their authored palettes; unmapped owners may acquire
stable icon-derived Zigzag, Picnic, or Twinkle. Shared Zigzag uses each app's
winning base yarn and derives contrast from that base. Neither representative
preview claims to show the selected application's icon or live border.

Every user edit captures **all** current appearance inputs with the exact
displayed committed configuration, server identity, and client lease. Rapid
slider/color/control changes coalesce without losing unrelated fields. Only the
host's validated durable acknowledgment changes committed settings. The UI shows
saved revision, conflict/recovery/rejection status, and actual Win32 save error.
A failed save restores the committed controls and preview; unsaved draft previews
are labelled separately.

Connection and save status appears directly below the heading, not below the
color controls. Before any authoritative snapshot, disconnected output is
labelled `Default preview · host disconnected`, never a committed revision.
After a confirmed host snapshot, a disconnect retains the last confirmed
appearance with an explicit disconnected label. Recovery defaults are also
labelled as unavailable host configuration rather than committed settings.

The production `DraftState` dispatcher reducer rejects out-of-order worker
publication sequences. Each accepted edit gets a Session generation; a queued
snapshot or earlier acknowledgment can update the separately confirmed host
appearance but cannot replace newer draft controls or advance their displayed
revision base. Only settlement or explicit discard of the current generation
allows committed controls to replace the draft. This prevents a queued plain
snapshot from undoing a new Spotify selection before a later width/color input.

Authoritative configuration confirmation is independent of mutation-error text.
A fresh admitted snapshot after host restart or client-lease expiry clears the
transient expiry status and confirms its complete configuration. A later
disconnect therefore retains the renewed host's latest appearance, not a
previous instance's values. Publication/edit/confirmation metadata is local to
Settings; protocol 5 preserves this local ordering contract while schema 4 adds
the distinct shared mode and retains persisted eligibility.

## Applications

The native Applications list uses host-owned metadata, never displayed names as
mutation keys. It includes running disabled owners, window/process counts,
supported identity keys, saved non-running choices, and non-mutable unavailable
owner diagnostics. All on/off changes the default and clears exceptions in one
host transaction. Default for new applications preserves explicit choices.
Remove saved choice restores the current default.

`AppsSession` has an independent lease and 750 ms observation worker. It does
not share the 150 ms appearance debounce or rebase commands onto refreshed
snapshots. Application commands carry the exact displayed revision and require
a typed eligibility acknowledgment. `AppsDraftState`, used by the actual GUI,
preserves a new toggle against older observations and settles only on terminal
acknowledgment/discard. Controls are disabled while an app command is pending.
No application worker launches or restarts Host. Closing Settings stops only its
two sessions and preview; Host remains running.
An authoritative Apps snapshot clears obsolete `recovery_required` after the
appearance session's explicit Preserve and reset action. The same open Apps
session and native controls become editable again; no close/reopen is required.
Relevant application conflict/storage-error status and native error codes remain
visible across successful observation polls. `AppsDraftState::CanEdit()` is the
edit gate used by the native UI and recovery regressions.

Application rows retain their native controls by stable `AppIdentity`.
Preference/metadata/acknowledgment updates change properties in place; only
membership or order changes modify the items collection. Remove saved choice
has one retained button whose visibility changes without reconstructing the
toggle. Pending commands keep those controls focusable while `CanEdit()` rejects
additional mutations and restores any rejected toggle to the current draft.
Disconnect/recovery still disables unavailable controls; saves and accessibility
remain active.

The native update captures Apps focus and the outer/inner scroll offsets,
restores the retained identity's control if an item move displaces focus, and
restores offsets after layout (clamped only to the available scroll extent).
The focus anchor records the exact toggle or Remove saved choice subcontrol.
A retained visible Remove button keeps its purpose and focus after a row move;
only a collapsed Remove button falls back to that owner's toggle.
Removing the focused saved-only row moves focus to the Apps default control,
not to an unrelated appearance selector.
It never activates a window or changes foreground ownership. Appearance
refreshes still advance the production draft's displayed CAS base, but
eligibility-only revisions do not reset selection/color/width controls or submit
another appearance preview. Genuine appearance edits update only changed
native values.

Apps presentation now owns an explicit native-layout boundary: worker snapshots
received before `Loaded`, an attached `XamlRoot`, and nonzero layout are retained
by publication sequence. A post-layout dispatcher turn applies the latest
snapshot; the Loaded/LayoutUpdated event never directly runs focus restoration
or forces another layout inside that event. Initial row population has no
existing Apps context, so it does not call FocusManager or restore viewport
offsets. Subsequent attached updates preserve the established context.

Dispatcher and Apps event callbacks record the exact operation/stage and
`hresult_error` HRESULT/message before entering a terminal, visibly reported
native-UI failure state. This is not a successful fallback: mutations are
disabled and the error remains on screen and in `settings-startup.log`.
Diagnostics distinguish focus capture, viewport lookup, row construction,
items reconciliation, property updates, focus restore and both scroll restores.
Stage tracking is allocation-free state on ordinary observations. Successful
admission and first presentation log once per App lifetime; unchanged 750 ms
Apps polls do not perform diagnostic file I/O. Typed failure logging remains
unconditional and independent of both success flags.
These records address the previously unlogged CoreMessaging callback failure;
native first-snapshot startup remains a CU check, not a reducer/build claim.

## Production preview

`Preview` is a single coalescing MTA worker with one in-flight job and one latest
pending immutable configuration. It uses `MakeDesignChart` on actual catalogue
cells/flags and `KnitRaster::RenderFrame`, which composes the real production
perimeter pieces. There is no approximate knit drawing or UI-thread raster work.
Output is fixed 1x BGRA, with 2x production supersampling, a 400×140 DIP content
rectangle, and at most 528×268 pixels. The tile cache is bounded to 16 MiB.

Completed obsolete generations are dropped on the worker and again on the
dispatcher. Weak app/dispatcher references prevent late callbacks from touching
a closed window. The UI copies completed pixels into a native `WriteableBitmap`.
Preview errors are reported separately from save errors.

`WindowSweaters.Settings.Core.vcxproj` compiles the same production native
knitting/chart/raster sources with the Settings `/MD` runtime. It has no Host or
WinUI dependency. The host's existing `/MT` renderer remains unchanged.

## Build-only validation

Use the installed MSBuild without launching or installing the app:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
foreach ($configuration in 'Debug','Release') {
    & $msbuild src\Settings\WindowSweaters.Settings.vcxproj /nologo /m /verbosity:minimal /p:Platform=x64 "/p:Configuration=$configuration"
    & $msbuild tests\Configuration\WindowSweaters.Configuration.Tests.vcxproj /nologo /m /verbosity:minimal /p:Platform=x64 "/p:Configuration=$configuration"
    & ".\out\x64\$configuration\WindowSweaters.Configuration.Tests.exe"
}
```

The test executable also accepts `--session-only`, `--appearance-only` and `--apps-only` for
focused checks. Tests exercise real authenticated IPC, failed atomic writes,
read-only migration, restart/reconnection, full appearance deduplication,
70-change chains/coalescing, and every original chart's production preview.
The apps selector covers legacy migration, typed real IPC, aggregate bounds,
durable all-off, stable identity persistence, hosted owners, saved/non-running
rows, independent observation changes, lost acknowledgments, conflicts and the
actual `AppsDraftState` reducer. Host `--integration-contract` separately runs
the real MTA metadata worker and production-render completion races in private
hidden fixtures; it does not change desktop/session/startup state.

For build-only qualification while another Host/Settings binary is active, use
isolated artifacts and per-project intermediate paths:

```powershell
$root = (Get-Location).Path
& $msbuild src\Settings\WindowSweaters.Settings.vcxproj /nologo /m /v:quiet /p:Platform=x64 /p:Configuration=Release "/p:OutDir=$root\out\issue8\x64\Release\" "/p:NativeIntermediateRoot=$root\out\issue8\obj"
```

The same properties apply to Host and configuration/integration test projects.
They keep the active application's executable and canonical `resources.pri`
unchanged. Native Apps-list CU, keyboard/accessibility and live window/identity
qualification remain separate acceptance checks; build/hidden tests do not
substitute for that gate.

### Issue #8 qualification

Independent review covers application persistence, recovery, keyed controls,
exact subcontrol focus, deferred initial presentation, and dispatcher error
reporting. Final Debug and Release Configuration and hidden Host gates pass.
Focused results in each configuration are:

| Executed artifact / selector | Result in each configuration |
| --- | --- |
| `WindowSweaters.Configuration.Tests.exe --apps-only` | 335 application persistence, protocol, observation and production Apps reducer assertions |
| `WindowSweaters.Configuration.Tests.exe --session-only` | 87 preserved appearance coalescing, own-chain, reconnect and production draft assertions |
| `WindowSweaters.ConfigurationHost.Tests.exe` | 123 ownership/binding checks |
| `WindowSweaters.Host.exe --integration-contract` | 469 actual Host consumer assertions plus the existing scripted and four-surface GPU-readback contracts |

The apps/session selectors are subsets of the full configuration suite, not
additional independent coverage. Host gates exercise app-only in-flight
retirement, unrelated queued/prepared retention, missed disable/re-enable
epochs, metadata arriving after disable/reuse, packaged helper exclusion,
same-lifetime owner-property refresh, future apps after all-off and exact Stop
retirements. Recovery regressions preserve corrupt/newer bytes during explicit
reset and restore the same open Apps session without hiding conflicts or storage
errors.

Computer Use verified the actual Settings window, per-app off/on, default-off
with an explicit enabled exception, persistence after a new PID/HWND, and no
initial overlay or render request after All-off. Subsequent off/on changes
retained the exact Remove saved choice focus and unchanged top-of-page
coordinates. Native wheel input did not establish a nonzero-viewport test.
The first-snapshot correction was checked against the actual application after
a previous candidate failed immediately after activation; passing reducer/build
checks alone had not established native startup safety.

The isolated outputs are `out\issue8\x64\Debug` and
`out\issue8\x64\Release`, with generated canonical `resources.pri` retained.
No OS startup, package installation, Store publication, or production-user
configuration was changed. See `docs\parity.md` for the native evidence and
remaining platform qualification.

App SDK 2.5.1, C++/WinRT 2.0.240405.15, and self-contained App SDK settings remain
pinned. Release retains `NDEBUG`, `WINRT_NO_SOURCE_LOCATION`, and portable
`/PDBALTPATH:%_PDB%` metadata. No UI launch, certificate operation, or Store
installation is required for these checks. Host application of all catalogue variants,
native visual verification, independent review, and Store qualification are
separate gates; these settings alone do not complete issue #7.

## Native startup and crash diagnostics

The code-only `SettingsApp` implements WinUI `IXamlMetadataProvider` and forwards
both type lookups and XML namespace definitions to
`XamlTypeInfo::XamlControlsXamlMetaDataProvider`. This supplies the control
metadata that a generated App.xaml normally provides; control resources/styles
remain enabled.

The resource dictionary is constructed and merged in `OnLaunched`, after the
code-only application finishes composition, not in the application constructor.
Startup phase records distinguish window creation, metadata initialization,
resource construction, resource merge, and appearance-control construction.

`GenerateSettingsApplicationPri` runs Windows SDK MakePRI after the native build.
It imports the three pinned, self-contained framework indexes into the canonical
application `resources.pri` beside the executable. Named runtime indexes remain
unchanged. The staging directory contains only compiled PRI inputs: it must not
also scan loose WinUI assets, which would duplicate existing indexed paths.
The result contains the actual embedded
`Files/Microsoft.UI.Xaml/Themes/themeresources.xbf` and control templates required
by WinUI's `ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml` lookup.

Unpackaged launches and the MSIX companion folder both require this generated
`resources.pri` at the application root. The packaging project must include it
byte-for-byte instead of regenerating an index from loose framework assets.

Startup logs record stage, process ID, exact HRESULT, and the WinUI exception
message through `OutputDebugString`, inherited stderr when present, and
`settings-startup.log`. The latest launch replaces this log in the normal
configuration directory (`LocalAppData\WindowSweaters`), or the supplied
`--test-mode --storage-dir` directory. Diagnostic write failure cannot mark
startup successful or suppress the exception.

The optional `--startup-probe` checks the application metadata interface and
ColorPicker/ComboBox/Slider/ToggleSwitch metadata before constructing the actual
controls. It records `window-activated` only after the real native window
activation succeeds. It also activates a minimal window before resource loading,
recorded separately as `first-window-activated`, to isolate native-window startup
from control-resource failures. This option still opens the application; only an authorized
CU/UI workflow should launch it. A successful build or a metadata source check
does not prove that the native GUI starts.
