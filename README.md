# Window Sweaters for Windows

A native Windows port of [Window Sweaters](https://github.com/saragordic/window-sweaters), which draws knitted borders around application windows.

Implementation is in progress under [issue #1](https://github.com/BumpyClock/window-sweaters/issues/1). This repository is not yet a release-qualified application. The [parity ledger](docs/parity.md) separates implemented code from desktop and distribution evidence.

Microsoft Store distribution is now the target. MSIX packaging is in progress.
Until Partner Center identity values are supplied, packages use an explicitly
marked development identity and are not Store submissions. Package creation,
signing, installation, and publication are separate operations.

## Platform

The target is Windows 11 x64 and ARM64, build 22000 or later. The tray host uses C++20, Win32, Direct2D, and DirectComposition. Portable knitting code remains C where practical. Settings use an on-demand C++/WinRT WinUI 3 process; the host does not load WinUI or the Windows App SDK.

Native projects use Visual Studio/MSBuild, not CMake. Install Visual Studio 2026 with the Desktop development with C++ workload and Windows SDK 10.0.26100.0. Projects pin the v145 platform toolset, C++20, and C17. Settings target the stable Windows App SDK 2.5.1.

## Build and run

From PowerShell at the repository root:

```powershell
.\scripts\build.ps1 -Configuration Debug -IncludeFixture
.\scripts\build.ps1 -Configuration Release -IncludeFixture
.\scripts\build.ps1 -Configuration Release -Platform ARM64
.\scripts\test.ps1 -Configuration Debug -NoBuild
```

The build script accepts `-MSBuild <path>` for other Visual Studio editions or installation locations. Native artifacts are written to `out\<Platform>\<Configuration>`. ARM64 compilation requires the corresponding v145 toolchain. Cross-compilation does not prove execution on ARM64 hardware.

After both Release builds, `.\scripts\package.ps1 -Architectures x64,ARM64 -Bundle`
produces an unsigned development bundle. It validates matching architecture
payloads and source/build receipts. See [packaging](packaging/README.md) for
Partner Center identity setup and Visual Studio F5/Ctrl+F5 of
`WindowSweaters.Package`. Signing, certificate trust, installation and
Store submission remain separate operations.

To iterate on chart designs, `python scripts\preview-designs.py` renders the Windows-authored themes as flat labelled strips to `out\design-preview.png`. Pass theme IDs to select themes, or `--all` for the whole catalogue. It shows chart cells only; the Knitting tests write knitted renders to `tests\Knitting\outputs`.

For Computer Use verification, `.\scripts\test.ps1 -Desktop` builds the separate-process fixture and prints its executable path. It does not launch or interact with desktop applications. Launch `Fixture.exe` through Computer Use, then use the fixture's **Start test host** control. The host receives only that fixture's HWND in test mode. Stop it through **Stop test host** or the host tray's **Quit** command.

The host now tracks ordinary windows through WinEvents and bounded reconciliation.
Run `WindowSweaters.Host.exe` without arguments for production discovery.
Fixture mode requires an explicitly selected fixture and seals its initial
identity; it must not select other desktop applications. Pure movement reuses
decoration, actual resize suppresses it until settlement, and hidden/minimized
windows can recover without losing their lifetime.

Open Settings from the tray and select **By app** for authored Windows mappings
and patterns generated from each other app's icon, or **Shared Zigzag** for one motif with per-app colors.
Manual original designs remain selectable. Settings saves choices through the
host; closing Settings leaves decoration running.

To use one base color everywhere, turn on **Use my base color for every sweater**
and pick a color. Every sweater then knits on that base. Each design's other yarns
shift to suit it, so the pattern stays readable and keeps the app's character.
Imported PNG charts keep their exact pixels, and a precise application color from
personal designs still wins. Turn the switch off to return to each app's own colors.

Exterior borders are intentionally hidden for minimized, maximized, fullscreen,
off-desktop, locked, and uncertain targets. Restore a window to ordinary size to
see its sweater. Elevated (higher-integrity) windows are not decorated because Windows blocks z-ordering against them. An isolated `--preview-target` launch decorates only its selected
lifetimes, not every application; it is separate from normal no-argument launch.

Native knitting, settings, exclusions, and app resolution are connected and have
desktop evidence in the parity ledger. Physical input/desktop transitions,
aggregate compositor budgets, and Store-installed execution remain separate
qualification gates.

## Safety and scope

Decorations use windows owned by this application. The port must not inject code, change another application's window styles, move its windows, or require administrator privileges. Desktop probes must be opt-in and restricted to fixtures or explicitly inspected preview lifetimes.

Startup at sign-in is opt-in. Diagnostics remain local. Window titles, captured application content, telemetry uploads, and arbitrary startup commands are outside the design.

## Upstream and license

The reference baseline is [`a49dff52483dafdbe27f972bd271cd28d37cef1c`](https://github.com/saragordic/window-sweaters/tree/a49dff52483dafdbe27f972bd271cd28d37cef1c), reviewed on October 9, 2026. The author has authorized this Windows port.

This repository retains the GNU GPL v3 license in [LICENSE](LICENSE). Imported or adapted upstream material must retain its attribution and applicable original/JankyBorders notices. The local reference checkout is not part of the distribution.
