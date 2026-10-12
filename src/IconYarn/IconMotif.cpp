#include "IconMotif.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace window_sweaters::icon_yarn {
namespace {

using Glyph = std::vector<std::string>;
using Grid = std::array<std::string, MotifHeight>;

class Rng {
public:
    explicit Rng(std::uint32_t seed) noexcept : state_((seed ^ 0x9e3779b9u) ? seed ^ 0x9e3779b9u : 1) {}
    std::uint32_t Below(std::uint32_t n) noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_ % n;
    }
    bool Chance(std::uint32_t num, std::uint32_t den) noexcept { return Below(den) < num; }
    template<class T, std::size_t N> T Pick(const T (&options)[N]) noexcept {
        return options[Below(static_cast<std::uint32_t>(N))];
    }
private:
    std::uint32_t state_;
};

Grid Blank(std::size_t width) {
    Grid rows;
    rows.fill(std::string(width, '.'));
    return rows;
}

void FreeStripe(Grid& rows, Rng& rng, std::size_t free) {
    if (!rng.Chance(1, 2)) return;
    for (std::size_t x = 0; x < rows[free].size(); ++x) rows[free][x] = x % 4 < 2 ? 'a' : '.';
}

Grid GrownPeerie(Rng& rng, const Glyph& glyph) {
    const auto side = static_cast<int>(glyph.size());
    const int m = rng.Pick({5, 7, 9}), half = m / 2 + 1;
    const int gap = 1 + static_cast<int>(rng.Below(2));
    const int top = static_cast<int>(rng.Below(2));
    std::array<std::string, 3> cells;
    for (int attempt = 0; attempt < 4; ++attempt) {
        for (int y = 0; y < 3; ++y) {
            const auto& source = glyph[std::array{0, side / 2, side - 1}[y]];
            cells[y].assign(half, '.');
            for (int x = 0; x < half; ++x)
                cells[y][x] = rng.Below(8) ? source[x * side / half] : rng.Pick({'.', 'a', 'b'});
        }
        if (cells[2][half - 1] == '.') cells[2][half - 1] = 'a';
        int filled = 0;
        for (const auto& row : cells) filled += static_cast<int>(std::count_if(row.begin(), row.end(), [](char c) { return c != '.'; }));
        if (half * 3 * 3 / 10 <= filled && filled <= half * 3 * 6 / 10) break;
    }
    auto rows = Blank(m + gap);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < half; ++x)
            for (const int yy : {y, 4 - y})
                for (const int xx : {x, m - 1 - x}) rows[top + yy][xx] = cells[y][x];
    FreeStripe(rows, rng, top == 0 ? 5 : 0);
    return rows;
}

Grid GlyphFiller(Rng& rng, const Glyph& glyph) {
    const auto side = glyph.size();
    const std::size_t filler = 2 + rng.Below(2);
    const std::size_t y0 = rng.Below(static_cast<std::uint32_t>(7 - side));
    const bool mirrored = rng.Below(2);
    const std::size_t band = rng.Below(5);
    const bool stripe = rng.Below(2);
    auto rows = Blank(side + filler);
    for (std::size_t y = 0; y < side; ++y) rows[y0 + y].replace(0, side, glyph[y]);
    const char color = std::any_of(glyph.begin(), glyph.end(),
        [](const auto& row) { return row.find('b') != std::string::npos; }) ? 'b' : 'a';
    for (std::size_t x = side; x < side + filler; ++x) {
        rows[band][x] = rows[band + 1][x] = color;
        if (stripe && band > 1) rows[0][x] = 'a';
    }
    if (mirrored)
        for (auto& row : rows) row += std::string(row.rbegin(), row.rend());
    return rows;
}

Grid MirrorGarland(Rng& rng, const Glyph& glyph) {
    const auto side = glyph.size();
    Glyph mirrored;
    for (const auto& row : glyph) {
        const auto half = (row.size() + 1) / 2;
        mirrored.push_back(row.substr(0, half) + std::string(row.rend() - (row.size() - half), row.rend()));
    }
    const std::size_t gap = 1 + rng.Below(2);
    const std::size_t y0 = rng.Below(static_cast<std::uint32_t>(7 - side));
    auto rows = Blank(2 * (side + gap));
    for (std::size_t y = 0; y < side; ++y) {
        rows[y0 + y].replace(0, side, mirrored[y]);
        rows[y0 + y].replace(side + gap, side, mirrored[side - 1 - y]);
    }
    if (side == 5) FreeStripe(rows, rng, y0 == 0 ? 5 : 0);
    return rows;
}

}

std::string_view DiamondGlyph(std::size_t side) noexcept {
    return side == 5 ? "..a.." ".aba." "aabaa" ".aba." "..a.."
                     : "..aa.." ".aaaa." "aabbaa" "aabbaa" ".aaaa." "..aa..";
}

void RemoveIsolatedCells(std::span<std::string> rows) {
    const std::vector<std::string> source(rows.begin(), rows.end());
    const auto height = static_cast<std::ptrdiff_t>(source.size());
    for (std::ptrdiff_t y = 0; y < height; ++y) {
        const auto width = static_cast<std::ptrdiff_t>(source[y].size());
        for (std::ptrdiff_t x = 0; x < width; ++x) {
            if (source[y][x] == '.') continue;
            bool neighbour = false;
            for (auto yy = std::max<std::ptrdiff_t>(0, y - 1); yy < std::min(height, y + 2); ++yy)
                for (auto dx = -1; dx <= 1; ++dx) {
                    const auto xx = (x + dx + width) % width;
                    if ((yy != y || xx != x) && source[yy][xx] != '.') neighbour = true;
                }
            if (!neighbour) rows[y][x] = '.';
        }
    }
}

MotifRows GenerateMotif(std::uint32_t seed, std::string_view glyph) {
    if (glyph.empty()) glyph = DiamondGlyph(5 + (seed & 1));
    const std::size_t side = glyph.size() == 25 ? 5 : glyph.size() == 36 ? 6 : 0;
    if (!side || glyph.find_first_not_of(".ab") != std::string_view::npos)
        throw std::invalid_argument("Invalid icon motif glyph");
    Glyph rows;
    for (std::size_t y = 0; y < side; ++y) rows.emplace_back(glyph.substr(y * side, side));
    const auto variant = seed % 3;
    Rng rng(seed ^ (variant + 1));
    auto motif = variant == 0 ? GrownPeerie(rng, rows) : variant == 1 ? GlyphFiller(rng, rows) : MirrorGarland(rng, rows);
    RemoveIsolatedCells(motif);
    return motif;
}

}
