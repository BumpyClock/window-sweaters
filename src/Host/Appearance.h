#pragma once

#include "..\Contracts\Configuration.h"
#include "..\Colorways\Resolver.h"
#include "..\Knitting\KnitTile.h"
#include "..\Integration\IconAppearance.h"
#include "..\Integration\ImportedAppearance.h"
#include "..\Integration\BaseOverride.h"
#include <stdexcept>
#include <utility>

namespace window_sweaters::host {

namespace detail {
inline knitting::TileSpec Finish(const contracts::Configuration& configuration, knitting::TileSpec tile,
    integration::ChartRecolor recolor) {
    if (configuration.base_override)
        tile = integration::OverrideBase(std::move(tile), configuration.base_override_argb, recolor,
            colorways::ZigzagContrast(configuration.base_override_argb));
    return knitting::NormalizeTileSpec(std::move(tile));
}

inline knitting::TileSpec Configured(const contracts::Configuration& configuration,
    const integration::ImportedAppearance* imported, integration::ChartRecolor& recolor) {
    if (!contracts::Valid(configuration)) throw std::invalid_argument("Invalid host appearance configuration");
    knitting::TileSpec tile;
    tile.bandDip = configuration.border_width_dip;
    tile.baseArgb = configuration.yarn_argb;
    tile.gauge.rows = configuration.stitch_rows;
    tile.stitch = static_cast<knitting::Stitch>(configuration.stitch_style);
    tile.material = static_cast<knitting::Material>(configuration.material_style);
    if (!configuration.imports.selected_id.empty()) {
        tile.chart=imported?imported->Find(configuration.imports.selected_id):nullptr;
        if (!tile.chart) throw std::invalid_argument("Selected imported chart is unavailable");
        recolor = integration::ChartRecolor::Preserve;
    } else if (configuration.design_selection == contracts::DesignSelection::original) {
        const auto theme = designs::ws_design_theme_by_id(configuration.design_id.c_str());
        const auto chart = theme ? designs::ws_design_chart_by_name(theme->chart_name) : nullptr;
        if (!chart) throw std::invalid_argument("Host design chart is unavailable");
        tile.chart=imported?imported->Named(chart->name):nullptr;
        recolor = tile.chart ? integration::ChartRecolor::Preserve : integration::ChartRecolor::Adapt;
        if (!tile.chart) tile.chart = knitting::MakeDesignChart(*chart, theme->id);
    }
    return tile;
}
}

inline knitting::TileSpec MakeAppearance(const contracts::Configuration& configuration,
    const integration::ImportedAppearance* imported=nullptr) {
    auto recolor = integration::ChartRecolor::Preserve;
    auto tile = detail::Configured(configuration, imported, recolor);
    return detail::Finish(configuration, std::move(tile), recolor);
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

inline knitting::TileSpec MakeByAppAppearance(const contracts::Configuration& configuration,
    const colorways::ResolutionResult& resolution, const integration::ImportedAppearance* imported=nullptr) {
    if (!contracts::PerApplication(configuration.design_selection) && !imported)
        throw std::invalid_argument("Automatic appearance requires per-application configuration");
    auto recolor = integration::ChartRecolor::Preserve;
    auto tile = detail::Configured(configuration, imported, recolor);
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
    if (resolved.mapping && (!resolved.theme_id || *resolved.theme_id != resolved.mapping->theme_id))
        throw AppearanceResolutionError(colorways::ResolutionError::UnknownTheme);
    tile.baseArgb = resolved.base_argb;
    recolor = integration::ChartRecolor::Preserve;
    if (const auto* authored = std::get_if<colorways::AuthoredChart>(&resolved.pattern)) {
        const auto* chart = designs::ws_design_chart_by_name(authored->name.c_str());
        if (!chart || authored->name.find('\0') != std::string::npos)
            throw AppearanceResolutionError(colorways::ResolutionError::UnknownChart);
        if (imported && imported->Named(authored->name)) {
            tile.chart=imported->Named(authored->name);
        } else if (resolved.chart_colors == colorways::ChartColorPolicy::LiteralAuthored) {
            tile.chart = knitting::MakeDesignChart(*chart, themeId.empty() ? authored->name : themeId);
            recolor = integration::ChartRecolor::Adapt;
        } else throw std::invalid_argument("Unsupported authored chart color policy");
    } else if (std::holds_alternative<colorways::ZigzagPattern>(resolved.pattern)) {
        if (resolved.chart_colors != colorways::ChartColorPolicy::GeneratedContrast || !resolved.contrast_argb)
            throw AppearanceResolutionError(colorways::ResolutionError::InvalidColor);
        tile.chart = integration::MakeGeneratedChart("zigzag", *resolved.contrast_argb);
        recolor = integration::ChartRecolor::Contrast;
    } else if (const auto* generated = std::get_if<colorways::GeneratedPattern>(&resolved.pattern)) {
        if (resolved.chart_colors != colorways::ChartColorPolicy::GeneratedContrast ||
            !resolved.contrast_argb || !resolved.accent_argb)
            throw AppearanceResolutionError(colorways::ResolutionError::InvalidColor);
        tile.chart = integration::MakeMotifChart(generated->seed, generated->glyph,
            *resolved.contrast_argb, *resolved.accent_argb);
        recolor = integration::ChartRecolor::Adapt;
    } else if (const auto* personal=std::get_if<colorways::PersonalChart>(&resolved.pattern)) {
        if (resolved.chart_colors!=colorways::ChartColorPolicy::PreservePersonalPixels || !imported ||
            !(tile.chart=imported->Find(personal->id,personal->revision)))
            throw AppearanceResolutionError(colorways::ResolutionError::InvalidPersonalChart);
    } else if (!std::holds_alternative<colorways::PlainPattern>(resolved.pattern) ||
               resolved.chart_colors != colorways::ChartColorPolicy::NoChart) {
        throw std::invalid_argument("Host personal pattern adaptation is unavailable");
    }
    if (resolved.color_source == colorways::ColorSource::Personal)
        return knitting::NormalizeTileSpec(std::move(tile));
    return detail::Finish(configuration, std::move(tile), recolor);
}

inline knitting::TileSpec MakeAppearance(const contracts::Configuration& configuration,
    const applications::AppIdentity& application, const colorways::WindowsInstallRoots& roots,
    const std::optional<colorways::AutomaticAppearance>& automatic = {},
    std::span<const colorways::PersonalRule> personal = {},
    const integration::ImportedAppearance* imported=nullptr) {
    const bool per_app=contracts::PerApplication(configuration.design_selection);
    if (!per_app && (!imported || imported->Personal().empty()))
        return MakeAppearance(configuration,imported);
    if (imported) personal=imported->Personal();
    colorways::ResolverSettings settings;
    if (!configuration.imports.selected_id.empty()) {
        auto chart=imported?imported->Find(configuration.imports.selected_id):nullptr;
        if (!chart) throw AppearanceResolutionError(colorways::ResolutionError::InvalidPersonalChart);
        settings.global_pattern=colorways::PersonalChart{chart->semanticId,chart->contentRevision};
    } else if (configuration.design_selection == contracts::DesignSelection::original)
        settings.design=colorways::SelectedTheme{configuration.design_id};
    else if (configuration.design_selection == contracts::DesignSelection::plain)
        settings.global_pattern=colorways::PlainPattern{};
    else if (configuration.design_selection == contracts::DesignSelection::shared_zigzag)
        settings.global_pattern = colorways::ZigzagPattern{};
    auto resolved=colorways::ResolveAppearance(&application,settings,personal,roots,automatic);
    if (!per_app)
        if (auto appearance=std::get_if<colorways::ResolvedAppearance>(&resolved);
            appearance && appearance->color_source!=colorways::ColorSource::Personal)
            appearance->base_argb=configuration.yarn_argb;
    return MakeByAppAppearance(configuration,resolved,imported);
}

}
