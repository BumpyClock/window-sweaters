# IconYarn preparation contract

This module supplies portable sampling and bounded preparation for issue #10.
The Host integrates its completion gate and worker with validated owner metadata,
the shared resolver, and native knitting publication. Settings exposes a distinct
persisted Shared Zigzag mode. Native acceptance and independent integration review
are recorded separately in `docs\parity.md`.
Portable consumers compile `IconPalette.cpp`, `IconMotif.cpp` and `Preparation.cpp`
with C++20. Windows consumers add `WindowsIcon.cpp` and link `user32.lib`, `gdi32.lib`,
`ole32.lib` and `windowscodecs.lib`; the standalone test project supplies these.

## Palette boundary

`IconPalette.h` has no Windows dependency. `SamplePalette(pixels, identity)` accepts
exactly 2,304 top-down, 48x48 sRGB `PremultipliedRgba8` pixels. Channels are bytes in
R,G,B,A order, premultiplied in sRGB, not linear-light space. Invalid size or RGB
greater than alpha returns `Fallback{InvalidPixels}`. Fewer than 38 pixels with alpha
128 or more returns `Fallback{InsufficientColorCoverage}`. Grey and monochrome icons
succeed. Input and output contain no handles.

The algorithm ports the approved icon-seeded prototype (`icongen.py`) exactly,
including Python's `max`/`index` tie breaks, floor division, `colorsys` HLS and
round-half-even byte conversion. Golden tests in `tests\IconYarn` pin its output.

1. Divide RGB by alpha, rounded. Pixels with alpha 128 or more are opaque.
2. Bucket opaque pixels by the top 3 bits of each channel. Average each bucket and
   sort by (count, average) descending. The largest is the base. Candidates are the
   other buckets with at least 1/33 of the pixels and squared RGB distance 3,600 or
   more from the base. The contrast is the candidate with the largest luma difference
   from the base. Without one, the base is mixed 60% toward white (luma below 140)
   or 55% toward black. The accent is the first candidate at distance 3,600 or more
   from the contrast, else the contrast mixed 50% with gold `#edae49`.
3. The glyph side G is `5 + (FNV-1a(identity) & 1)`. The opaque bounding box is cut
   into a GxG grid. Each cell takes the majority of transparent, base, contrast or
   accent pixels (nearest raw colour), written `.`, `.`, `a`, `b`. Cells with no
   8-neighbour are cleared (x wraps, y does not). A glyph with fewer than G*G/6 stitches
   or fewer than G*G/5 gaps becomes the built-in diamond.
4. The seed is FNV-1a over the identity bytes, the glyph characters and each raw
   channel shifted right by 4.
5. Each colour is softened in HLS: saturation `clamp(s*.48,.22,.60)` (0 below .08),
   lightness `clamp(l*.85+.10,.30,.72)`. A contrast or accent within .18 lightness of
   the base moves to base lightness ±.22.

`PaletteResult` is `variant<SampledPalette,Fallback>`. `SampledPalette` holds opaque
`baseArgb`, `contrastArgb` and `accentArgb` in `0xffRRGGBB` order, the row-major
`glyph` string and the `seed`. `FallbackAccent(base, contrast)` applies the same
gold mix and softening, separated from `base`, for apps without a sample.
`ZigzagContrast` serves Shared Zigzag and the Settings preview: `0xfff6f0de` below
lightness .62, otherwise the same hue and saturation at L-.26.

The identity must be the persistent, canonical, kind-prefixed UTF-8 identity from
`integration::MakeIconInput`. `AppIdentity::Kind()+Key()` must be converted at the
Host boundary: executable identity equality is Windows ordinal case-insensitive,
while AUMID equality is exact. Raw Key alone, PID, HWND, window titles, display names
and basenames are not valid persistent salts.

## Motif generation

`IconMotif.h` is portable. `GenerateMotif(seed, glyph)` returns six equal rows of
`.`, `a` and `b`, 6 to 18 stitches wide, with no isolated stitches. An empty glyph
uses the diamond of side `5 + (seed & 1)`. `seed % 3` picks the generator, seeded with
xorshift32 `Rng(seed ^ (variant + 1))`:

* 0, grown peerie: a 5, 7 or 9 wide mirrored peerie grown from the glyph's top,
  middle and bottom rows, with an optional dashed stripe.
* 1, glyph and filler: the glyph beside a two-row filler bar, optionally mirrored.
* 2, mirror garland: the left-mirrored glyph next to its vertical flip.

## Resolver interoperability and precedence

`integration::AutomaticProposal` translates a successful sample to
`colorways::AutomaticAppearance{base_argb, contrast_argb, accent_argb, seed, glyph}`.
An unmapped app with a sample resolves to `GeneratedPattern{seed, glyph}` with
`PatternSource::AutomaticSample`. While pending or after a fallback, it resolves to
`GeneratedPattern{StableHash(key), ""}` with `PatternSource::GeneratedFallback`, on
the stable basket colour, `ZigzagContrast(base)` and `FallbackAccent`. Only a missing
application identity gets `PlainFallback`. `integration::MakeMotifChart` knits the
rows as `generated-<seed>`, with `a` as contrast, `b` as accent and `.` as base.

The resolver, not this worker, applies explicit personal colors, manual selections,
authored By App mapping, mode semantics and personal PNG precedence. A personal base
replaces the sampled contrast and accent with ones derived from that base. Sampling
must never directly mutate a border or recolor a semantic personal PNG asset. Shared
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
