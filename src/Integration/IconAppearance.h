#pragma once

#include "..\Applications\AppIdentity.h"
#include "..\IconYarn\IconMotif.h"
#include "..\IconYarn\Preparation.h"
#include "..\Colorways\Resolver.h"
#include "..\Knitting\KnitTile.h"
#include <windows.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace window_sweaters::integration {

inline std::variant<icon_yarn::IconInput, icon_yarn::Fallback> MakeIconInput(
    const applications::AppIdentity& identity, std::wstring_view executableSource,
    std::uint64_t iconRevision = 0) {
    using namespace icon_yarn;
    if (executableSource.size() > 4096)
        return Fallback{FallbackReason::ResourceLimit};
    if (executableSource.find(L'\0') != std::wstring_view::npos)
        return Fallback{FallbackReason::UnsupportedSource};
    auto key = identity.Key();
    if (identity.Kind() == applications::IdentityKind::NormalizedExecutablePath) {
        const auto size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
            key.data(), static_cast<int>(key.size()), nullptr, 0, nullptr, nullptr, 0);
        if (!size) return Fallback{FallbackReason::QueryFailed, GetLastError()};
        std::wstring canonical(static_cast<std::size_t>(size), L'\0');
        if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, key.data(),
            static_cast<int>(key.size()), canonical.data(), size, nullptr, nullptr, 0))
            return Fallback{FallbackReason::QueryFailed, GetLastError()};
        key = std::move(canonical);
    }
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, key.data(),
        static_cast<int>(key.size()), nullptr, 0, nullptr, nullptr);
    const std::string_view prefix = identity.Kind() == applications::IdentityKind::PackagedAumid ?
        "aumid:" : "exe:";
    if (!size) return Fallback{FallbackReason::InvalidResource, GetLastError()};
    if (static_cast<std::size_t>(size) > 4096 - prefix.size())
        return Fallback{FallbackReason::ResourceLimit};
    std::string salt(prefix);
    salt.resize(prefix.size() + static_cast<std::size_t>(size));
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, key.data(),
        static_cast<int>(key.size()), salt.data() + prefix.size(), size, nullptr, nullptr))
        return Fallback{FallbackReason::InvalidResource, GetLastError()};
    return IconInput{std::move(salt), iconRevision, std::wstring(executableSource)};
}

inline std::optional<colorways::AutomaticAppearance> AutomaticProposal(const icon_yarn::PaletteResult& result) {
    const auto* palette = std::get_if<icon_yarn::SampledPalette>(&result);
    if (!palette) return {};
    return colorways::AutomaticAppearance{palette->baseArgb, palette->contrastArgb, palette->accentArgb,
        palette->seed, palette->glyph};
}

inline std::shared_ptr<const knitting::Chart> MakeMotifChart(
    std::uint32_t seed, std::string_view glyph, std::uint32_t contrast, std::uint32_t accent) {
    if ((contrast >> 24) != 255 || (accent >> 24) != 255)
        throw std::invalid_argument("Invalid generated motif yarn");
    const auto rows = icon_yarn::GenerateMotif(seed, glyph);
    const auto width = static_cast<std::uint32_t>(rows[0].size());
    std::vector<std::uint32_t> cells;
    cells.reserve(rows.size() * width);
    for (const auto& row : rows)
        for (const char cell : row) cells.push_back(cell == 'a' ? contrast : cell == 'b' ? accent : 0);
    return std::make_shared<const knitting::Chart>("generated-" + std::to_string(seed), 0, width,
        static_cast<std::uint32_t>(rows.size()), std::move(cells),
        knitting::ChartAppearance{designs::ChartDefinedYarn | designs::ChartSculptedYarn});
}

inline std::shared_ptr<const knitting::Chart> MakeGeneratedChart(
    std::string_view name, std::uint32_t contrast) {
    if ((contrast >> 24) != 255 || name != "zigzag")
        throw std::invalid_argument("Invalid generated yarn chart");
    const std::string chartName(name);
    const auto* source = designs::ws_design_chart_by_name(chartName.c_str());
    if (!source) throw std::invalid_argument("Generated yarn chart is unavailable");
    auto literal = knitting::MakeDesignChart(*source, chartName);
    auto cells = literal->argbCells;
    for (auto& cell : cells) if ((cell >> 24) >= 128) cell = contrast;
    return std::make_shared<const knitting::Chart>("automatic-" + chartName, 0,
        literal->width, literal->height, std::move(cells), literal->appearance);
}

}
