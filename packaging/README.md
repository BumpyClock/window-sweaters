# Window Sweaters MSIX packaging

This Windows Application Packaging Project consumes
`out\x64\Release\WindowSweaters.Host.exe` and
`out\<Platform>\Release\WindowSweaters.Settings.exe`, where Platform is `x64`
or `ARM64`.

Command-line MSBuild and `scripts\package.ps1` use those prebuilt Release
artifacts. They do not rebuild, run, or install the apps. Build each native
architecture with the repository script first.

```powershell
.\scripts\build.ps1 -Configuration Release -Platform x64
.\scripts\build.ps1 -Configuration Release -Platform ARM64
.\scripts\package.ps1 -Version 0.2.0.0 -Architectures x64,ARM64 -Bundle
```

Or use Visual Studio 2026 Enterprise MSBuild directly:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe' `
  .\packaging\WindowSweaters.Package.wapproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:AppxBundle=Never
```

That command-line WAP build still skips launch (`DoNotLaunchApp` is true
outside Visual Studio).

Required tools: DesktopBridge/WAP targets, native v145 tools/redist, and Windows
SDK **10.0.26100.0** MakeAppx and Manifest Tool. The manifest targets Windows 11
desktop **10.0.22000.0**, with native x64 and ARM64 payloads. App SDK **2.5.1** and C++/WinRT
**2.0.240405.15** come from the Settings project's pinned native package references.
No packaging NuGet dependency is added.

## Visual Studio startup

`WindowSweaters.Package` is the first project in `WindowSweaters.sln`, so a
fresh Visual Studio open treats it as the startup project. If this machine
already saved a different startup project, right-click
`WindowSweaters.Package` and choose **Set as Startup Project** once. The
current Visual Studio session has that startup project set.

The packaging project is Release-only. Solution `Debug|x64` and `Debug|ARM64`
map it to `Release|x64` and `Release|ARM64`. That mapping is intentional; it is
not a missing Debug configuration. Build and Deploy are enabled for those
mappings so F5 and Ctrl+F5 (**Start Without Debugging**) can build, deploy, and
launch the package. After a solution reload, the mapping warning was gone.
Configuration Manager shows solution `Debug|x64` mapping the WAP project to
`Release|x64` with Build and Deploy checked. An amd64 MSBuild package build
completed successfully.

The WAP project has no redundant solution project dependencies. Inside Visual
Studio, `PrepareWindowSweatersPayload` first runs
`scripts\build.ps1 -Configuration Release` for the active platform to refresh
the native build receipt, then stages the package. That script owns the native
Release builds. Visual Studio does not set `DoNotLaunchApp`, so F5/Ctrl+F5
start Settings (`EntryPointExe` is `WindowSweaters.Settings.exe`).

Settings references `WindowSweaters.Startup.Settings.vcxproj`, a distinct
project GUID that imports the shared startup sources with `/MD` and `/MDd`.
Host retains `WindowSweaters.Startup.vcxproj` with `/MT` and `/MTd`. That
split resolved the missing
`Startup\MultiThreadedDebugDLL\WindowSweaters.Startup.lib` link failure.

F5/Ctrl+F5 deploy uses Windows Developer Mode and local package deployment.
The user authorized that local Visual Studio development registration. It does
not sign, trust a certificate, change OS settings, or prove Store-installed
execution. It registered the x64 Development `0.1.0.0` layout, not the
unsigned x64/ARM64 `0.2.0.0` delivery bundle.

F5 then deployed and launched x64 Development `0.1.0.0` Settings. The debugger
first paused on thrown `winrt::hresult_error` because **Break when this
exception type is thrown** was checked. Continuing past those first-chance
stops produced a responsive Settings UI connected to Host.
`settings-startup.log` recorded successful `window-activated` and
`apps-presentation-ready` stages. Throw sites were not identified; symbols
were missing. This records handled first-chance stops, not that every
`hresult_error` is harmless. The local checkbox was then unchecked for
`winrt::hresult_error` only. Production exception handling was not changed.
That uncheck does not hide unhandled errors and is not a reason to disable
other first-chance breaks.

After Stop Debugging, Visual Studio **Start Without Debugging** launched a
fresh packaged Settings process with the same responsive Connected UI. Both
launch paths were verified.

## Visual Studio bundles

Visual Studio **Publish > Create App Packages** can request **Generate app
bundle: Always** with **Release x64** and **Release ARM64** selected.
`AppxBundle=Always` routes bundle creation through the same validated MakeAppx
path as `scripts\package.ps1 -Bundle`, instead of WAP's PRI-splitting pipeline.
The selected `AppxBundlePlatforms` must be `x64`, `ARM64`, or both separated
by `|`; duplicate and unsupported architectures are rejected.

The bundle build refreshes native Release outputs for every selected
architecture, builds and validates each unsigned WAP MSIX, then bundles those
MSIX files without changing their bytes. The final bundle path is reported
to Visual Studio through its packaging information file and build output.
Artifacts remain under `packaging\artifacts\<PackageChannel>\<PackageVersion>`.
Use `AppxBundle=Never` for a single-platform MSIX and the normal development
build path. A saved `Always` setting also creates a bundle on normal package
builds; it is not limited to the publish wizard.

`PackageChannel`, `PackageVersion`, and `StoreIdentityProps` remain the
project's identity/version inputs. If the wizard sets
`UapAppxPackageBuildMode` to `StoreUpload` or `StoreAndSideload` and
`PackageChannel` is unset, WAP now selects Store. It previously kept the
Development default, so Store-mode packages still used checked-in
development metadata. An explicit Development channel with Store
publishing mode is rejected. `scripts\package.ps1` still defaults to
Development unless `-Channel Store`. Bundle creation does not enable
signing, installation, Store association, upload, or publication. Store
builds still require the local Partner Center identity file. Selecting
wizard options that enable signing is rejected rather than silently
producing a different artifact. ARM64 packaging validation does not
establish ARM64 execution.

## Development versus Store

The checked-in identity is **not a Store identity**:

- Name: `BumpyClock.WindowSweaters.Development`
- Publisher: `CN=WindowSweaters Development`
- Publisher display name: `Window Sweaters Development`
- Display name: `Window Sweaters (Development)`
- Version: `0.1.0.0`

The Store wizard previously generated that development metadata because
`PackageChannel` stayed at its Development default while publishing mode
was `StoreUpload` or `StoreAndSideload`. Store rejected PublisherDisplayName
`Window Sweaters Development` because it does not match `Weakly Design`.
WAP now selects Store for those publishing modes unless `PackageChannel`
is already set, and rejects Development plus Store publishing mode. The
CLI still defaults to Development unless `-Channel Store`.

Store mode requires a local MSBuild property file. Copy
`WindowSweaters.Store.props.example` to `WindowSweaters.Store.props.user`,
then set `PackageName`, `PackagePublisher`, and
`PackagePublisherDisplayName` to the exact Partner Center **Product
identity** values. Use `Weakly Design` for the publisher display name.
Do not invent `PackageName` or `PackagePublisher`; copy those from
Partner Center. The `.user` file is git-ignored and contains no
credentials. Development/example identities and incomplete values are
rejected.

Previously uploaded packages used the development identity and a hidden
Host Application. Regenerate both architectures and a bundle in Store
mode after building native Release outputs:

```powershell
.\scripts\build.ps1 -Configuration Release -Platform x64
.\scripts\build.ps1 -Configuration Release -Platform ARM64
.\scripts\package.ps1 -Channel Store `
  -StoreIdentityProps .\packaging\WindowSweaters.Store.props.user `
  -Version 0.1.0.0 -Architectures x64,ARM64 -Bundle
