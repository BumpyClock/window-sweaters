# GPU rendering validation

Host borders and Settings previews use GPU strip assembly and HLSL shading.
The former `--renderer` option is rejected, including `cpu` and `gpu` values.
The software border implementation exists only in `BorderOracle` and `BorderMath`
under this test directory. CPU stitch/chart tile generation remains production
code because GPU assembly uses those immutable colour and height textures.

## Implementation boundary

`KnitPreparation::PreparePerimeter` creates geometry, shared tile inputs, and a
required `KnitAssembly` recipe. It creates no border strip pixels. `KnitAssembler`
draws periodic colour and height strips, corner patches, cuffs, masks, and dimming
with Direct2D. `GpuShader` applies tube/relief lighting, fray, shadow, and the client
cut-out into retained DirectComposition surfaces. `GpuSnapshot` uses the same
assembly and shader, composes the four strips on the GPU, and reads back one BGRA
frame for previews. Its device and caches persist across preview requests.

DPI changes reuse immutable tile pixels while geometry and sampling use the
current frame scale. Native tile and coordinate caches reserve aggregate GPU
quota before allocation. Overlay release preserves cached bytes; eviction or
device destruction releases them. Admission reserves the full upload working
set, transfers actual cache allocations into persistent leases, and releases
unused budget and strip temporaries after publication. Snapshot admission also
includes the complete frame texture and staging readback. GPU byte counts are
allocation estimates, not driver residency measurements.

The Host retains published surfaces for idle periods and pure movement. Each
publication batch draws once, then commits and waits once for the prepared
borders. Shader failures follow the HRESULT publication/recovery path. SDK FXC
embeds shader-model-4 bytecode during MSBuild for both x64 and ARM64.

## Repeatable validation

From the repository root:

```powershell
.\tests\Rendering\compare-gpu.ps1 -Configuration Release -Platform x64
.\tests\Rendering\compare-gpu.ps1 -Configuration Release -Platform ARM64
```

The script uses isolated `out\gpu-validation` binaries, an owned PE fixture, and
hidden native contracts. ARM64 builds on x64; execution requires ARM64 Windows.
`-NoBuild` reuses binaries; `-SkipWarp` explicitly omits WARP qualification.
Hardware shader tests request hardware without fallback. WARP tests explicitly
request WARP. Knitting visual tests use persistent WARP snapshots for deterministic
GPU rendering; production previews and appearance parity use hardware snapshots.

The matrix covers every theme, chart, material, sampling mode, anchor, dimming,
fractional/extreme supported scales, physical padding, literal RGB cells, tiny
frames, square/rounded corners, and four band/scale combinations. Each case uploads
identical software-prepared colour and height strips to the shader and applies
the test-only CPU ring oracle to those same inputs. The strict maximum is **two
channel levels on hardware and WARP**. Separate stage counters require lighting,
relief, fray, and shadow to change pixels above that tolerance.

The complete assembly comparison has a separate existing rasterization gate:
WARP allows two channel levels. Hardware Direct2D resolves edge coverage and
nearest-neighbour texel ties differently from software Direct2D, so linear
sampling allows a maximum of 96; nearest sampling allows at most 64 pixels beyond
96. This does not relax the identical-input shader gate. Premultiplied bounds
and transparent interiors are checked independently.

`--strip-probe` compares strips before shading; `--shader-only` runs the strict
shader matrix and caches; `--contracts-only` runs shared corner/cuff, cache quota,
hidden upload/recreation, and snapshot contracts; `--benchmarks-only` runs hardware
timings. Snapshot checks cover repeated output, resize/restoration, composition
against captured production strips, and rejected dimensions. Hidden masks use
opaque unmasked samples to establish exclusion at straight edges and rounded
corners. Host contracts cover catalogue/appearance changes, clipping, lifetime,
movement reuse, aggregate quotas, and injected graphics recovery. The fixture
reports `shader_draws` for the current device generation. Recovery tests recreate
real D3D/DComp objects without forcing a driver reset. Idle checks observe draw
counters, not power consumption.

## Artifacts and timing

`out\gpu-comparison\<platform>-<configuration>` contains source hashes, dirty
state, driver/build information, hidden Host logs, hardware/WARP adapter metadata,
test-oracle/GPU PNG pairs, and amplified diffs. Expected injected errors remain
in logs; exit codes establish pass/fail.

`generation-benchmark.csv` records CPU tile/preparation work plus GPU assembly,
shading, and explicit D3D event-query completion. `benchmark.csv` adds production
four-strip upload and DirectComposition commit completion, with retained CPU
input bytes and estimated GPU bytes. Both now contain GPU samples only: five
cold and twenty warm per theme at 620x140 DIP and 2x. Cold samples clear the CPU
tile cache; devices and native caches persist. PNG, readback, and device creation
are outside timing. Commit timing can include compositor cadence and scheduling.

The D3D11 texture dimension limit of 16,384 pixels includes band and shadow
padding. Queue admission and direct preparation reject larger dimensions before
tile generation; the assembler validates strip dimensions before device
initialization. Shader failures follow the existing HRESULT publication/recovery
gates.

`multi-overlay-benchmark.csv` compares eight hidden GPU overlays published
individually or in one batch. It records completed GPU/DComp timing, commit/wait
counts, and native cache upload counts, with one cold and nine warm samples per
theme/publication mode. Window/device creation and CPU preparation are excluded.
Summary CSVs report means, medians, and ranges. These measure hidden publication;
hardware results must not be inferred from WARP or treated as desktop performance.

## Recorded batching measurement

[Raw samples from 2026-10-12](measurements/gpu-batching-2026-10-12.csv) were
captured before the GPU-only migration on Windows with AMD Radeon 860M graphics
using an x64 Release build. They are historical batch-publication evidence, not
qualification results for the current GPU-only runtime. Eight hidden overlays with different frame
sizes were published individually or in one batch. Each theme/publication mode
has one cold sample followed by nine warm samples. Timing includes GPU completion
and DirectComposition commit completion; device/window creation and CPU tile
preparation are excluded.

Spotify's warm median fell from 133.579 ms to 66.7576 ms with batching. Commits
and waits fell from eight each to one each. All 54 warm samples recorded zero
native tile and coordinate uploads. These measurements cover hidden publication,
not whole-desktop presentation.
