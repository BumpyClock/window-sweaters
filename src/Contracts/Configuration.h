#pragma once
#include <cmath>
#include <cstdint>

namespace window_sweaters::contracts {
inline constexpr std::uint32_t configuration_schema = 1;
inline constexpr float minimum_width_dip = 2.0f;
inline constexpr float maximum_width_dip = 64.0f;
inline constexpr float default_width_dip = 16.0f;

// Semantic values only. Future appearance fields belong here, not in HWND/tile state.
struct Configuration {
    std::uint32_t schema = configuration_schema;
    std::uint64_t revision = 0;
    bool paused = false;
    float border_width_dip = default_width_dip;
    bool operator==(Configuration const&) const = default;
};

inline bool Valid(Configuration const& value) noexcept {
    return value.schema == configuration_schema && std::isfinite(value.border_width_dip) &&
        value.border_width_dip >= minimum_width_dip && value.border_width_dip <= maximum_width_dip;
}
}
