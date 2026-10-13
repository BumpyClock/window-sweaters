# CPU/GPU rendering comparison

`WindowSweaters.Host.exe --renderer cpu` selects the reference renderer.
CPU remains the default. `--renderer gpu` selects experimental hardware D2D strip
assembly and D3D11 shading. The selection is part of each immutable frame request and survives
configuration changes and device recovery. Invalid selections and shader failures
are errors, not CPU fallback. Hardware device initialization can still use WARP
when hardware creation fails; this is not a renderer fallback.

## Implementation boundary

Both paths use the existing CPU stitch and chart tile generation, material
preparation, fibre grain and blurred height fields. Both draw the periodic
colour and height strips, corner patches, cuffs, ring masks and dimming through
the same `DrawKnitStrip` code. The CPU path draws them with software
D2D/WIC on the queue worker and shades them on the CPU. A GPU perimeter carries
a `KnitAssembly` recipe and **no strip pixels**. At upload, `KnitAssembler` draws
the strips with hardware D2D on the shared device, and HLSL applies tube/relief
lighting, fray erosion and drop shadow. CPU constructs one-dimensional coordinate
tables so GPU reciprocal rounding cannot change integer fray hashes or shadow
ownership at exact pixel-grid ties.

Monitor DPI changes reuse the same immutable colour and height tile pixels when
the raster scale and other raster inputs stay equal. Strip geometry, physical
dimensions and sampling still use the current frame scale. The native tile bitmap
cache also reuses these tiles. Coordinate textures are cached by length, origin
and scale. Both caches reserve aggregate GPU quota before allocation; overlay
release retains cached bytes, and cache eviction or device destruction releases
them.

Both paths draw each side straight into its retained DComp surface. The content
cut-out is applied in HLSL as a signed-distance rounded rectangle; the CPU path
uses a copy shader with the same cut-out. Production does not read back GPU
pixels, run an idle render loop, or resubmit draws for pure movement. Queue and
resource-ledger admission reserve the GPU working set before preparation. The
queue estimate includes transient strip and coordinate bytes. The resource ledger
also reserves worst-case native tile and coordinate cache bytes, transfers actual
allocations to persistent cache leases, and drops unused cache budget and strip
temporaries after publication. GPU allocations remain estimates, not driver residency
measurements. Retained CPU accounting includes the main tile and GPU assembly
corner/cuff tiles, deduplicated by pointer in the resource ledger. CPU publication
also reserves all four in-flight source textures until batch completion. Queue ready
admission includes all three tiles for GPU chart requests.

The mitre masks overlap by 1/8 pixel. Without the overlap, their aliased 45°
edges pass through pixel centres, and software and hardware D2D resolve those
ties differently, which showed as dotted seams after shading.

SDK FXC compiles shader-model-4 bytecode into generated headers during MSBuild;
the executable embeds these bytes. No runtime HLSL files, compiler dependency or
source paths are needed. x64 and ARM64 use the same embedded shader programs.

## Safe repeatable validation

From the repository root:

```powershell
.\tests\Rendering\compare-gpu.ps1 -Configuration Release -Platform x64
.\tests\Rendering\compare-gpu.ps1 -Configuration Release -Platform ARM64
```

The script uses isolated `out\gpu-validation` binaries, an owned PE fixture, and
hidden native contracts. It does not launch a production Host, Settings or visible
fixture. ARM64 builds on x64 but runtime checks require ARM64 Windows. `-NoBuild`
reuses those binaries; `-SkipWarp` explicitly omits WARP validation.

Hardware shader comparisons request hardware without fallback. WARP comparisons
explicitly request WARP, including hidden upload/recreation. The matrix covers
every theme with four width/scale combinations (2/1x, 10/1.25x, 24/2x, 64/3x),
both materials and square/rounded corners, every authored chart, both sampling
modes, both anchors, dimming, arbitrary literal RGB chart cells, fractional and
extreme supported scales, physical padding and tiny frames. Pixel readbacks
are compared with CPU reference output. Every matrix case also uploads identical
software-prepared colour and height strips to the shader and applies CPU ring
lighting, relief, fray and shadow to those same inputs. This shader-only check
allows at most two channel levels on both hardware and WARP. Adapter metadata
records its separate maximum error and the number of pixels changed above the
tolerance by each shading stage. The matrix requires all four stages to change
pixels above tolerance and includes one shared solid tile used for both corner
and cuff brushes.

