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
// Windows invariant uppercase canonicalization, then FNV-1a over ASCII-folded UTF-8
// identity bytes and the upstream Murmur3 finalizer. Original ASCII vectors are unchanged.
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
// An already validated/imported asset. No path, PNG decoding or palette rewriting occurs here.
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
    // Unset follows the chosen design/app rule. Explicit plain/chart overrides all app patterns.
    std::optional<PatternChoice> global_pattern;
    Basket fallback_basket = Basket::Wool;
};
struct ColorOverrides {
    std::optional<std::uint32_t> base_argb;
    // Only generated patterns consume this; authored palettes and personal PNGs remain literal.
    std::optional<std::uint32_t> contrast_argb;
};
struct PersonalRule {
    applications::AppIdentity application;
    ColorOverrides colors;
    std::optional<PatternChoice> pattern;
};
// Future #10 worker adapter supplies a completed, identity/generation-checked proposal.
// Absence means pending/unavailable. This library never queries icons or starts work.
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

// Color priority:
//   By App: personal > authored app > sample > stable fallback.
//   Shared Zigzag: personal > sample > authored provisional > stable fallback.
//   Manual theme: personal > selected theme.
// Pattern priority: explicit global > selected theme > personal > authored app > sample > plain.
// Duplicate bound personal rules use first file order. Imports resolve longest-prefix and
// ambiguity before binding AppIdentity; omission of an imported chart can bind explicit PlainPattern.
// Null identity is unavailable metadata and receives the deterministic empty-key fallback.
ResolutionResult ResolveAppearance(
    const applications::AppIdentity* application,
    const ResolverSettings& settings,
    std::span<const PersonalRule> personal_rules = {},
    const WindowsInstallRoots& roots = {},
    const std::optional<AutomaticAppearance>& automatic = {});

} // namespace window_sweaters::colorways
