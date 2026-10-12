// SPDX-License-Identifier: GPL-3.0-only
// Baskets/hash/contrast adapted from Window Sweaters src/knit.c:7-113,
// a49dff52483dafdbe27f972bd271cd28d37cef1c (https://github.com/saragordic/window-sweaters).
// Original precedence: src/apps.c and src/autoyarn.m at the same revision.
#include "Resolver.h"
#include "..\Designs\Catalogue.h"
#include "..\IconYarn\IconPalette.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <utility>

namespace window_sweaters::colorways {
namespace {
constexpr std::uint32_t dopamine[] = {0xffff2d95, 0xff00d9ff, 0xffffd400, 0xff7c3aff, 0xff00e676, 0xffff6b00, 0xffff1744, 0xff00b8d4};
constexpr std::uint32_t neon[] = {0xfff50057, 0xff00e5ff, 0xffc6ff00, 0xff651fff, 0xff1de9b6, 0xffff9100};
constexpr std::uint32_t punch[] = {0xffe8175d, 0xff0fb9b1, 0xfffec230, 0xff5f27cd, 0xff10ac84, 0xffee5a24};
constexpr std::uint32_t wool[] = {0xffd1495b, 0xff4e8098, 0xffedae49, 0xff7c9885, 0xff9b6a8f, 0xffe8846b, 0xff3f7d6e, 0xffc46a4e, 0xff6d7ba8, 0xffb8935f};
constexpr std::uint32_t sorbet[] = {0xfff08ca4, 0xff7fc6c4, 0xffffc978, 0xffb08ed4, 0xff8fce90, 0xffff9f7a, 0xff8ab6f0, 0xffe5a3d0};
constexpr std::uint32_t forest[] = {0xff4a6b52, 0xff7d8f5c, 0xff3f6b6e, 0xff8a7248, 0xff5c6b8a, 0xff6b5344, 0xff2f5e4a};
constexpr std::uint32_t mono[] = {0xff9aa0a6, 0xff7c8288, 0xffb4bac0, 0xff686e74, 0xff8d939a};
constexpr std::uint32_t cream = 0xfff6f0de;

std::uint32_t Mix(std::uint32_t hash) noexcept {
    hash ^= hash >> 16; hash *= 0x85ebca6b;
    hash ^= hash >> 13; hash *= 0xc2b2ae35;
    return hash ^ (hash >> 16);
}

void HashByte(std::uint32_t& hash, std::uint32_t byte) noexcept {
    if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    hash = (hash ^ byte) * 16777619u;
}

void HashCodePoint(std::uint32_t& hash, std::uint32_t point) noexcept {
    if (point <= 0x7f) HashByte(hash, point);
    else if (point <= 0x7ff) {
        HashByte(hash, 0xc0 | (point >> 6));
        HashByte(hash, 0x80 | (point & 0x3f));
    } else if (point <= 0xffff) {
        HashByte(hash, 0xe0 | (point >> 12));
        HashByte(hash, 0x80 | ((point >> 6) & 0x3f));
        HashByte(hash, 0x80 | (point & 0x3f));
    } else {
        HashByte(hash, 0xf0 | (point >> 18));
        HashByte(hash, 0x80 | ((point >> 12) & 0x3f));
        HashByte(hash, 0x80 | ((point >> 6) & 0x3f));
        HashByte(hash, 0x80 | (point & 0x3f));
    }
}

void HashUtf16(std::uint32_t& hash, std::wstring_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto point = static_cast<std::uint32_t>(value[i]);
        if (point >= 0xd800 && point <= 0xdbff && i + 1 < value.size()) {
            const auto low = static_cast<std::uint32_t>(value[i + 1]);
            if (low >= 0xdc00 && low <= 0xdfff) {
                point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
                ++i;
            }
        }
        HashCodePoint(hash, point);
    }
}

struct ColorCandidate { std::optional<std::uint32_t> color; ColorSource source; };
struct PatternCandidate { std::optional<PatternChoice> pattern; PatternSource source; };

std::optional<ResolutionError> ValidatePattern(const PatternChoice& pattern) {
    if (const auto* chart = std::get_if<AuthoredChart>(&pattern)) {
        if (!designs::ws_design_chart_by_name(chart->name.c_str()) ||
            chart->name.find('\0') != std::string::npos) return ResolutionError::UnknownChart;
    } else if (const auto* personal = std::get_if<PersonalChart>(&pattern)) {
        if (personal->id.empty() || personal->id.find('\0') != std::string::npos || !personal->revision)
            return ResolutionError::InvalidPersonalChart;
    } else if (const auto* generated = std::get_if<GeneratedPattern>(&pattern)) {
        const auto size = generated->glyph.size();
        if ((size && size != 25 && size != 36) || generated->glyph.find_first_not_of(".ab") != std::string::npos)
            return ResolutionError::UnknownChart;
    }
    return std::nullopt;
}

bool Opaque(std::uint32_t color) noexcept { return (color >> 24) == 0xff; }
}

