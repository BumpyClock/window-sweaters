# Application owner identity

`MetadataWorker` resolves the expected root window through `QueryWindowOwnerMetadata`, not through a
PID-only identity query. This prevents legacy packaged windows sharing the system
`ApplicationFrameHost.exe` process from being merged into one application.

## Supported facts

- Read `PKEY_AppUserModel_ID` through the public `SHGetPropertyStoreForWindow` and `IPropertyStore::GetValue`
  APIs. Only bounded, validated packaged `PFN!PRAID` values become packaged identities, with
  `MetadataProvenance::WindowApplicationId`. This is source provenance, not publisher or package-registration proof.
- A matching packaged root process can supply its existing executable alias. The frame host's image
  never supplies a hosted application's alias or display name.
- An absent property permits the existing process metadata for standalone owners. The system frame
  host is recognized by its full normalized `GetSystemDirectoryW` path, never by basename. Without a
  verified window identity it returns `OwnerUnverified`, not the host executable's identity.
- Malformed, unsupported nonpackaged shell IDs, property failures, unavailable process metadata, and
  contradictory packaged root/window facts fail closed. This adapter does not guess at shell-generated
  internal IDs or inspect child windows/processes. Unsupported hosted owners remain unavailable.

Queries validate the root HWND, owning PID, complete process birth time and non-exited state before
and after metadata lookup. Thread ID and class atom must also remain stable. The process handle is
held for the native lookup. There is no public atomic HWND incarnation lease: `CompletionGate` must
still compare the current tracker generation and configuration revision before accepting a result.
No titles, window mutation, private shell APIs, icon/resource reads, injection or user-app enumeration
are used.

## Worker and snapshots

The single worker initializes COM MTA and releases it on shutdown, after query-local COM objects are
destroyed. Cache keys include the complete target token: HWND, PID, birth, tracker generation and
configuration revision. Every cache hit repeats lifetime validation and compares the original
fingerprint. Different windows in the same host process never share a metadata cache entry.
Property changes without a target lifetime change are refreshed after the existing bounded cache TTL.
Unavailable results are not cached. Custom `Query` callbacks own lookup validation; without an
explicit `LifetimeQuery`, their results are not cached.

Queue/completion count limits, cache count/charged-byte limits, and immutable results remain bounded.
A full completion queue stops dequeue; it does not discard accepted current requests. `Stop` joins
the active query and returns exact queued, completed and in-flight requests for owner-thread
`CompletionGate::Retire`. Public synchronous APIs have no cancellable hard deadline.

`BuildApplicationSnapshot` accepts different verified window identities in one root process.
Window and distinct root-process counts are computed per application. Unavailable windows can
coexist with known windows in that process. Conflicting metadata on a duplicate window lifetime,
stale revisions, and contradictory process-only identities still fail closed.

## Verification and integration boundary

Build and run `tests\Applications\ApplicationsTests.vcxproj` in Debug and Release with the existing
v145, SDK 10.0.26100.0, C++20 configuration. Deterministic tests cover hosted identities, provenance,
ambiguity, lifetime reuse, Unicode identity handling, cache validation, byte/count limits, completion
pressure, cancellation retirement and snapshot counts. Native probes use only this test executable,
its hidden root, its no-window child lifetime fixture, and invalid handles.

This is the source/test prerequisite for issue #8, not complete application preferences. Host wiring,
schema3 persistence and the native application list remain separate integration work. At landing,
release notes should describe distinct packaged host-window owners and conservative suppression of
unverified hosted owners; no Settings behavior is implemented by this module change.

Public API references:

- [SHGetPropertyStoreForWindow](https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-shgetpropertystoreforwindow)
- [System.AppUserModel.ID](https://learn.microsoft.com/windows/win32/properties/props-system-appusermodel-id)
- [GetApplicationUserModelId](https://learn.microsoft.com/windows/win32/api/appmodel/nf-appmodel-getapplicationusermodelid)
- [GetProcessTimes](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)
