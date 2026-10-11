# Native packaged startup API

This module supplies startup control for #16 under the Store/MSIX direction. It uses
the Windows OS `Windows.ApplicationModel.StartupTask` API, not a registry `Run`
entry, executable-path command, Windows App SDK, or WinUI dependency.
The manifest task ID is `WindowSweatersStartup`, initially `Enabled="false"`.
The current package identity is required. An unpackaged process returns
`UnavailableNoPackage`; there is no fallback or successful-disabled default.

## Consumer contract

Include `Startup.h`; namespace: `window_sweaters::startup`.

```cpp
Controller host(Consumer::HostReadOnly, committedStartupPreference);
auto submission = host.ReadState();
// Later, on a normal message/timer tick, not a WinEvent callback:
auto completion = host.TakeCompletion();
auto view = host.GetSnapshot();
```

`ReadState()` is asynchronous and observational. Construction, reads,
`SetSavedPreference()`, launch, and reconnect never request enable/disable.
Host controllers reject all mutation requests with `ReadOnly`.

In the packaged Settings process, create a separate controller with
`Consumer::SettingsUserActions`. Only an explicit user's toggle/button handler
may call:

```cpp
settings.RequestUserChange(DesiredPreference::On, UserIntent::SettingsGesture);
settings.RequestUserChange(DesiredPreference::Off, UserIntent::SettingsGesture);
```

These return `Submission`, not an OS success. Inspect `TakeCompletion()` for
`Applied`, `AlreadyEffective`, `BlockedByUser`, `BlockedByPolicy`, `NotApplied`,
`UnavailableNoPackage`, `Failed` with HRESULT, or `Cancelled`. Read success is
`Observed`. `EffectiveEnabled()` returns an optional bool; unknown/unavailable
is not false. Reads and blocked requests retain the exact five OS states.
`DisabledByUser`, `DisabledByPolicy`, and `EnabledByPolicy` never trigger a
module mutation, including disable requests. Windows owns those choices.
If Windows changes ownership during an enable request, its returned state is
reported, not replaced by the requested state.

`GetSnapshot().savedDesired` represents the consumer's last **committed**
preference. It is independent of OS effective state. The existing configuration
owner must persist explicit desired choices through its normal mutation contract
and call `SetSavedPreference(committedPreference)` after a successful commit.
There is no preference file, second store, or disk I/O in this module. OS failure,
OS user/policy blocks, cancellation, and observations do not rewrite that value.
A saved On plus effective Disabled/DisabledByUser is legitimate; never reconcile
that mismatch by auto-enabling. Config/IPC/UI integration and durable
configuration round trips are supplied by the native Settings Session and the
host's schema-6/protocol-7 mutation contract.

### Authorization and Windows semantics

Microsoft documents that packaged desktop `RequestEnableAsync()` has **no**
UWP consent dialog; the UWP UI-thread requirement does not apply to this
packaged Win32 module. Therefore a Settings gesture can initiate the request,
while native OS calls and completions run away from the UI thread. Both
executables must have identity in the same installed package; lookup uses the
manifest TaskId, never a hard-coded package family or another application's
executable. Cross-application Settings-to-Host task lookup needs packaged
integration qualification; this slice does not claim that evidence.

`Consumer` and `UserIntent` are API usage gates, not user authentication or a
security boundary. Trusted runtime code must only construct the Settings
consumer/use its gesture marker from an authorized user action. A supplied
marker does not prove a real user click. The backend seam is for the module and
isolated tests; production construction exposes no native mutation backend.
It is not a route for Host mutation. No agent test clicks authorize
real OS startup changes.

References:

- [StartupTask and packaged desktop semantics](https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.startuptask?view=winrt-26100)
- [RequestEnableAsync and user overrides](https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.startuptask.requestenableasync?view=winrt-26100)

## Scheduling and shutdown limits

