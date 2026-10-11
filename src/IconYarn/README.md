# IconYarn preparation contract

This module supplies portable sampling and bounded preparation for issue #10.
The Host integrates its completion gate and worker with validated owner metadata,
the shared resolver, and native knitting publication. Settings exposes a distinct
persisted Shared Zigzag mode. Native acceptance and independent integration review
are recorded separately in `docs\parity.md`.
Portable consumers compile `IconPalette.cpp` and `Preparation.cpp` with C++20.
Windows consumers add `WindowsIcon.cpp` and link `user32.lib`, `gdi32.lib`,
`ole32.lib` and `windowscodecs.lib`; the standalone test project supplies these.

## Palette boundary

`IconPalette.h` has no Windows dependency. `SamplePalette` accepts exactly 2,304
top-down, 48x48 sRGB `PremultipliedRgba8` pixels. Channels are bytes in R,G,B,A order.
They are premultiplied in sRGB, not linear-light space. Invalid size or RGB greater
than alpha returns `Fallback{InvalidPixels}`. Input and output contain no handles.

The mathematics follows `src/autoyarn.m:63-159` at upstream revision
`a49dff52483dafdbe27f972bd271cd28d37cef1c`, under the repository GPL-3.0 license:

* Skip alpha below 217, unpremultiply by dividing RGB bytes by alpha.
* Reject HSV saturation below .18, brightness below .12, or channel range below .10.
* Require at least 38 qualifying pixels. Count floor-quantized RGB in 4,096 bins.
* Pick up to three dominant bins, with deterministic ascending-bin tie breaks.
  Candidates must differ by at least .06 circular hue or .20 HSL lightness.
* Soften HSL saturation to `clamp(s*.48,.22,.60)` and lightness to
  `clamp(l*.85+.10,.30,.72)`. Truncate output channels, as upstream does.
* Select an accent with raw lightness separation of at least .20, then recheck
  actual softened byte colors. If insufficient, derive `ZigzagContrast(base)`.
  It is `0xfff6f0de` below lightness .62, otherwise the same hue/saturation at L-.26.

`PaletteResult` is `variant<SampledPalette,Fallback>`. A successful palette contains
opaque `baseArgb` and `contrastArgb` in `0xffRRGGBB` order. Transparent, monochrome,
faint-gray and insufficient-coverage inputs return an explicit reason, not empty
colors. There is deliberately no duplicate fallback basket table here.

`StableMotif` applies case-sensitive 32-bit FNV-1a to the complete supplied identity
bytes, modulo `zigzag`, `picnic`, `twinkle`. The Host must supply a persistent,
canonical, kind-prefixed UTF-8 identity. `AppIdentity::Kind()+Key()` must be converted
at the Host boundary: executable identity equality is Windows ordinal
case-insensitive, while AUMID equality is exact. Equivalent executable identities
need one stable canonical byte spelling before use here. Raw Key alone, PID, HWND,
window titles, display names and basenames are not valid persistent salts.

## Resolver interoperability and precedence

The Host translates successful sampling to
`colorways::AutomaticAppearance{base_argb, contrast_argb, chart_name}`:
copy `baseArgb`, copy `contrastArgb` for By App, and use
`string(MotifName(StableMotif(canonicalAppIdentity)))` as the generated built-in motif.
On fallback or while pending, omit the automatic proposal and let the resolver
provide its existing stable basket/authored provisional result.

The resolver, not this worker, applies explicit personal colors, manual selections,
authored By App mapping, mode semantics and personal PNG precedence. Sampling must
never directly mutate a border or recolor a semantic personal PNG asset. Shared
Zigzag derives its contrast from the winning main yarn, not the sample's secondary
accent; explicit personal contrast remains the resolver's highest-priority override.
Authored By App charts keep their literal authored colors.

## Preparation ownership

`Preparation.h` is portable. One Host owner creates one `CompletionGate` and one
`PreparationWorker`. The native query is `SampleExecutableIcon`; tests can supply a
local decoder honoring the same stop/deadline boundary.

1. Create `IconInput{canonicalAppIdentity, iconRevision, executablePath}` from
   authoritative metadata. `executablePath` is a transient, trusted full local
   image path. Packaged metadata aliases are not full paths; no source means fallback.
   Identity is at most 4,096 bytes and source at most 4,096 wide characters.
2. Call `gate.Begin(WorkToken{targetId,processBirth,targetLifetime,
   appearanceRevision,deviceGeneration}, input)`. Target/lifetime/process birth and
   device generation must be nonzero. Tokens and the shared input are immutable;
   they hold no raw border pointer. Begin supersedes that target's prior sequence.
3. `worker.Submit(request)` is nonblocking with respect to decoding.
   `Accepted`, `Deduplicated` or `Cached` mean there is one outstanding result for
   this target/sequence. `Full`, `Invalid` and `Stopped` require forgetting the gate
   request or a later bounded, explicit targeted retry. They are not palette results.
