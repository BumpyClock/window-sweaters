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
    const uint32_t* palette_argb; // Indexed by a..f.
    uint32_t flags;
    uint32_t corner_argb;
    uint32_t cuff_argb;
    const char* const* cells; // '.' uses base yarn.
} ChartView;

typedef struct ThemeView {
    const char* id;
    const char* name;
    const char* upstream_match;
    const char* chart_name;
    uint32_t base_argb;
    const char* palette_description;
    const char* pattern_description;
} ThemeView;

typedef struct ThemeAliasView {
    const char* upstream_match;
    const char* theme_id;
} ThemeAliasView;

size_t ws_design_chart_count(void);
const ChartView* ws_design_chart_at(size_t index);
const ChartView* ws_design_chart_by_name(const char* name);
size_t ws_design_theme_count(void);
const ThemeView* ws_design_theme_at(size_t index);
const ThemeView* ws_design_theme_by_id(const char* id);
size_t ws_design_theme_alias_count(void);
const ThemeAliasView* ws_design_theme_alias_at(size_t index);

uint32_t ws_design_authored_cell_argb(const ChartView* chart, uint32_t x, uint32_t y);
// Zero is a base-yarn sentinel, not an instruction to leave a transparent pixel.
uint32_t ws_design_cell_argb(const ChartView* chart, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
}
#endif