Per controller there is **one** active logical operation (lookup, followed by
at most one enable request, or one synchronous OS disable on its worker).
There is no work queue. A second submission receives `Busy`; one unconsumed
completion receives `CompletionPending` backpressure. Consume that completion
before another request. Calls admitted concurrently still have one slot.
Each operation has a monotonically increasing generation; duplicate/late
callbacks cannot overwrite another generation. Do not create disposable
controllers to evade these bounds.

The native backend submits one Windows thread-pool callback and attaches
`IAsyncOperation` completions; it never waits with `.get()`, spins on a WinRT
operation, or creates an application-owned worker thread. The terminal callback
only updates a small locked snapshot/mailbox: no consumer callback, disk work,
window access, UI dispatcher, or unbounded event posting. Poll the mailbox on
normal application work, not from the window event callback. Configure the
consumer's polling cadence outside this module.
An operation-scoped `CoIncrementMTAUsage` cookie keeps the MTA alive between
lookup/enable callbacks; its matching decrement happens when the native
operation is destroyed, after its WinRT references are released.

`CancelPending()` requests cancellation once and retains the active slot until
terminal completion. Cancellation is best effort: Windows can already have
applied a change. Cancel is **not** a compensating enable/disable or rollback;
use a later explicit user action and read to resolve effective state.
An unresponsive OS operation leaves the slot busy instead of accumulating work.
`Shutdown()`/destruction rejects further work, clears pending mailbox data,
requests cancellation, and drops late results without blocking the caller.
It cannot force Windows to drain a nonresponsive operation. Detached native
operation lifetime can outlast the controller; no callback holds its raw
address. Perform normal controller shutdown before process/COM teardown, not
from `DllMain` or a process-detach callback. This does not implement Host-wide
quit/drain.

## Standalone builds

Windows 11 x64; MSBuild v145; Windows SDK `10.0.26100.0`; C++20. Build the test
project directly, without editing the solution:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe' tests\Startup\StartupTests.vcxproj /m:2 /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
& .\out\x64\Debug\StartupTests.exe
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe' tests\Startup\StartupTests.vcxproj /m:2 /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
& .\out\x64\Release\StartupTests.exe
```

The project generates only the StartupTask/Foundation projection from the SDK
union metadata with **Microsoft.Windows.CppWinRT 2.0.240405.15**. It reuses an
already-restored exact package under `src\Startup\.packages`,
`src\Settings\.packages`, or the user's NuGet cache (in that order). No Settings
project, App SDK targets, or WinUI binary is loaded. If absent, restore the
exact package locally and pass its trailing-backslash directory as
`/p:StartupCppWinRTDir=...`. The compile-time version check rejects other
projection versions. Generated files live in this project's `IntDir`.

Native consumers link `WindowSweaters.Startup.lib`, `windowsapp.lib`, and
`runtimeobject.lib` plus `ole32.lib` for the MTA lifetime APIs.
Default CRT is `/MTd` for Debug and `/MT` for Release,
matching Host. A Settings `/MD[d]` consumer must build this static library into
separate output/intermediate directories with
`StartupRuntimeLibrary=MultiThreadedDLL` or `MultiThreadedDebugDLL`; do not link
one CRT variant into another. The public header exposes no WinRT/WinUI types.

Tests use an in-memory fake startup backend and isolated fake preference store
for all mutations. The only native call under test is **unpackaged read**;
the executable refuses native qualification if package identity is present.
They cover the full state/command matrix, desired/effective separation,
allocation/OS failure, user/policy overrides, idempotent changes, cancellation,
shutdown, stale/duplicate callbacks, concurrent admission, and finite
backpressure. No real StartupTask changes, registry edits, settings UI launch,
package install, Store publication, or startup fallback is tested/performed.

Settings/Host integration and bounded TaskbarCreated tray recovery are implemented
in their respective consumers. Actual installed-package enable/disable and
startup launch remain unqualified while delivery is unsigned. Module tests never
change real Windows startup.