4. Drain completions on the owner thread and call `gate.Accept(completion,currentToken)`
   against fresh authoritative lifetime, appearance revision and device generation.
   Only then may the Host run the resolver and invalidate that target's existing yarn.
   Accept is one-shot. A stale result cannot recreate or recolor a border.
5. On destruction/exclusion call both `gate.Forget(targetId)` and
   `worker.Forget(targetId)`. Mode/device changes clear or supersede the gate and
   cancel affected subscriptions. An already-running CPU query may finish/cache,
   but has no callback and cannot apply itself.
6. Stop accepting requests, clear the gate, and `worker.Stop()` before teardown.
   Stop is owner-only, is idempotent, cancels queued work and joins workers.

The immutable cache key is the complete identity, icon revision and source path,
not process lifetime or appearance/device revision. Relaunch can reuse CPU colors;
the complete target token still guards acceptance. A changed icon/source requires
an incremented icon revision. Positive and negative results are cached identically.
A retry after temporary unavailability is an explicit icon revision change, not a
timer or automatic redraw-driven resubmission.
The current Host does not watch files for same-path icon resource replacement.
An executable-source path change invalidates the owner's sample; replacement
at the same path is detected only after Host restart.

Defaults: two workers, 32 pending jobs, 128 total subscriptions including completed
results, 256 KiB charged pending bytes, 64 cache entries and 256 KiB charged cache
bytes. Running empty jobs retain their charge. Cache eviction never invokes a
callback or schedules another job. Shared immutable results remain valid after
eviction. Charges include retained string capacities and conservative ownership/node
allowances, not allocator/process-wide memory. The bounded gate is separate
(512 targets by default); consumers retaining drained results own those references.

## Native source and deadline limits

`WindowsIcon.h` accepts only a trusted executable resource or a process-owned HICON.
There are no foreign window messages, shell-extension calls, screenshot APIs or
application-content capture. The executable is read as PE data, never loaded as code.

Packaged apps usually keep their icon as PNG assets, not in the executable. When the
PE icon is missing or unusable, the reader looks for `AppxManifest.xml` in the
executable's folder and up to three parents. It takes the `Square44x44Logo` of the
`Application` whose `Executable` matches, or else the package `<Logo>`, and picks the
`altform-unplated` variant nearest 48px. It skips `altform-lightunplated` and
`contrast-*` variants because they recolour the brand. The manifest (1 MiB) and the
PNG (256 KiB, 256x256) use the same path, reparse and size rules as the executable.
Only the system PNG decoder is used.

The native reader restricts paths to fixed local drives, rejects reparse paths,
verifies the opened final path, rejects DOS device names, and bounds source files to
64 MiB. PE directory storage is capped at 1 MiB, sections at 96, each traversed
directory at 1,024 entries, group icons at 256 entries, and icon images at 256 KiB
and 256x256. Unsupported or inaccessible sources return an explicit fallback.
These conservative caps can reject otherwise valid large application resources.

WIC/GDI input and intermediate bitmaps are capped at 256x256, then scaled to 48x48.
AND masks, monochrome transparency and partial alpha are preserved. WIC's HICON
bridge labels its bytes BGRA. Unscaled icon DIBs are straight alpha, while
GDI-stretched icons are already premultiplied. When any channel exceeds alpha the
bytes are straight, so this module premultiplies once; it then scales as PBGRA and
swaps channels without a lossy WIC round trip.

The default per-query deadline is 500 ms. Overlapped reads check cancellation and
deadline every 5 ms; timeout cancels the operation. Results finishing after deadline
are rejected even if the query returned a palette. This is a cooperative deadline,
not a hard wall-clock guarantee: synchronous Windows file-open/metadata/WIC calls
and completion of kernel I/O cancellation have no supported hard timeout. Stop must
wait for those calls to release their buffers. A custom query must not perform
arbitrary blocking I/O. A process-isolated broker would be needed to qualify a hard
deadline against broken storage/filter drivers; that integration is not claimed.

No persistent telemetry, window titles, path logs or global invalidation callbacks
are produced. Statistics are in-memory counters for contract tests/Host diagnostics.

## Standalone verification

Use VS 2026 v145, Windows SDK 10.0.26100.0, C++20/C17, Windows 11 x64:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe' tests\IconYarn\IconYarnTests.vcxproj /m /p:Platform=x64 /p:Configuration=Debug /v:minimal
& .\tests\IconYarn\build\x64\Debug\IconYarnTests.exe
& 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe' tests\IconYarn\IconYarnTests.vcxproj /m /p:Platform=x64 /p:Configuration=Release /v:minimal
& .\tests\IconYarn\build\x64\Release\IconYarnTests.exe
```

The project generates a tiny embedded icon from `FixtureIcon.base64` into its own
ignored build directory. The native probe reads only its own test executable and
process-owned synthetic HICONs. No desktop application, OS setting, window launch
automation, repository-wide project or shared script is changed.
