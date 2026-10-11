#pragma once

#include "..\Colorways\Recolor.h"
#include "..\Knitting\KnitTile.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace window_sweaters::integration {

enum class ChartRecolor { Preserve, Adapt, Contrast };

inline knitting::TileSpec OverrideBase(knitting::TileSpec tile, std::uint32_t base,
    ChartRecolor recolor, std::uint32_t contrast) {
    base |= 0xff000000u;
    if (tile.chart && recolor != ChartRecolor::Preserve) {
        const auto& chart = *tile.chart;
        auto cells = chart.argbCells;
        auto look = chart.appearance;
        if (recolor == ChartRecolor::Adapt) {
            std::vector<std::uint32_t> yarns;
            const auto add = [&](std::uint32_t argb) {
                if (std::find(yarns.begin(), yarns.end(), argb | 0xff000000u) == yarns.end())
                    yarns.push_back(argb | 0xff000000u);
            };
            for (auto cell : cells) if ((cell >> 24) >= 128) add(cell);
            if (look.cornerArgb) add(look.cornerArgb);
            if (look.cuffArgb) add(look.cuffArgb);
            const auto adapted = colorways::AdaptPalette(tile.baseArgb, yarns, base);
            const auto adapt = [&](std::uint32_t argb) {
                return adapted[static_cast<std::size_t>(
                    std::find(yarns.begin(), yarns.end(), argb | 0xff000000u) - yarns.begin())];
            };
            for (auto& cell : cells) if ((cell >> 24) >= 128) cell = adapt(cell);
            if (look.cornerArgb) look.cornerArgb = adapt(look.cornerArgb);
            if (look.cuffArgb) look.cuffArgb = adapt(look.cuffArgb);
        } else {
            for (auto& cell : cells) if ((cell >> 24) >= 128) cell = contrast | 0xff000000u;
        }
        static constexpr char digits[] = "0123456789abcdef";
        std::string id = chart.semanticId + "~base-";
        for (int shift = 20; shift >= 0; shift -= 4) id += digits[(base >> shift) & 15];
        tile.chart = std::make_shared<const knitting::Chart>(std::move(id), chart.contentRevision,
            chart.width, chart.height, std::move(cells), look);
    }
    tile.baseArgb = base;
    return tile;
}

}
