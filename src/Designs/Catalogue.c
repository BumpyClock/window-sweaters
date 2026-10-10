// SPDX-License-Identifier: GPL-3.0-only
// Authored data: Window Sweaters a49dff52483dafdbe27f972bd271cd28d37cef1c.
// Derived from JankyBorders by Felix Kratz; see PROVENANCE.txt and NOTICE.md.
#include "Catalogue.h"

#include <string.h>

struct collection_chart {
    const char* name;
    const char* rows[12];
    uint32_t yarn[6];
};

#include "AuthoredCharts.inc"
#include "ChartViews.inc"
#include "ThemeViews.inc"
#include "WindowsDesigns.inc"

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

_Static_assert(COUNT_OF(k_collection) == 47, "The pinned source has 47 chart repeats");
_Static_assert(COUNT_OF(k_chart_views) == COUNT_OF(k_collection), "Every chart has a view");
_Static_assert(COUNT_OF(k_theme_views) == 37, "The pinned source has 37 authored app themes");

size_t ws_design_chart_count(void) {
    return COUNT_OF(k_chart_views) + COUNT_OF(k_windows_chart_views);
}

const ChartView* ws_design_chart_at(size_t index) {
    if (index < COUNT_OF(k_chart_views))
        return &k_chart_views[index];
    index -= COUNT_OF(k_chart_views);
    return index < COUNT_OF(k_windows_chart_views) ? &k_windows_chart_views[index] : NULL;
}

const ChartView* ws_design_chart_by_name(const char* name) {
    if (name)
        for (size_t i = 0; i < ws_design_chart_count(); ++i)
            if (strcmp(name, ws_design_chart_at(i)->name) == 0)
                return ws_design_chart_at(i);
    return NULL;
}

size_t ws_design_theme_count(void) {
    return COUNT_OF(k_theme_views) + COUNT_OF(k_windows_theme_views);
}

const ThemeView* ws_design_theme_at(size_t index) {
    if (index < COUNT_OF(k_theme_views))
        return &k_theme_views[index];
    index -= COUNT_OF(k_theme_views);
    return index < COUNT_OF(k_windows_theme_views) ? &k_windows_theme_views[index] : NULL;
}

const ThemeView* ws_design_theme_by_id(const char* id) {
    if (id)
        for (size_t i = 0; i < ws_design_theme_count(); ++i)
            if (strcmp(id, ws_design_theme_at(i)->id) == 0)
                return ws_design_theme_at(i);
    return NULL;
}

size_t ws_design_theme_alias_count(void) {
    return COUNT_OF(k_theme_aliases);
}

const ThemeAliasView* ws_design_theme_alias_at(size_t index) {
    return index < COUNT_OF(k_theme_aliases) ? &k_theme_aliases[index] : NULL;
}

uint32_t ws_design_authored_cell_argb(const ChartView* chart, uint32_t x, uint32_t y) {
    if (!chart || x >= chart->width || y >= chart->height)
        return 0;
    const char yarn = chart->cells[y][x];
    return yarn >= 'a' && yarn <= 'f' ? chart->palette_argb[yarn - 'a'] : 0;
}

uint32_t ws_design_cell_argb(const ChartView* chart, uint32_t x, uint32_t y) {
    const uint32_t color = ws_design_authored_cell_argb(chart, x, y);
    return chart && (chart->flags & ChartOuterCuff) && color == chart->cuff_argb ? 0 : color;
}