```

Unsigned Store artifacts are written to
`packaging\artifacts\Store\<Version>`. Direct MSBuild properties:
`PackageChannel`, `StoreIdentityProps`, `PackageVersion`.
`StoreIdentityProps` is an absolute path when supplied to MSBuild
directly. WAP Store builds emit per-architecture `.msixupload` files
alongside each MSIX. The MakeAppx bundler produces the combined
`.msixbundle`, not a combined upload wrapper. Artifact creation is not
Store certification or publication.

## Manifest and runtime contract

Settings is the only Application and stays visible (`AppListEntry`
default). The package does not declare a second Host Application. Store
certification requires a HeadlessAppBypass waiver for
`AppListEntry="none"`, which is why the previously uploaded x64 and ARM64
MSIX packages were rejected. Do not restore a hidden Host Application, set
`AppListEntry="none"` on any Application, or add Host as a second Start
shortcut. `WindowSweaters.Host.exe` remains a sibling payload at package
root so companion launch can use the known package folder. Settings is a classic packaged process with
`uap10:RuntimeBehavior="packagedClassicApp"` and
`uap10:TrustLevel="mediumIL"`. The desktop `windows.startupTask`
extension is nested under Settings, uses **TaskId** `WindowSweatersStartup`
(not `Id`), `Executable="WindowSweaters.Host.exe"`, and starts with
`Enabled="false"`. Package generation writes the same display name on
Settings and that startup task. Enabling/disabling startup is the app's
separate consent flow; users can also disable it in Windows startup
settings. Embedded PMv2/asInvoker manifests are verified.
Only staged copies of the two unsigned application EXEs have their CodeView
PDB paths reduced to basenames; original build outputs are not modified.
Other private paths cause a build failure rather than a code/literal rewrite.
Native Release Settings must use `WINRT_NO_SOURCE_LOCATION` to prevent C++/WinRT
source-location diagnostics from embedding absolute generated-header paths.
The packager checks for remaining private home paths and refuses signed input
EXEs rather than invalidate a signature. Embedded manifests and code are retained.

`runFullTrust` is the only restricted capability. The app draws its own overlay
windows and observes desktop window metadata as the current user. It does not
need administrator rights, UIAccess, process injection, networking, services,
drivers or a broad file-system capability. `runFullTrust` needs a justification
and approval in Partner Center; this manifest does not imply approval.

Host retains its native **static CRT**, without WinUI. Settings retains its
existing dynamic CRT: the package includes the **app-local v145 redistributable**
instead of relying on a machine-installed Visual C++ redistributable. Its App SDK
runtime files come only from the project's evaluated self-contained payload
items, including PRI files, satellite resources and required WinUI assets.
Each must already exist in the project's output and match the pinned dependency
hash. WinMD design metadata, test/fixture executables, PDBs, private user files
and probe logs are not copied. PE architecture/imports are inspected; OS import
availability is measured on the build machine, not a clean minimum-OS machine.
Every application and runtime binary must match its package's PE architecture:
x64 `0x8664` or ARM64 `0xAA64`. Settings includes the matching app-local CRT.
ARM64 cross-compilation on an x64 host does not establish ARM64 execution.
The native Settings build must also produce its merged application index
`out\<Platform>\Release\resources.pri`; named framework PRI files alone do not satisfy
the app's `ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml` lookup.
Packaging includes this explicit generated output at package root, keeps its
hash unchanged, and rejects missing or concurrently changing resource output.
WAP must not regenerate or merge this application PRI.
The package verifier uses SDK MakePRI to inspect the packaged index and requires
both embedded `Microsoft.UI.Xaml/Themes/themeresources.xbf` and `generic.xbf`.
This is resource-content validation, not proof that installed WinUI startup or
theme changes succeed; those require the separately authorized UI check.

## Artifacts, validation and licensing

`packaging\artifacts\<Channel>\<Version>\<Platform>` holds each architecture's
unsigned `.msix`, corresponding-source `.zip`, `payload-report.json` and
`validation-report.json`. WAP may nest its MSIX inside that platform folder.
The combined `.msixbundle` and its validation report sit in the version folder.
Older versions remain separate and are never bundle inputs.

The development delivery artifact is
`packaging\artifacts\Development\0.2.0.0\WindowSweaters.Package_0.2.0.0_arm64_x64.msixbundle`.
It is unsigned and has no installed-package qualification. Both architecture
payloads validate against their source/build receipts; source archives are
identical and reproducible without Git. Previously uploaded Store packages
are obsolete: they used development identity and a hidden Host
Application. Regenerate Store-channel artifacts at
`packaging\artifacts\Store\<Version>` with the Partner Center identity
file. This does not upload, certify, or publish.
`packaging\obj\inspection` contains the validated unpacked package.
MakeAppx validation remains enabled. The verifier checks identity, minimum OS,
architecture, sibling exes, app visibility, disabled startup, capability and
framework dependency lists, unsigned status, exact payload hashes, and excludes
unexpected content.

Original neutral yarn artwork is generated from fixed vector geometry in
`Generate-Assets.ps1`: 44×44, 50×50 StoreLogo, 71×71, 150×150, 310×150 and
310×310 PNGs, plus `WindowSweaters.ico` with 16, 20, 24, 32, 40, 48, 64 and
256 px PNG frames from the same yarn-ball drawing. Regenerating the ICO leaves
those PNG bytes unchanged. Packaging consumes the checked-in assets without
regenerating them; asset authoring must finish before native builds record
their source hashes. No fonts, third-party marks, catalogue plates or AI
promotional images are used. Rerun the generator to reproduce them. Both native
executables embed that shared ICO as resource 101 via `src\AppIcon.rc`. Host
tray loads the embedded small icon with `LR_SHARED` instead of `IDI_APPLICATION`.

The package includes `LICENSE`, `NOTICE.md` and `SOURCE.txt`. Its accompanying
source ZIP snapshots the current repository source, including uncommitted
implementation, tests, build and package files; excluded local/generated files
are not included. Those snapshots include uncommitted additions and edits and
exclude working-tree deletions. `SOURCE.txt` records that archive's SHA256. Keep the exact
archive available to recipients when redistributing. Review GPLv3 and Store
distribution terms before publication; restore upstream dependencies from their
pinned manifests and retain their own license/notice requirements.
Each ZIP includes `packaging\SourceInventory.json` with the exact source paths
and SHA256 hashes. Extracted recipients need no Git installation or `.git`
metadata: packaging reads this inventory, reapplies the same source exclusions,
and rejects unlisted source files instead of copying the directory blindly.
Edits to inventoried source files are permitted for a recipient's new build and
produce updated hashes; added files require an explicit inventory update.
The package verifier extracts the ZIP, reruns its own inventorier without Git,
checks every original hash, rejects a planted unexpected file, and proves
restored/generated `out` content remains excluded. With an empty `PATH` (no Git
executable available), the extracted recipient archiver must reproduce the
source ZIP byte-for-byte. Run the source-only check independently:

```powershell
.\packaging\Test-SourceArchive.ps1 `
  -SourceZip .\packaging\artifacts\Development\0.2.0.0\x64\WindowSweaters_0.2.0.0_Development_Source.zip
```

