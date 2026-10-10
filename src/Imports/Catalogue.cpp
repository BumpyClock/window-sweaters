// SPDX-License-Identifier: GPL-3.0-only
#include "Internal.h"
#include "..\Designs\Catalogue.h"
#include <algorithm>
#include <set>
#include <limits>

namespace window_sweaters::imports {

ImportedChart::ImportedChart(std::string id, std::string name, std::string revision,
    DecodedChart data, Material material, std::uint64_t content_revision)
    : id_(std::move(id)), name_(std::move(name)), revision_(std::move(revision)),
      data_(std::move(data)), material_(material), content_revision_(content_revision) {}

const ImportedChart* ImportCatalogue::FindById(std::string_view id) const noexcept {
    for (const auto& chart : charts_) if (chart.Id() == id) return &chart;
    return nullptr;
}
const ImportedChart* ImportCatalogue::FindByName(std::string_view name) const noexcept {
    for (const auto& chart : charts_) if (chart.Name() == name) return &chart;
    return nullptr;
}

namespace {
constexpr std::string_view id_prefix = "imported:sha256:";
constexpr std::string_view revision_prefix = "sha256:";
constexpr auto all_flags = designs::ChartSolidCorners | designs::ChartOuterCuff |
    designs::ChartRoundDots | designs::ChartFittedRepeat |
    designs::ChartSculptedYarn | designs::ChartDefinedYarn;

bool HashIdentifier(std::string_view value, std::string_view prefix) {
    if (!value.starts_with(prefix) || value.size() != prefix.size() + 64) return false;
    value.remove_prefix(prefix.size());
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool ValidMaterial(Material material) {
    const auto opaque = [](std::uint32_t value) { return value == 0 || (value >> 24) == 255; };
    return (material.flags & ~static_cast<std::uint32_t>(all_flags)) == 0 &&
        opaque(material.corner_argb) && opaque(material.cuff_argb);
}
const designs::ChartView* BuiltIn(std::string_view name) {
    for (std::size_t i = 0; i != designs::ws_design_chart_count(); ++i) {
        const auto* chart = designs::ws_design_chart_at(i);
        if (name == chart->name) return chart;
    }
    return nullptr;
}
Material InitialMaterial(std::string_view name) {
    if (const auto* source = BuiltIn(name))
        return {source->flags, source->corner_argb, source->cuff_argb};
    return {designs::ChartSculptedYarn | designs::ChartDefinedYarn, 0, 0};
}
Result<std::string> Revision(const DecodedChart& data, Material material) {
    auto hash = detail::HashContent(data.Width(), data.Height(), data.Cells(), material);
    if (const auto* error = std::get_if<Error>(&hash)) return *error;
    return std::string(revision_prefix) + std::get<std::string>(std::move(hash));
}
bool ValidData(const DecodedChart& data) {
    return data.Width() && data.Height() && data.Width() <= maximum_chart_axis &&
        data.Height() <= maximum_chart_axis &&
        data.Cells().size() == static_cast<std::size_t>(data.Width()) * data.Height() &&
        !data.EncodedPng().empty() && data.EncodedPng().size() <= maximum_png_bytes &&
        HashIdentifier(data.ContentHash(), "");
}
Result<std::monostate> Validate(const ImportCatalogue& catalogue) {
    std::set<std::string> names, ids;
    std::set<std::uint64_t> revisions;
    std::size_t total_bytes = 0, effective_count = designs::ws_design_chart_count();
    for (const auto& chart : catalogue.Charts()) {
        if (!detail::ValidChartName(chart.Name()))
            return Error{ErrorCode::InvalidName, "Chart names need 1..63 UTF-8 token bytes"};
        if (!names.insert(chart.Name()).second)
            return Error{ErrorCode::DuplicateName, "Duplicate semantic chart name; replace explicitly"};
        if (!ids.insert(chart.Id()).second)
            return Error{ErrorCode::DuplicateIdentity, "Duplicate imported content identity"};
        if (!chart.ContentRevision() || chart.ContentRevision() > catalogue.RevisionHighWater() ||
            !revisions.insert(chart.ContentRevision()).second)
            return Error{ErrorCode::InvalidStoredChart, "Asset revisions must be unique and within the catalogue high-water mark"};
        if (!BuiltIn(chart.Name())) ++effective_count;
        const auto size = chart.Data().EncodedPng().size() + chart.Data().Cells().size_bytes();
        if (size > maximum_catalogue_bytes - total_bytes)
            return Error{ErrorCode::CatalogueLimit, "Imported chart data exceeds aggregate 16 MiB limit"};
        total_bytes += size;
    }
    if (effective_count > maximum_effective_charts || catalogue.Charts().size() > maximum_effective_charts)
        return Error{ErrorCode::CatalogueLimit, "Effective chart catalogue exceeds 128 entries"};
    std::size_t line = 0;
    for (const auto& rule : catalogue.PersonalRules().Rules()) {
        ++line;
        if (rule.chart_name && !BuiltIn(*rule.chart_name) && !catalogue.FindByName(*rule.chart_name))
            return Error{ErrorCode::UnknownChart, "Personal rule refers to a missing semantic chart", line};
    }
    return std::monostate{};
}
}

struct CatalogueAccess {
    static Result<std::uint64_t> AllocateRevision(ImportCatalogue& candidate) {
        if (candidate.revision_high_water_ == (std::numeric_limits<std::uint64_t>::max)())
            return Error{ErrorCode::RevisionOverflow, "Catalogue asset revision high-water mark is exhausted"};
        return ++candidate.revision_high_water_;
    }

