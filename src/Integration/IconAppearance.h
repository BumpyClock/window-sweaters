#pragma once

#include "..\Applications\AppIdentity.h"
#include "..\IconYarn\Preparation.h"
#include "..\Colorways\Resolver.h"
#include "..\Knitting\KnitTile.h"
#include <windows.h>
#include <stdexcept>

namespace window_sweaters::integration {

// Kind is part of the salt. Executable equality is Windows ordinal-insensitive;
// package AUMIDs are exact. Neither a source path nor a process lifetime is a salt.
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

inline std::optional<colorways::AutomaticAppearance> AutomaticProposal(
    const icon_yarn::IconInput& input, const icon_yarn::PaletteResult& result) {
    const auto* palette = std::get_if<icon_yarn::SampledPalette>(&result);
    if (!palette) return {};
    return colorways::AutomaticAppearance{palette->baseArgb, palette->contrastArgb,
        std::string(icon_yarn::MotifName(icon_yarn::StableMotif(input.canonicalAppIdentity)))};
}

inline std::shared_ptr<const knitting::Chart> MakeGeneratedChart(
    std::string_view name, std::uint32_t contrast) {
    if ((contrast >> 24) != 255 || (name != "zigzag" && name != "picnic" && name != "twinkle"))
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

} // namespace window_sweaters::integration