std::span<const std::uint32_t> BasketColors(Basket basket) noexcept {
    switch (basket) {
    case Basket::Dopamine: return dopamine;
    case Basket::Neon: return neon;
    case Basket::Punch: return punch;
    case Basket::Wool: return wool;
    case Basket::Sorbet: return sorbet;
    case Basket::Forest: return forest;
    case Basket::Mono: return mono;
    }
    return {};
}

std::uint32_t StableHash(std::wstring_view key) noexcept {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const auto point = static_cast<std::uint32_t>(key[i]);
        if (point <= 0x7f) {
            HashByte(hash, point);
            continue;
        }
        std::size_t units = 1;
        if (point >= 0xd800 && point <= 0xdbff && i + 1 < key.size()) {
            const auto low = static_cast<std::uint32_t>(key[i + 1]);
            if (low >= 0xdc00 && low <= 0xdfff) units = 2;
        }
        // Map complete scalars; never split surrogate pairs.
        std::array<wchar_t, 6> uppercase{};
        const int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
            key.data() + i, static_cast<int>(units), uppercase.data(),
            static_cast<int>(uppercase.size()), nullptr, nullptr, 0);
        HashUtf16(hash, mapped > 0 ? std::wstring_view(uppercase.data(), static_cast<std::size_t>(mapped)) :
            key.substr(i, units));
        i += units - 1;
    }
    return Mix(hash);
}

std::uint32_t FallbackColor(std::wstring_view key, Basket basket) noexcept {
    auto colors = BasketColors(basket);
    if (colors.empty()) colors = wool;
    return colors[StableHash(key) % colors.size()];
}

std::uint32_t ZigzagContrast(std::uint32_t base) noexcept {
    const double r = ((base >> 16) & 255) / 255.0, g = ((base >> 8) & 255) / 255.0, b = (base & 255) / 255.0;
    const double maximum = std::max({r, g, b}), minimum = std::min({r, g, b});
    double lightness = (maximum + minimum) / 2, hue = 0, saturation = 0;
    if (lightness < 0.62) return cream;
    if (maximum != minimum) {
        const double delta = maximum - minimum;
        saturation = lightness > 0.5 ? delta / (2 - maximum - minimum) : delta / (maximum + minimum);
        hue = maximum == r ? (g - b) / delta + (g < b ? 6 : 0) :
            maximum == g ? (b - r) / delta + 2 : (r - g) / delta + 4;
        hue /= 6;
    }
    lightness -= 0.26;
    const double q = lightness < 0.5 ? lightness * (1 + saturation) :
        lightness + saturation - lightness * saturation;
    const double p = 2 * lightness - q;
    const std::array<double, 3> positions{hue + 1.0 / 3, hue, hue - 1.0 / 3};
    std::array<std::uint32_t, 3> channels{};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        double c = positions[i];
        if (c < 0) c += 1;
        if (c > 1) c -= 1;
        const double value = c < 1.0 / 6 ? p + (q - p) * 6 * c : c < 0.5 ? q :
            c < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - c) * 6 : p;
        channels[i] = static_cast<std::uint32_t>(value * 255);
    }
    return 0xff000000u | (channels[0] << 16) | (channels[1] << 8) | channels[2];
}

