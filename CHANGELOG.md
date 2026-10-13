# Changelog

## Unreleased

- Render knitted borders and Settings previews with GPU shaders. Hardware D3D11 is required; the CPU border renderer and `--renderer` option are removed.
- Batch up to eight border updates into one compositor publication and bound native caches and in-flight upload memory.
- Reject oversized border requests before allocation, discard stale staged updates, and clean up visibility registrations after overlay destruction.
- Recreate failed preview rendering sessions for the next request.
