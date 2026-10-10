#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include "..\Designs\Catalogue.h"
#include "ApplicationPayload.h"

namespace window_sweaters::contracts {
inline constexpr std::uint32_t configuration_schema = 4;
inline constexpr float minimum_width_dip = 2.0f;
inline constexpr float maximum_width_dip = 64.0f;
inline constexpr float default_width_dip = 16.0f;

inline constexpr std::size_t maximum_design_id_bytes = 63;
inline constexpr float minimum_stitch_rows = 1.5f;
inline constexpr float maximum_stitch_rows = 12.f;
enum class DesignSelection : std::uint32_t { plain, original, by_app, shared_zigzag };
inline bool PerApplication(DesignSelection selection) noexcept {
    return selection == DesignSelection::by_app || selection == DesignSelection::shared_zigzag;
}
enum class StitchStyle : std::uint32_t { stockinette, rib, garter };
enum class MaterialStyle : std::uint32_t { plain, raised };

struct Configuration {
    std::uint32_t schema = configuration_schema;
    std::uint64_t revision = 0;
    bool paused = false;
    float border_width_dip = default_width_dip;
    DesignSelection design_selection = DesignSelection::plain;
    std::string design_id;
    StitchStyle stitch_style = StitchStyle::stockinette;
    MaterialStyle material_style = MaterialStyle::raised;
    float stitch_rows = 6.f;
    std::uint32_t yarn_argb = 0xff4e8098u;
    applications::EligibilityPreferences eligibility;
    bool operator==(Configuration const&) const = default;
};

inline bool Valid(Configuration const& value) noexcept {
    return value.schema == configuration_schema && BoundedPreferences(value.eligibility) && std::isfinite(value.border_width_dip) &&
        value.border_width_dip >= minimum_width_dip && value.border_width_dip <= maximum_width_dip &&
        value.design_selection <= DesignSelection::shared_zigzag && value.stitch_style <= StitchStyle::garter &&
        value.material_style <= MaterialStyle::raised &&
        std::isfinite(value.stitch_rows) && value.stitch_rows >= minimum_stitch_rows &&
        value.stitch_rows <= maximum_stitch_rows && (value.yarn_argb >> 24) == 255 &&
        value.design_id.size() <= maximum_design_id_bytes &&
        (value.design_selection == DesignSelection::original
            ? !value.design_id.empty() &&
                value.design_id.find('\0') == std::string::npos &&
                designs::ws_design_theme_by_id(value.design_id.c_str()) != nullptr
            : value.design_id.empty());
}
}