ResolutionResult ResolveAppearance(
    const applications::AppIdentity* application, const ResolverSettings& settings,
    std::span<const PersonalRule> personal_rules, const WindowsInstallRoots& roots,
    const std::optional<AutomaticAppearance>& automatic) {
    if (BasketColors(settings.fallback_basket).empty()) return ResolutionError::InvalidBasket;
    const designs::ThemeView* selected = nullptr;
    if (const auto* manual = std::get_if<SelectedTheme>(&settings.design)) {
        selected = designs::ws_design_theme_by_id(manual->id.c_str());
        if (!selected || manual->id.find('\0') != std::string::npos) return ResolutionError::UnknownTheme;
    }
    const auto* mapping = application && !selected ? MatchWindowsApplication(*application, roots) : nullptr;
    const auto* authored = mapping ? designs::ws_design_theme_by_id(mapping->theme_id.data()) : nullptr;
    const PersonalRule* personal = nullptr;
    if (application) {
        for (const auto& rule : personal_rules) {
            if (rule.application == *application) { personal = &rule; break; }
        }
    }
    const auto* sample = application && automatic ? &*automatic : nullptr;
    auto global = settings.global_pattern;
    if (global) {
        if (const auto* chart = std::get_if<AuthoredChart>(&*global); chart && chart->name == "zigzag")
            global = ZigzagPattern{};
    }
    const bool shared = global && std::holds_alternative<ZigzagPattern>(*global);
    const auto personal_base = personal ? personal->colors.base_argb : std::nullopt;
    const auto sampled_base = sample ? std::optional(sample->base_argb) : std::nullopt;
    const auto authored_base = authored ? std::optional(authored->base_argb) : std::nullopt;
    const std::uint32_t fallback = FallbackColor(application ? application->Key() : std::wstring_view{}, settings.fallback_basket);
    const std::array<ColorCandidate, 6> colors{{
        {personal_base, ColorSource::Personal},
        {selected ? std::optional(selected->base_argb) : std::nullopt, ColorSource::SelectedTheme},
        {shared && !selected ? sampled_base : std::nullopt, ColorSource::AutomaticSample},
        {authored_base, ColorSource::AuthoredApplication},
        {!selected ? sampled_base : std::nullopt, ColorSource::AutomaticSample},
        {fallback, ColorSource::StableFallback}
    }};
    const auto color = std::find_if(colors.begin(), colors.end(), [](const auto& item) { return item.color.has_value(); });
    if (!Opaque(*color->color)) return ResolutionError::InvalidColor;
    const std::array<PatternCandidate, 7> patterns{{
        {std::move(global), PatternSource::GlobalSelection},
        {selected ? std::optional<PatternChoice>(AuthoredChart{selected->chart_name}) : std::nullopt, PatternSource::SelectedTheme},
        {personal ? personal->pattern : std::nullopt, PatternSource::Personal},
        {authored ? std::optional<PatternChoice>(AuthoredChart{authored->chart_name}) : std::nullopt, PatternSource::AuthoredApplication},
        {!selected && sample ? std::optional<PatternChoice>(GeneratedPattern{sample->seed, sample->glyph}) : std::nullopt, PatternSource::AutomaticSample},
        {application ? std::optional<PatternChoice>(GeneratedPattern{StableHash(application->Key()), {}}) : std::nullopt, PatternSource::GeneratedFallback},
        {PlainPattern{}, PatternSource::PlainFallback}
    }};
    const auto pattern = std::find_if(patterns.begin(), patterns.end(), [](const auto& item) { return item.pattern.has_value(); });
    if (auto error = ValidatePattern(*pattern->pattern)) return *error;
    auto policy = ChartColorPolicy::LiteralAuthored;
    if (std::holds_alternative<PlainPattern>(*pattern->pattern)) policy = ChartColorPolicy::NoChart;
    else if (std::holds_alternative<PersonalChart>(*pattern->pattern)) policy = ChartColorPolicy::PreservePersonalPixels;
    else if (std::holds_alternative<ZigzagPattern>(*pattern->pattern) || std::holds_alternative<GeneratedPattern>(*pattern->pattern))
        policy = ChartColorPolicy::GeneratedContrast;
    const bool sampled = pattern->source == PatternSource::AutomaticSample && !personal_base;
    std::optional<std::uint32_t> contrast, accent;
    if (policy == ChartColorPolicy::GeneratedContrast) {
        const std::array<std::optional<std::uint32_t>, 3> contrasts{
            personal ? personal->colors.contrast_argb : std::nullopt,
            sampled ? std::optional(sample->contrast_argb) : std::nullopt,
            ZigzagContrast(*color->color)
        };
        contrast = *std::find_if(contrasts.begin(), contrasts.end(), [](const auto& item) { return item.has_value(); });
        if (!Opaque(*contrast)) return ResolutionError::InvalidColor;
    }
    if (std::holds_alternative<GeneratedPattern>(*pattern->pattern)) {
        accent = sampled ? sample->accent_argb : icon_yarn::FallbackAccent(*color->color, *contrast);
        if (!Opaque(*accent)) return ResolutionError::InvalidColor;
    }
    const auto* theme = selected ? selected : authored;
    return ResolvedAppearance{*color->color, contrast, accent, *pattern->pattern, color->source, pattern->source, policy,
        theme ? std::optional<std::string_view>(theme->id) : std::nullopt, mapping};
}
}