The payload report records the original prebuilt application hashes. Packaging
fails if either native EXE changes while its package is being prepared. Wait
for integration builds to finish before invoking packaging.
Each Release build records source and output hashes in `native-build-report.json`.
Package preparation refuses a missing receipt, changed source, changed output,
or mismatched platform. `-NativeOutputRoot` selects isolated outputs under
repository `out`; pass the same root to both build and package commands.

**Not verified:** signed installation, ARM64 runtime execution, startup enable/disable on an installed
package, a clean Windows 11 22000 machine, WACK/Store certification or publication.
The unsigned Development `0.2.0.0` x64/ARM64 bundle remains unsigned and is not
signed-installation or ARM64-runtime qualified. Visual Studio F5/Ctrl+F5
registered a separate local x64 Development `0.1.0.0` layout after explicit
user approval of local Visual Studio development package deployment. No
certificate, signing, trust insertion, Store upload, or submission was
performed. OS settings were not changed. Partner Center identity remains
deferred.

## Authoritative schema references

- [Application execution model](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-f-application)
- [VisualElements and AppListEntry](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-uap-visualelements)
- [desktop:StartupTask](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-desktop-startuptask)
- [Self-contained Windows App SDK deployment](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps)

Schema validation is pinned to the installed Windows SDK 26100 MakeAppx/WAP
toolchain rather than a downloaded or guessed package schema.

The installed WAP pipeline attempts to merge runtime PRI indexes and loose
WinUI resources, causing measured duplicate-resource errors. This project
disables project PRI generation and preserves the App SDK PRI files as explicit
payload. WAP's native bundle splitting also requires that generated project PRI;
both Visual Studio `AppxBundle=Always` and the `-Bundle` command instead use SDK MakeAppx to bundle independently validated
architecture-specific MSIX files without resource splitting. It unbundles the
result and verifies every inner MSIX hash, architecture, identity, and version.
WAP remains responsible for building each primary MSIX.
