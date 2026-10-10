// SPDX-License-Identifier: GPL-3.0-only
#include "../../src/Designs/Catalogue.h"

int catalogue_c_smoke(void) {
    const ChartView* finder = ws_design_chart_by_name("atelier-finder");
    const ThemeView* discord = ws_design_theme_by_id("discord");
    return ws_design_chart_count() == 47 && ws_design_theme_count() == 37
        && finder && finder->width == 16 && finder->height == 6
        && ws_design_cell_argb(finder, 2, 0) == 0xffb7e4f7u
        && discord && discord->base_argb == 0xff5865f2u;
}
