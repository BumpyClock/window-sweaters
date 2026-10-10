#pragma once

#include "..\Contracts\Configuration.h"
#include "..\Colorways\Resolver.h"
#include "..\Knitting\KnitTile.h"
#include <stdexcept>
#include <utility>

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
    // The identity-free overload remains the shared configuration placeholder.
    // Production By-app targets must use accepted metadata and the overload below.
    return knitting::NormalizeTileSpec(std::move(tile));
}

class AppearanceResolutionError final : public std::invalid_argument {
public:
    explicit AppearanceResolutionError(colorways::ResolutionError error)
        : std::invalid_argument(Message(error)), error_(error) {}
    colorways::ResolutionError Error() const noexcept { return error_; }
private:
    static const char* Message(colorways::ResolutionError error) noexcept {
        switch (error) {
        case colorways::ResolutionError::UnknownTheme: return "Host ByApp resolution: UnknownTheme";
        case colorways::ResolutionError::UnknownChart: return "Host ByApp resolution: UnknownChart";
        case colorways::ResolutionError::InvalidColor: return "Host ByApp resolution: InvalidColor";
        case colorways::ResolutionError::InvalidPersonalChart: return "Host ByApp resolution: InvalidPersonalChart";
        case colorways::ResolutionError::InvalidBasket: return "Host ByApp resolution: InvalidBasket";
        }
        return "Host ByApp resolution: invalid error";
    }
    colorways::ResolutionError error_;
};

// Authored/plain resolver-to-renderer boundary; imported assets and generated
// patterns require their own adapters and must not silently turn into plain yarn.
inline knitting::TileSpec MakeByAppAppearance(const contracts::Configuration& configuration,
    const colorways::ResolutionResult& resolution) {
    if (configuration.design_selection != contracts::DesignSelection::by_app)
        throw std::invalid_argument("ByApp appearance requires ByApp configuration");
    auto tile = MakeAppearance(configuration);
    if (const auto* error = std::get_if<colorways::ResolutionError>(&resolution))
        throw AppearanceResolutionError(*error);
    const auto& resolved = std::get<colorways::ResolvedAppearance>(resolution);
    if ((resolved.base_argb >> 24) != 255)
        throw AppearanceResolutionError(colorways::ResolutionError::InvalidColor);
    std::string themeId;
    if (resolved.theme_id) {
        themeId = *resolved.theme_id;
        if (themeId.find('\0') != std::string::npos ||
            !designs::ws_design_theme_by_id(themeId.c_str()))
            throw AppearanceResolutionError(colorways::ResolutionError::UnknownTheme);
    }
    // ResolveAppearance currently retains a mapping even if its theme is missing.
    // Detect that catalogue inconsistency instead of publishing its plain fallback.
    if (resolved.mapping && (!resolved.theme_id || *resolved.theme_id != resolved.mapping->theme_id))
        throw AppearanceResolutionError(colorways::ResolutionError::UnknownTheme);
    tile.baseArgb = resolved.base_argb;
    if (const auto* authored = std::get_if<colorways::AuthoredChart>(&resolved.pattern)) {
        const auto* chart = designs::ws_design_chart_by_name(authored->name.c_str());
        if (!chart || authored->name.find('\0') != std::string::npos)
            throw AppearanceResolutionError(colorways::ResolutionError::UnknownChart);
        if (resolved.chart_colors != colorways::ChartColorPolicy::LiteralAuthored)
            throw std::invalid_argument("Host ByApp generated chart adaptation is unavailable");
        tile.chart = knitting::MakeDesignChart(*chart, themeId.empty() ? authored->name : themeId);
    } else if (!std::holds_alternative<colorways::PlainPattern>(resolved.pattern) ||
               resolved.chart_colors != colorways::ChartColorPolicy::NoChart) {
        throw std::invalid_argument("Host ByApp generated/personal pattern adaptation is unavailable");
    }
    return knitting::NormalizeTileSpec(std::move(tile));
}

// Manual selections retain saved yarn/stitch semantics and never consult identity.
// The caller caches QueryWindowsInstallRoots() outside this per-target path. A
// missing/unavailable owner stays hidden at the existing Host metadata gate; it
// must not be fabricated as an empty identity to obtain a successful fallback.
inline knitting::TileSpec MakeAppearance(const contracts::Configuration& configuration,
    const applications::AppIdentity& application, const colorways::WindowsInstallRoots& roots) {
    if (configuration.design_selection != contracts::DesignSelection::by_app)
        return MakeAppearance(configuration);
    return MakeByAppAppearance(configuration,
        colorways::ResolveAppearance(&application, colorways::ResolverSettings{}, {}, roots));
}

} // namespace window_sweaters::host
