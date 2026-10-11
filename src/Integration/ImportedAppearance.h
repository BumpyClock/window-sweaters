#pragma once
#include "..\Configuration\ImportState.h"
#include "..\Colorways\Resolver.h"
#include "..\Knitting\KnitTile.h"
#include <map>

namespace window_sweaters::integration {
class ImportedAppearance {
public:
    ImportedAppearance() = default;
    ImportedAppearance(contracts::ImportPreferences const& preferences, configuration::CatalogueSnapshot catalogue,
        ImportedAppearance const* previous=nullptr) : catalogue_(std::move(catalogue)) {
        if (!catalogue_) throw std::invalid_argument("Missing owned import catalogue");
        for (auto const& c:catalogue_->Charts()) {
            const bool outside=designs::ws_design_chart_by_name(c.Name().c_str()) && c.Name().starts_with("atelier-");
            std::shared_ptr<const knitting::Chart> chart;
            if (previous) {
                auto found=previous->charts_.find(c.Id());
                if (found!=previous->charts_.end() && found->second->contentRevision==c.ContentRevision() &&
                    found->second->appearance.outsideAnchored==outside)
                    chart=found->second;
            }
            if (!chart) {
                auto const& m=c.SourceMaterial();
                chart=std::make_shared<const knitting::Chart>(c.Id(),c.ContentRevision(),c.Data().Width(),c.Data().Height(),
                    std::vector<std::uint32_t>(c.Data().Cells().begin(),c.Data().Cells().end()),
                    knitting::ChartAppearance{m.flags,m.corner_argb,m.cuff_argb,outside});
            }
            charts_.emplace(c.Id(),std::move(chart));
        }
        for (auto const& b:preferences.bindings) {
            colorways::PatternChoice pattern=colorways::PlainPattern{};
            if (b.rule.chart_name) {
                if (auto c=catalogue_->FindByName(*b.rule.chart_name))
                    pattern=colorways::PersonalChart{c->Id(),c->ContentRevision()};
                else pattern=colorways::AuthoredChart{*b.rule.chart_name};
            }
            personal_.push_back({b.application,{b.rule.base_argb,{}},std::move(pattern)});
        }
    }
    std::shared_ptr<const knitting::Chart> Find(std::string const& id,std::uint64_t revision=0) const {
        auto found=charts_.find(id);
        return found!=charts_.end() && (!revision || found->second->contentRevision==revision)?found->second:nullptr;
    }
    std::shared_ptr<const knitting::Chart> Named(std::string const& name) const {
        auto source=catalogue_?catalogue_->FindByName(name):nullptr;
        return source?Find(source->Id(),source->ContentRevision()):nullptr;
    }
    std::span<const colorways::PersonalRule> Personal() const noexcept { return personal_; }
private:
    configuration::CatalogueSnapshot catalogue_;
    std::map<std::string,std::shared_ptr<const knitting::Chart>> charts_;
    std::vector<colorways::PersonalRule> personal_;
};
}
