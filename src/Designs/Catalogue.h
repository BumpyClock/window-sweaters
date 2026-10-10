// SPDX-License-Identifier: GPL-3.0-only
// Authored data: Window Sweaters a49dff52483dafdbe27f972bd271cd28d37cef1c.
// Derived from JankyBorders by Felix Kratz; see PROVENANCE.txt and NOTICE.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
namespace window_sweaters::designs {
extern "C" {
#endif

enum ChartFlags {
    ChartSolidCorners = 1u << 0,
    ChartOuterCuff = 1u << 1,
    ChartRoundDots = 1u << 2,
    ChartFittedRepeat = 1u << 3,
    ChartSculptedYarn = 1u << 4,
    ChartDefinedYarn = 1u << 5
};

typedef struct ChartView {
    const char* name;
    uint32_t width;
    uint32_t height;
    const uint32_t* palette_argb; // Six literal contrast yarns, indexed by a..f.
    uint32_t flags;
    uint32_t corner_argb; // Zero uses the caller's base yarn.
    uint32_t cuff_argb;   // Curved outer cuff, one third of the band.
    const char* const* cells; // Original rows, top-down; '.' uses base yarn.
} ChartView;

typedef struct ThemeView {
    const char* id; // Stable semantic setting; never persist a catalogue index.
    const char* name;
    const char* upstream_match; // macOS source metadata, not a Windows identity.
    const char* chart_name;
    uint32_t base_argb;
    const char* palette_description;
    const char* pattern_description;
} ThemeView;

typedef struct ThemeAliasView {
    const char* upstream_match;
    const char* theme_id;
} ThemeAliasView;

// All returned data has process lifetime, is immutable, and needs no initialization.
size_t ws_design_chart_count(void);
const ChartView* ws_design_chart_at(size_t index);
const ChartView* ws_design_chart_by_name(const char* name);
size_t ws_design_theme_count(void);
const ThemeView* ws_design_theme_at(size_t index);
const ThemeView* ws_design_theme_by_id(const char* id);
size_t ws_design_theme_alias_count(void);
const ThemeAliasView* ws_design_theme_alias_at(size_t index);

// Raw authored contrast, or zero for '.', invalid input, or unused palette slots.
uint32_t ws_design_authored_cell_argb(const ChartView* chart, uint32_t x, uint32_t y);
// Renderer cell: also suppresses cuff-colored cells, matching upstream load_collection.
// Zero is a base-yarn sentinel, not an instruction to leave a transparent pixel.
uint32_t ws_design_cell_argb(const ChartView* chart, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
}
#endif