    static Result<ImportCatalogue> EditCatalogue(const ImportCatalogue& committed, const Edit& edit) {
        ImportCatalogue candidate = committed;
        if (const auto* add = std::get_if<AddChart>(&edit)) {
            if (!ValidData(add->data))
                return Error{ErrorCode::InvalidStoredChart, "Decoded chart value is empty or moved-from"};
            if (!detail::ValidChartName(add->name))
                return Error{ErrorCode::InvalidName, "Invalid semantic chart name"};
            if (candidate.FindByName(add->name))
                return Error{ErrorCode::DuplicateName, "Name already exists; use explicit replacement"};
            const auto material = InitialMaterial(add->name);
            auto revision = Revision(add->data, material);
            if (const auto* error = std::get_if<Error>(&revision)) return *error;
            const auto id = std::string(id_prefix) +
                std::get<std::string>(revision).substr(revision_prefix.size());
            if (candidate.FindById(id))
                return Error{ErrorCode::DuplicateIdentity, "Identical content and material are already imported; use its identity"};
            const auto content_revision = AllocateRevision(candidate);
            if (const auto* error = std::get_if<Error>(&content_revision)) return *error;
            candidate.charts_.push_back(ImportedChart(id, add->name,
                std::get<std::string>(std::move(revision)), add->data, material,
                std::get<std::uint64_t>(content_revision)));
        } else if (const auto* replace = std::get_if<ReplaceChart>(&edit)) {
            if (!ValidData(replace->data))
                return Error{ErrorCode::InvalidStoredChart, "Replacement chart value is empty or moved-from"};
            auto chart = std::find_if(candidate.charts_.begin(), candidate.charts_.end(),
                [&](const auto& entry) { return entry.Id() == replace->id; });
            if (chart == candidate.charts_.end())
                return Error{ErrorCode::UnknownChart, "Replacement identity is not imported"};
            const auto name = replace->new_name ? *replace->new_name : chart->Name();
            if (!detail::ValidChartName(name))
                return Error{ErrorCode::InvalidName, "Invalid replacement chart name"};
            const auto* named_source = BuiltIn(name);
            if (named_source && chart->Name() != name &&
                chart->SourceMaterial() != InitialMaterial(name))
                return Error{ErrorCode::InvalidStoredChart, "Rename would lose known chart source flags"};
            auto revision = Revision(replace->data, chart->material_);
            if (const auto* error = std::get_if<Error>(&revision)) return *error;
            auto content_revision = chart->content_revision_;
            if (std::get<std::string>(revision) != chart->Revision()) {
                const auto allocated = AllocateRevision(candidate);
                if (const auto* error = std::get_if<Error>(&allocated)) return *error;
                content_revision = std::get<std::uint64_t>(allocated);
            }
            *chart = ImportedChart(chart->id_, name, std::get<std::string>(std::move(revision)),
                replace->data, chart->material_, content_revision);
        } else if (const auto* remove = std::get_if<RemoveChart>(&edit)) {
            const auto chart = std::find_if(candidate.charts_.begin(), candidate.charts_.end(),
                [&](const auto& entry) { return entry.Id() == remove->id; });
            if (chart == candidate.charts_.end())
                return Error{ErrorCode::UnknownChart, "Removal identity is not imported"};
            candidate.charts_.erase(chart);
        } else {
            candidate.rules_ = std::get<SetRules>(edit).rules;
        }
        const auto validation = Validate(candidate);
        if (const auto* error = std::get_if<Error>(&validation)) return *error;
        return candidate;
    }