The complete strip-assembly comparison has a separate gate. WARP D2D matches software D2D, so WARP
runs keep a two-level maximum channel error. Hardware D2D anti-aliases geometry
edges and resolves nearest-neighbour texel ties differently, so hardware runs
allow up to 96 levels per channel for linear sampling, and at most 64 pixels
beyond 96 levels for nearest sampling. `GpuShaderTests --strip-probe` compares software and
hardware strips before shading. `--shader-only` runs only the identical-input
matrix and cache contracts. `--contracts-only` runs shared corner/cuff parity,
cache quota and hidden upload/recreation contracts. `--benchmarks-only` runs
hardware timings. Premultiplied bounds and transparent client interiors are
asserted independently. Upload-mask samples lie inside retained strips at a straight
edge and rounded corner; an unmasked readback proves the excluded samples contain
opaque yarn. Mask fixtures use a six-DIP source tuck and a one-DIP upload tuck
to test upload exclusion independently of the source ring cutout. Hidden DComp mask checks run CPU and GPU frames at 1x, 1.25x, 1.5x
and 2x with square, round-small and round corners.

Hidden Host contracts additionally cover all catalogue selections, stitch/material
variants, clipping, lifetime gates, movement reuse, aggregate quotas and injected
graphics recovery. GPU selection is asserted at preparation and draw submission.
The fixture probe reports `renderer` and `shader_draws`; the latter counts GPU
relief draws only for the current graphics-device generation and resets on recreation.
Recovery tests recreate real D3D/DComp objects but do not force an actual driver
reset. Retained-idle checks observe draw counters, not GPU power consumption.

## Artifacts and timing

`out\gpu-comparison\<platform>-<configuration>` contains:

- `source.json`: source commit, dirty state, rendering file hashes, driver and build
  information.
- CPU and GPU hidden-contract logs. Expected injected errors and quota logs are
  retained; the process exit code is the gate.
- Hardware/WARP adapter metadata, counts, CPU/GPU PNG pairs and amplified diffs.
- `generation-benchmark.csv`: CPU preparation plus GPU strip assembly, shader
  execution and explicit D3D event-query completion. CPU timing completes all
  CPU shading. No DComp, PNG, readback or device creation is timed.
- `benchmark.csv`: production CPU/GPU preparation plus four-strip upload, native
  DComp commit completion and explicit GPU completion. No PNG or pixel readback
  is timed. Commit timing can include compositor cadence and scheduling.
  Each sample also records retained CPU bytes and estimated GPU surface bytes
  after upload. Estimated GPU bytes include native tile/coordinate caches and
  retained surfaces; they exclude total process memory and queue/CPU tile/scratch
  caches. Upload-transient resources are released after publication.
- `multi-overlay-benchmark.csv`: eight hidden GPU overlays, each with a different
  frame size, published individually or in one batch. Each sample includes DComp
  completion, explicit GPU completion, commit/wait counts and native cache upload
  counts. Each theme/publication mode has one cold and nine warm samples; the
  cold sample starts with a new graphics device. Preparation of CPU tiles and
  creation of windows/devices are outside the timing.
- Summary CSVs: means, medians and ranges for five cold and twenty warm samples
  per backend/theme, using 620x140 DIP at 2x for Spotify, Messages and Notes.

Cold samples clear CPU tile and raster scratch caches. They do not recreate the
graphics device; first-use shader setup may occur in an initial sample. Warm
samples reuse those CPU caches but allocate upload textures as production does.
These are in-process comparison measurements, not a claim about whole-desktop
performance. Hardware results must not be inferred from WARP. The D3D11 texture
dimension limit of 16,384 pixels includes band and shadow padding. Queue admission
for both renderers and direct GPU preparation reject larger dimensions before tile
generation; the assembler also validates strip dimensions before device initialization.
Standalone CPU oracle preparation retains its separate 32,768-pixel dimension limit.
Shader failures follow the existing HRESULT publication/recovery gates.

## Recorded batching measurement

[Raw samples from 2026-10-12](measurements/gpu-batching-2026-10-12.csv) were
captured on Windows with AMD Radeon 860M graphics using the x64 Release build
of this implementation before commit. Eight hidden overlays with different frame
sizes were published individually or in one batch. Each theme/publication mode
has one cold sample followed by nine warm samples. Timing includes GPU completion
and DirectComposition commit completion; device/window creation and CPU tile
preparation are excluded.

Spotify's warm median fell from 133.579 ms to 66.7576 ms with batching. Commits
and waits fell from eight each to one each. All 54 warm samples recorded zero
native tile and coordinate uploads. These measurements cover hidden publication,
not whole-desktop presentation.
