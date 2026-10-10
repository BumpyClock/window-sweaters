// SPDX-License-Identifier: GPL-3.0-only
#include "KnitTile.h"
#include <stdexcept>
#include <string_view>

namespace window_sweaters::knitting {
namespace {
std::uint64_t HashCells(const std::vector<std::uint32_t>& cells) {
    std::uint64_t hash = 14695981039346656037ull;
    for (auto cell : cells)
        for (int shift = 0; shift < 32; shift += 8)
            hash = (hash ^ ((cell >> shift) & 255u)) * 1099511628211ull;
    return hash;
}
}

Chart::Chart(std::string id, std::uint64_t revision, std::uint32_t w,
             std::uint32_t h, std::vector<std::uint32_t> cells, ChartAppearance look)
    : semanticId(std::move(id)), contentRevision(revision), width(w), height(h),
      argbCells(std::move(cells)), appearance(look), contentHash(HashCells(argbCells)) {
    constexpr std::uint32_t supported = designs::ChartSolidCorners | designs::ChartOuterCuff
        | designs::ChartRoundDots | designs::ChartFittedRepeat | designs::ChartSculptedYarn
        | designs::ChartDefinedYarn;
    if (semanticId.empty() || semanticId.size() > 1024 || !width || !height
        || width > 256 || height > 256
        || argbCells.size() != static_cast<std::size_t>(width) * height
        || (appearance.flags & ~supported)
        || ((appearance.flags & designs::ChartOuterCuff) != 0 && !appearance.cuffArgb)
        || (!(appearance.flags & designs::ChartOuterCuff) && appearance.cuffArgb))
        throw std::invalid_argument("Invalid chart dimensions, identity, cells or appearance");
}

std::size_t Chart::ByteSize() const noexcept {
    return sizeof(Chart) + semanticId.capacity() + argbCells.capacity() * sizeof(std::uint32_t);
}

std::shared_ptr<const Chart> MakeDesignChart(const designs::ChartView& source,
                                            std::string id, std::uint64_t revision) {
    if (!source.name || !source.cells || !source.palette_argb || !source.width
        || !source.height || source.width > 256 || source.height > 256)
        throw std::invalid_argument("Invalid source chart view");
    std::vector<std::uint32_t> cells;
    cells.reserve(static_cast<std::size_t>(source.width) * source.height);
    for (std::uint32_t y = 0; y < source.height; ++y) {
        if (!source.cells[y] || std::string_view(source.cells[y]).size() != source.width)
            throw std::invalid_argument("Invalid source chart row");
        for (std::uint32_t x = 0; x < source.width; ++x) {
            auto c = source.cells[y][x];
            if (c != '.' && (c < 'a' || c > 'f'))
                throw std::invalid_argument("Invalid source chart palette token");
            auto argb = c == '.' ? 0u : source.palette_argb[c - 'a'];
            if (source.cuff_argb && argb == source.cuff_argb) argb = 0;
            cells.push_back(argb);
        }
    }
    ChartAppearance look{source.flags, source.corner_argb, source.cuff_argb,
        std::string_view(source.name).starts_with("atelier-")};
    return std::make_shared<const Chart>(std::move(id), revision, source.width,
                                        source.height, std::move(cells), look);
}
}
