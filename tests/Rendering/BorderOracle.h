// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../src/Rendering/KnitPreparation.h"

namespace window_sweaters::testing {
knitting::RasterImage SoftwareStrip(const rendering::KnitPerimeter& perimeter, std::size_t side, bool heights);
std::array<knitting::RasterImage, 4> ReferenceStrips(const rendering::KnitPerimeter& perimeter);
knitting::RasterImage ComposeStrips(const rendering::KnitPerimeter& perimeter,
    const std::array<knitting::RasterImage, 4>& strips);
knitting::RasterImage ReferenceFrame(const rendering::KnitPerimeter& perimeter);
}
