#include "ImportState.h"
#include <set>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"windowscodecs.lib")

namespace window_sweaters::configuration {
using namespace imports;
using namespace contracts;
void DescribeCatalogue(ImportPreferences& p, ImportCatalogue const& catalogue) {
    p.charts.clear(); p.high_water=catalogue.RevisionHighWater();
    for (auto const& c:catalogue.Charts())
        p.charts.push_back({c.Id(),c.Name(),c.Revision(),c.ContentRevision(),c.Data().Width(),c.Data().Height()});
}
namespace {
bool KnownChart(std::string const& name, ImportCatalogue const& catalogue) {
    return catalogue.FindByName(name) || designs::ws_design_chart_by_name(name.c_str());
}
bool ValidBindings(ImportPreferences const& p, ImportCatalogue const& catalogue) {
    if (!p.selected_id.empty() && !catalogue.FindById(p.selected_id)) return false;
    std::set<applications::AppIdentity> ids;
    for (auto const& b:p.bindings)
        if (!ids.insert(b.application).second || (b.rule.base_argb>>24)!=255 ||
            (b.rule.chart_name && !KnownChart(*b.rule.chart_name,catalogue))) return false;
    return true;
}
}
bool ValidImportState(ImportPreferences const& p, ImportCatalogue const& catalogue) {
    auto parsed=ParseRules(p.document);
    auto rules=std::get_if<ParsedRules>(&parsed);
    if (!rules || *rules!=catalogue.PersonalRules() || rules->Rules().size()!=p.diagnostics.size() ||
        !ValidBindings(p,catalogue)) return false;
    auto described=p; DescribeCatalogue(described,catalogue);
    if (described!=p) return false;
    for (auto const& b:p.bindings) {
        if (b.explicit_edit) { if (!b.rule.original_prefix.empty()) return false; continue; }
        bool found=false;
        for (std::size_t i=0;i<rules->Rules().size();++i) {
            auto const& d=p.diagnostics[i];
            if (rules->Rules()[i]==b.rule && d.match==RuleMatch::matched &&
                d.candidates.size()==1 && d.candidates[0]==b.application) found=true;
        }
        if (!found) return false;
    }
    try {
        payload::Writer w; payload::Imports(w,p);
        payload::Reader r(w.bytes); ImportPreferences decoded;
        return payload::Imports(r,decoded) && r.Done() && decoded==p;
    } catch (std::length_error const&) { return false; }
}
Result<ImportCandidate> PrepareImport(ImportPreferences const& committed,
    CatalogueSnapshot const& catalogue, ImportMutation const& mutation,
    std::span<const applications::AppMetadata> metadata) {
    auto p=committed;
    CatalogueSnapshot next=catalogue;
    std::optional<Edit> edit;
    std::optional<std::string> document;
    if (auto text=std::get_if<SetRulesText>(&mutation)) document=text->document;
    else if (auto file=std::get_if<ReadRulesFile>(&mutation)) {
        auto read=ImportRulesFile(file->path);
        if (auto error=std::get_if<Error>(&read)) return *error;
        document=std::get<std::string>(std::move(read));
    } else if (auto add=std::get_if<AddPng>(&mutation)) {
        auto read=ImportPngFile(add->path);
        if (auto error=std::get_if<Error>(&read)) return *error;
        edit=AddChart{add->name,std::get<FileImport>(std::move(read)).chart};
    } else if (auto replace=std::get_if<ReplacePng>(&mutation)) {
        if (replace->name) {
            auto current=catalogue->FindById(replace->id);
            auto outside=[](std::string const& name) {
                return designs::ws_design_chart_by_name(name.c_str()) && name.starts_with("atelier-");
            };
            if (current && outside(current->Name())!=outside(*replace->name))
                return Error{ErrorCode::InvalidName,"Rename would change authored source anchoring; retain the semantic name"};
        }
        auto read=ImportPngFile(replace->path);
        if (auto error=std::get_if<Error>(&read)) return *error;
        edit=ReplaceChart{replace->id,std::get<FileImport>(std::move(read)).chart,replace->name};
    } else if (auto remove=std::get_if<RemovePng>(&mutation)) {
        edit=RemoveChart{remove->id};
        if (p.selected_id==remove->id) p.selected_id.clear();
    } else if (auto select=std::get_if<SelectPng>(&mutation)) {
        if (!select->id.empty() && !catalogue->FindById(select->id))
            return Error{ErrorCode::UnknownChart,"Selected chart is not imported"};
        p.selected_id=select->id;
    } else {
        auto const& color=std::get<SetAppColor>(mutation);
        if ((color.argb>>24)!=255) return Error{ErrorCode::InvalidRule,"Application RGB must be opaque"};
        std::optional<std::string> chart;
        if (color.chart_id) {
            auto c=catalogue->FindById(*color.chart_id);
            if (!c) return Error{ErrorCode::UnknownChart,"Application chart is not imported"};
            chart=c->Name();
        }
        auto found=std::find_if(p.bindings.begin(),p.bindings.end(),[&](auto const& b) {return b.application==color.application;});
        PersonalBinding b{color.application,{"",color.argb,std::move(chart)},true};
        if (found==p.bindings.end()) p.bindings.push_back(std::move(b)); else *found=std::move(b);
    }
    if (document) {
        auto parsed=ParseRules(*document);
        if (auto error=std::get_if<Error>(&parsed)) return *error;
        auto& rules=std::get<ParsedRules>(parsed);
        auto plan=PlanRuleImport(rules,metadata);
        if (auto error=std::get_if<Error>(&plan)) return *error;
        p.document=*document; p.diagnostics.clear();
        std::erase_if(p.bindings,[](auto const& b) {return !b.explicit_edit;});
        for (auto const& resolution:std::get<RuleImportPlan>(plan).resolutions) {
            RuleDiagnostic d;
            if (auto ambiguous=std::get_if<Ambiguous>(&resolution.match)) {
                d.match=RuleMatch::ambiguous; d.candidates=ambiguous->candidates;
            } else if (auto matched=std::get_if<Matched>(&resolution.match)) {
                d.match=RuleMatch::matched; d.candidates.push_back(matched->application);
            }
            p.diagnostics.push_back(std::move(d));
        }
        for (auto const& b:std::get<RuleImportPlan>(plan).bindings) {
            if (std::none_of(p.bindings.begin(),p.bindings.end(),[&](auto const& own) {return own.application==b.application;}))
                p.bindings.push_back({b.application,b.rule,false});
        }
        edit=SetRules{std::move(rules)};
    }
    if (edit) {
        auto built=BuildCandidate(*catalogue,*edit);
        if (auto error=std::get_if<Error>(&built)) return *error;
        next=std::make_shared<const ImportCatalogue>(std::get<ImportCatalogue>(std::move(built)));
    }
    DescribeCatalogue(p,*next);
    if (!ValidBindings(p,*next)) return Error{ErrorCode::UnknownChart,"Chart is still referenced by a saved application color"};
    try { payload::Writer w; payload::Imports(w,p); }
    catch (std::length_error const&) { return Error{ErrorCode::ResourceLimit,"Import metadata exceeds 256 KiB or 128 bindings"}; }
    return ImportCandidate{std::move(p),std::move(next)};
}
}
