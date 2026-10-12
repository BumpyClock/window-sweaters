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
the same `KnitRaster::DrawStrip` code. The CPU path draws them with software
D2D/WIC on the queue worker and shades them on the CPU. A GPU perimeter carries
a `KnitAssembly` recipe and **no strip pixels**. At upload, `KnitAssembler` draws
the strips with hardware D2D on the shared device, and HLSL applies tube/relief
lighting, fray erosion and drop shadow. CPU constructs one-dimensional coordinate
tables so GPU reciprocal rounding cannot change integer fray hashes or shadow
ownership at exact pixel-grid ties.

Both paths draw each side straight into its retained DComp surface. The content
cut-out is applied in HLSL as a signed-distance rounded rectangle; the CPU path
uses a copy shader with the same cut-out. Production does not read back GPU
pixels, run an idle render loop, or resubmit draws for pure movement. Queue and
resource-ledger admission charge GPU upload textures and coordinate tables as
transient GPU bytes. GPU allocations remain estimates, not driver residency
measurements.

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
are compared with CPU reference output. WARP D2D matches software D2D, so WARP
runs keep a two-level maximum channel error. Hardware D2D anti-aliases geometry
edges and resolves nearest-neighbour texel ties differently, so hardware runs
allow up to 96 levels per channel for linear sampling, and at most 64 pixels
beyond 96 levels for nearest sampling. On the AMD Radeon 860M the measured worst
cases were 88 and 34. `GpuShaderTests --strip-probe` compares software and
hardware strips before shading. Premultiplied bounds and transparent client interiors are
asserted independently.

Hidden Host contracts additionally cover all catalogue selections, stitch/material
variants, clipping, lifetime gates, movement reuse, aggregate quotas and injected
graphics recovery. GPU selection is asserted at preparation and draw submission.
The fixture probe reports `renderer` and `shader_draws`; the latter counts draw
submissions for the current graphics-device generation and resets on recreation.
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
  after upload; these exclude queue/tile/scratch caches, total process memory,
  the hardware tile-bitmap cache and transient shader resources.
- Summary CSVs: means, medians and ranges for five cold and twenty warm samples
  per backend/theme, using 620x140 DIP at 2x for Spotify, Messages and Notes.

Cold samples clear CPU tile and raster scratch caches. They do not recreate the
graphics device; first-use shader setup may occur in an initial sample. Warm
samples reuse those CPU caches but allocate upload textures as production does.
These are in-process comparison measurements, not a claim about whole-desktop
performance. Hardware results must not be inferred from WARP. The D3D11 texture
dimension limit is checked explicitly and shader failures follow the existing
HRESULT publication/recovery gates.
