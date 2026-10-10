# Window Sweaters for Windows

A native Windows port of [Window Sweaters](https://github.com/saragordic/window-sweaters), which draws knitted borders around application windows.

Implementation is in progress under [issue #1](https://github.com/BumpyClock/window-sweaters/issues/1). This repository is not yet a release-qualified application. The [parity ledger](docs/parity.md) separates implemented code from desktop and distribution evidence.

## Platform

The target is Windows 11 x64, build 22000 or later. The tray host uses C++20, Win32, Direct2D, and DirectComposition. Portable knitting code remains C where practical. Settings use an on-demand C++/WinRT WinUI 3 process; the host does not load WinUI or the Windows App SDK.

Native projects use Visual Studio/MSBuild, not CMake. The implementation toolchain is Visual Studio 2026, the v145 platform toolset, and Windows SDK 10.0.26100.0. Settings target the stable Windows App SDK 2.5.1. Build and execution commands will be documented with each verified slice.

## Safety and scope

Decorations use windows owned by this application. The port must not inject code, change another application's window styles, move its windows, or require administrator privileges. Desktop probes must be opt-in and restricted to test fixtures.

Startup at sign-in is opt-in. Diagnostics remain local. Window titles, captured application content, telemetry uploads, and arbitrary startup commands are outside the design.

## Upstream and license

The reference baseline is [`a49dff52483dafdbe27f972bd271cd28d37cef1c`](https://github.com/saragordic/window-sweaters/tree/a49dff52483dafdbe27f972bd271cd28d37cef1c), reviewed on October 9, 2026. The author has authorized this Windows port.

This repository retains the GNU GPL v3 license in [LICENSE](LICENSE). Imported or adapted upstream material must retain its attribution and applicable original/JankyBorders notices. The local reference checkout is not part of the distribution.
