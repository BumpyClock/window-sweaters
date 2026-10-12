#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace window_sweaters::icon_yarn {

inline constexpr std::size_t MotifHeight = 6;
using MotifRows = std::array<std::string, MotifHeight>;

// Glyph is a row-major 5x5 or 6x6 string of '.', 'a' and 'b'; empty selects DiamondGlyph(5 + (seed & 1)).
MotifRows GenerateMotif(std::uint32_t seed, std::string_view glyph);
std::string_view DiamondGlyph(std::size_t side) noexcept;
// Clears stitches with no 8-neighbour; x wraps, y does not.
void RemoveIsolatedCells(std::span<std::string> rows);

}
