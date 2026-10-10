# Agent notes

## Comments

Write no comments or docstrings by default. Add one only when the code cannot express a constraint and its absence would plausibly cause a bug, such as a required Win32 call order or a lifetime invariant. Keep `SPDX-License-Identifier` lines, licence and provenance headers, and the "do not edit by hand" markers in imported `src/Designs/*.inc` files.

## Chart designs

Use this section when you add or change a chart in `src/Designs/WindowsDesigns.inc`. Do not hand-edit the imported `AuthoredCharts.inc`, `ChartViews.inc` or `ThemeViews.inc`.

- Render a preview after each change: `python scripts/preview-designs.py [theme-id ...]`. It writes `out/design-preview.png`, which you can read as an image. Use `--all` to compare with the upstream themes and `-o <path>` to keep drafts apart.
- The preview shows flat cells only. Before you finish, build and run `tests/Knitting/WindowSweaters.Knitting.Tests.vcxproj`. Then inspect `tests/Knitting/outputs/<theme-id>-2x.png`, where a band is about 20px tall. A motif that you can read only in the flat preview is too detailed.
- The script exits with an error when rows differ from the declared `WINDOWS_CHART` width or when a cell letter has no yarn. Fix the chart. Do not loosen the check.
- After a pattern change, update the theme's palette and pattern descriptions. Then run `WindowSweaters.Designs.Tests` and `WindowSweaters.Colorways.Tests`.
