#pragma once

#include "..\Contracts\Configuration.h"
#include "..\Knitting\KnitTile.h"

namespace window_sweaters::host {

inline knitting::TileSpec MakeAppearance(const contracts::Configuration& configuration) {
    if (!contracts::Valid(configuration)) throw std::invalid_argument("Invalid host appearance configuration");
    knitting::TileSpec tile;
    tile.bandDip = configuration.border_width_dip;
    tile.baseArgb = configuration.yarn_argb;
    tile.gauge.rows = configuration.stitch_rows;
    tile.stitch = static_cast<knitting::Stitch>(configuration.stitch_style);
    tile.material = static_cast<knitting::Material>(configuration.material_style);
    if (configuration.design_selection == contracts::DesignSelection::original) {
        const auto theme = designs::ws_design_theme_by_id(configuration.design_id.c_str());
        const auto chart = theme ? designs::ws_design_chart_by_name(theme->chart_name) : nullptr;
        if (!chart) throw std::invalid_argument("Host design chart is unavailable");
        tile.chart = knitting::MakeDesignChart(*chart, theme->id);
    }
    // Until identity/resolver integration, By-app uses the saved plain yarn,
    // not the Finder chart shown only as Settings' representative example.
    return knitting::NormalizeTileSpec(std::move(tile));
}

} // namespace window_sweaters::host
