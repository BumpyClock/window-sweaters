# Window Sweaters MSIX packaging

This Windows Application Packaging Project consumes **already built**
`out\x64\Release\WindowSweaters.Host.exe` and
`out\x64\Release\WindowSweaters.Settings.exe`. It does not rebuild, run or install
the apps. Build the native Release|x64 projects with the repository build script
first. The coordinator adds this project to the solution.

```powershell
powershell.exe -NoProfile -File .\scripts\package.ps1
powershell.exe -NoProfile -File .\scripts\package.ps1 -Bundle
```

Or use Visual Studio 2026 Enterprise MSBuild directly:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe' `
  .\packaging\WindowSweaters.Package.wapproj /t:Build /p:Configuration=Release /p:Platform=x64
```

Required tools: DesktopBridge/WAP targets, native v145 tools/redist, and Windows
SDK **10.0.26100.0** MakeAppx and Manifest Tool. The manifest targets Windows 11
desktop **10.0.22000.0** and x64 only. App SDK **2.5.1** and C++/WinRT
**2.0.240405.15** come from the Settings project's pinned native package references.
No packaging NuGet dependency is added.

## Development versus Store

The checked-in identity is **not a Store identity**:

- Name: `BumpyClock.WindowSweaters.Development`
- Publisher: `CN=WindowSweaters Development`
- Publisher display name: `Window Sweaters Development`
- Display name: `Window Sweaters (Development)`
- Version: `0.1.0.0`

Store mode requires a local MSBuild property file. Copy
`WindowSweaters.Store.props.example` to `WindowSweaters.Store.props.user`,
then replace all three identity fields with the exact values from Partner Center
**Product identity**. The `.user` file is git-ignored and contains no credentials.
Development/example identities and incomplete values are rejected.

```powershell
powershell.exe -NoProfile -File .\scripts\package.ps1 -Channel Store `
  -StoreIdentityProps .\packaging\WindowSweaters.Store.props.user -Version 0.1.0.0
```

Direct MSBuild properties: `PackageChannel`, `StoreIdentityProps`,
`PackageVersion`. `StoreIdentityProps` is an
absolute path when supplied to MSBuild directly. Store mode still creates an
**unsigned artifact**, not a certified upload or publication. This project does
not create `.msixupload` files or claim they are Store-ready.

## Manifest and runtime contract

Settings is the primary Start menu entry. Host has `AppListEntry="none"`.
Both apps are classic packaged processes with `uap10:RuntimeBehavior=
"packagedClassicApp"` and `uap10:TrustLevel="mediumIL"`. The desktop startup
extension belongs to Host, uses **TaskId** `WindowSweatersStartup` (not `Id`),
and starts with `Enabled="false"`. Enabling/disabling startup is the app's
separate consent flow; users can also disable it in Windows startup settings.
Both executable files remain siblings at package root so companion launch can
use the known package folder. Embedded PMv2/asInvoker manifests are verified.
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
The native Settings build must also produce its merged application index
`out\x64\Release\resources.pri`; named framework PRI files alone do not satisfy
the app's `ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml` lookup.
Packaging includes this explicit generated output at package root, keeps its
hash unchanged, and rejects missing or concurrently changing resource output.
WAP must not regenerate or merge this application PRI.
The package verifier uses SDK MakePRI to inspect the packaged index and requires
both embedded `Microsoft.UI.Xaml/Themes/themeresources.xbf` and `generic.xbf`.
This is resource-content validation, not proof that installed WinUI startup or
theme changes succeed; those require the separately authorized UI check.

## Artifacts, validation and licensing

`packaging\artifacts\Development` (or `Store`) holds unsigned `.msix`, optional
`.msixbundle`, corresponding-source `.zip`, `payload-report.json` and
`validation-report.json`. WAP may nest the MSIX under its version folder.
`packaging\obj\inspection` contains the validated unpacked package.
MakeAppx validation remains enabled. The verifier checks identity, minimum OS,
architecture, sibling exes, app visibility, disabled startup, capability and
framework dependency lists, unsigned status, exact payload hashes, and excludes
unexpected content.

Original neutral yarn artwork is generated from fixed vector geometry in
`Generate-Assets.ps1`: 44×44, 50×50 StoreLogo, 71×71, 150×150, 310×150 and
310×310 PNGs. No fonts, third-party marks, catalogue plates or AI promotional
images are used. Rerun the generator to reproduce them.

The package includes `LICENSE`, `NOTICE.md` and `SOURCE.txt`. Its accompanying
source ZIP snapshots the current repository source, including uncommitted
implementation, tests, build and package files; excluded local/generated files
are not included. `SOURCE.txt` records that archive's SHA256. Keep the exact
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
  -SourceZip .\packaging\artifacts\Development\WindowSweaters_0.1.0.0_Development_Source.zip
```

The payload report records the original prebuilt application hashes. Packaging
fails if either native EXE changes while its package is being prepared. Wait
for integration builds to finish before invoking packaging.

**Not verified:** signed installation, startup enable/disable on an installed
package, a clean Windows 11 22000 machine, WACK/Store certification or publication.
No certificate, signing, trust insertion, installation, Store upload or
submission is performed. Those steps need separate explicit authorization and
the real Partner Center identity.

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
the optional `-Bundle` command instead uses SDK MakeAppx to bundle the validated
x64 MSIX without resource splitting. It unbundles the result and verifies the
inner MSIX hash. WAP remains responsible for building the primary MSIX.
