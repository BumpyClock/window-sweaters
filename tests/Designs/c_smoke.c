// SPDX-License-Identifier: GPL-3.0-only
#include "../../src/Designs/Catalogue.h"

int catalogue_c_smoke(void) {
    const ChartView* finder = ws_design_chart_by_name("atelier-finder");
    const ThemeView* discord = ws_design_theme_by_id("discord");
    const size_t charts = ws_design_chart_count();
    return charts && ws_design_chart_at(charts - 1) && !ws_design_chart_at(charts)
        && ws_design_theme_count() && !ws_design_theme_at(ws_design_theme_count())
        && finder && finder->width == 16 && finder->height == 6
        && ws_design_cell_argb(finder, 2, 0) == 0xffb7e4f7u
        && discord && discord->base_argb == 0xff5865f2u;
}