    static Result<ImportCatalogue> Restore(const StoredCatalogue& stored) {
        if (!stored.revision_high_water)
            return Error{ErrorCode::InvalidStoredChart, "Stored catalogue is missing its durable revision high-water mark"};
        if (stored.charts.size() > maximum_effective_charts)
            return Error{ErrorCode::CatalogueLimit, "Stored chart count exceeds 128"};
        ImportCatalogue candidate;
        candidate.rules_ = stored.rules;
        candidate.revision_high_water_ = *stored.revision_high_water;
        std::size_t total_bytes = 0;
        for (const auto& record : stored.charts) {
            if (!detail::ValidChartName(record.name) || !HashIdentifier(record.id, id_prefix) ||
                !HashIdentifier(record.revision, revision_prefix) || !ValidMaterial(record.material) ||
                !record.content_revision || record.content_revision > candidate.revision_high_water_)
                return Error{ErrorCode::InvalidStoredChart, "Stored chart name, identity, revision, or material is invalid"};
            if (record.encoded_png.size() > maximum_catalogue_bytes - total_bytes)
                return Error{ErrorCode::CatalogueLimit, "Stored encoded charts exceed aggregate limit"};
            auto decoded = DecodePng(record.encoded_png);
            if (const auto* error = std::get_if<Error>(&decoded)) return *error;
            auto data = std::get<DecodedChart>(std::move(decoded));
            const auto size = data.EncodedPng().size() + data.Cells().size_bytes();
            if (size > maximum_catalogue_bytes - total_bytes)
                return Error{ErrorCode::CatalogueLimit, "Stored chart data exceeds aggregate 16 MiB limit"};
            total_bytes += size;
            auto revision = Revision(data, record.material);
            if (const auto* error = std::get_if<Error>(&revision)) return *error;
            if (std::get<std::string>(revision) != record.revision)
                return Error{ErrorCode::RevisionMismatch, "Stored normalized content revision does not match PNG and material"};
            if (BuiltIn(record.name) && record.material != InitialMaterial(record.name))
                return Error{ErrorCode::InvalidStoredChart, "Stored known-chart replacement lost source material flags"};
            candidate.charts_.push_back(ImportedChart(record.id, record.name, record.revision,
                std::move(data), record.material, record.content_revision));
        }
        const auto validation = Validate(candidate);
        if (const auto* error = std::get_if<Error>(&validation)) return *error;
        return candidate;
    }
};

Result<ImportCatalogue> BuildCandidate(const ImportCatalogue& committed, const Edit& edit) {
    return detail::CheckedAllocation<ImportCatalogue>([&] {
        return CatalogueAccess::EditCatalogue(committed, edit);
    });
}
Result<StoredCatalogue> StoreCatalogue(const ImportCatalogue& catalogue) {
    return detail::CheckedAllocation<StoredCatalogue>([&]() -> Result<StoredCatalogue> {
        const auto validation = Validate(catalogue);
        if (const auto* error = std::get_if<Error>(&validation)) return *error;
        StoredCatalogue stored{catalogue.PersonalRules(), {}, catalogue.RevisionHighWater()};
        for (const auto& chart : catalogue.Charts())
            stored.charts.push_back({chart.Id(), chart.Name(), chart.Revision(), chart.SourceMaterial(),
                {chart.Data().EncodedPng().begin(), chart.Data().EncodedPng().end()}, chart.ContentRevision()});
        return stored;
    });
}
Result<ImportCatalogue> RestoreCatalogue(const StoredCatalogue& stored) {
    return detail::CheckedAllocation<ImportCatalogue>([&] {
        return CatalogueAccess::Restore(stored);
    });
}

}
