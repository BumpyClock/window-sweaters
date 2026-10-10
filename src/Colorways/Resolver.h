// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "..\Applications\AppIdentity.h"
#include "WindowsMappings.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace window_sweaters::colorways {

enum class Basket { Dopamine, Neon, Punch, Wool, Sorbet, Forest, Mono };
std::span<const std::uint32_t> BasketColors(Basket basket) noexcept;
// The input is AppIdentity::Key(), never a title, basename, HWND or PID.
std::uint32_t StableHash(std::wstring_view persistent_key) noexcept;
std::uint32_t FallbackColor(std::wstring_view persistent_key, Basket basket = Basket::Wool) noexcept;
std::uint32_t ZigzagContrast(std::uint32_t base_argb) noexcept;

struct PlainPattern { bool operator==(const PlainPattern&) const = default; };
struct ZigzagPattern { bool operator==(const ZigzagPattern&) const = default; };
struct AuthoredChart {
    std::string name;
    bool operator==(const AuthoredChart&) const = default;
};
struct PersonalChart {
    std::string id;
    std::uint64_t revision{};
    bool operator==(const PersonalChart&) const = default;
};
using PatternChoice = std::variant<PlainPattern, ZigzagPattern, AuthoredChart, PersonalChart>;

struct ByAppDesign {};
struct SelectedTheme { std::string id; };
using DesignSelection = std::variant<ByAppDesign, SelectedTheme>;
struct ResolverSettings {
    DesignSelection design = ByAppDesign{};
    std::optional<PatternChoice> global_pattern;
    Basket fallback_basket = Basket::Wool;
};
struct ColorOverrides {
    std::optional<std::uint32_t> base_argb;
    std::optional<std::uint32_t> contrast_argb;
};
struct PersonalRule {
    applications::AppIdentity application;
    ColorOverrides colors;
    std::optional<PatternChoice> pattern;
};
struct AutomaticAppearance {
    std::uint32_t base_argb{};
    std::optional<std::uint32_t> contrast_argb;
    std::optional<std::string> chart_name;
};

enum class ColorSource { Personal, SelectedTheme, AuthoredApplication, AutomaticSample, StableFallback };
enum class PatternSource { GlobalSelection, SelectedTheme, Personal, AuthoredApplication, AutomaticSample, PlainFallback };
enum class ChartColorPolicy { NoChart, LiteralAuthored, GeneratedContrast, PreservePersonalPixels };
struct ResolvedAppearance {
    std::uint32_t base_argb;
    std::optional<std::uint32_t> contrast_argb;
    PatternChoice pattern;
    ColorSource color_source;
    PatternSource pattern_source;
    ChartColorPolicy chart_colors;
    std::optional<std::string_view> theme_id;
    const WindowsMapping* mapping;
};
enum class ResolutionError { UnknownTheme, UnknownChart, InvalidColor, InvalidPersonalChart, InvalidBasket };
using ResolutionResult = std::variant<ResolvedAppearance, ResolutionError>;

// Imports resolve longest-prefix and ambiguity before AppIdentity binding; omitted charts can bind PlainPattern.
ResolutionResult ResolveAppearance(
    const applications::AppIdentity* application,
    const ResolverSettings& settings,
    std::span<const PersonalRule> personal_rules = {},
    const WindowsInstallRoots& roots = {},
    const std::optional<AutomaticAppearance>& automatic = {});

}
