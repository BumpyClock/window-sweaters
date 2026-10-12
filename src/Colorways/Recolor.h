// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace window_sweaters::colorways {

std::vector<std::uint32_t> AdaptPalette(std::uint32_t original_base_argb,
    std::span<const std::uint32_t> yarns_argb, std::uint32_t new_base_argb);

}
