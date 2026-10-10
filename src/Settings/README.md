# Native appearance settings

The native WinUI app includes pause, width, all 37 original application designs,
plain/by-app selection, stockinette/rib/garter, pattern/raised material, stitch
rows, and a native opaque-yarn `ColorPicker`. Selecting an original design loads
its authored base color; later color edits replace that base yarn without
changing the chart's contrast yarns. Selecting another design preserves width,
gauge, and retained plain stitch/material preferences.

Fresh settings and explicit reset default to raised stockinette. Valid legacy
schema-1 settings acquire that appearance default on read-only migration.
Existing schema-2 material choices, including explicit Pattern knit, stay unchanged.

Original charts use their authored material/stitches/corners/cuffs. Those plain
controls are disabled while an original/by-app design is selected. Raised plain
material uses stockinette; returning to Pattern knit restores the retained plain
stitch. By-app preview is explicitly a **Finder example**, not an application
resolver. The host owns actual app resolution and application of these settings.

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
Settings; the wire protocol and persisted schema are unchanged.

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

The test executable also accepts `--session-only` and `--appearance-only` for
focused checks. Tests exercise real authenticated IPC, failed atomic writes,
read-only migration, restart/reconnection, full appearance deduplication,
70-change chains/coalescing, and every original chart's production preview.

App SDK 2.5.1, C++/WinRT 2.0.240405.15, and self-contained App SDK settings remain
pinned. Release retains `NDEBUG`, `WINRT_NO_SOURCE_LOCATION`, and portable
`/PDBALTPATH:%_PDB%` metadata. No UI launch, certificate operation, or Store
installation is required for these checks. Host application of all 37 variants,
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
