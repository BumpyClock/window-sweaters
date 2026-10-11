#pragma once
#include "..\Imports\Imports.h"
#include "ApplicationPayload.h"
#include <algorithm>
#include <windows.h>

namespace window_sweaters::contracts {
inline constexpr std::size_t maximum_import_metadata_bytes = 256 * 1024;
inline constexpr std::size_t maximum_personal_bindings = 128;
struct ChartMetadata {
    std::string id, name, hash;
    std::uint64_t revision{};
    std::uint32_t width{}, height{};
    bool operator==(ChartMetadata const&) const = default;
};
enum class RuleMatch : std::uint32_t { unmatched, ambiguous, matched };
struct RuleDiagnostic {
    RuleMatch match{};
    std::vector<applications::AppIdentity> candidates;
    bool operator==(RuleDiagnostic const&) const = default;
};
struct PersonalBinding {
    applications::AppIdentity application;
    imports::Rule rule;
    bool explicit_edit{};
    bool operator==(PersonalBinding const&) const = default;
};
struct ImportPreferences {
    std::string document;
    std::vector<PersonalBinding> bindings;
    std::vector<RuleDiagnostic> diagnostics;
    std::vector<ChartMetadata> charts;
    std::uint64_t high_water{};
    std::string selected_id;
    bool operator==(ImportPreferences const&) const = default;
};
struct SetRulesText { std::string document; bool operator==(SetRulesText const&) const = default; };
struct ReadRulesFile { std::wstring path; bool operator==(ReadRulesFile const&) const = default; };
struct AddPng { std::string name; std::wstring path; bool operator==(AddPng const&) const = default; };
struct ReplacePng {
    std::string id; std::wstring path; std::optional<std::string> name;
    bool operator==(ReplacePng const&) const = default;
};
struct RemovePng { std::string id; bool operator==(RemovePng const&) const = default; };
struct SelectPng { std::string id; bool operator==(SelectPng const&) const = default; };
struct SetAppColor {
    applications::AppIdentity application;
    std::uint32_t argb;
    std::optional<std::string> chart_id;
    bool operator==(SetAppColor const&) const = default;
};
using ImportMutation = std::variant<SetRulesText, ReadRulesFile, AddPng, ReplacePng, RemovePng, SelectPng, SetAppColor>;
struct ImportFailure {
    std::uint32_t code{}, line{};
    std::string reason;
    bool operator==(ImportFailure const&) const = default;
};
namespace payload {
inline void Utf8(Writer& w, std::string const& text, std::size_t maximum) {
    if (text.size()>maximum || text.size()+4>w.limit-w.bytes.size())
        throw std::length_error("Import text exceeds bound");
    w.Number(text.size());
    for (unsigned char c:text) w.Number(c,1);
}
inline bool Utf8(Reader& r, std::string& text, std::size_t maximum) {
    std::uint64_t size{};
    if (!r.Number(size) || size>maximum || size>r.bytes.size()-r.offset) return false;
    text.clear(); text.reserve(static_cast<std::size_t>(size));
    for (std::size_t i=0;i<size;++i) {
        auto c=std::to_integer<unsigned char>(r.bytes[r.offset++]);
        if (!c) return false;
        text+=static_cast<char>(c);
    }
    return text.empty() || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),
        static_cast<int>(text.size()),nullptr,0)>0;
}
inline void Rule(Writer& w, imports::Rule const& rule) {
    Utf8(w,rule.original_prefix,63); w.Number(rule.base_argb);
    w.Number(rule.chart_name.has_value()); if (rule.chart_name) Utf8(w,*rule.chart_name,63);
}
inline bool Rule(Reader& r, imports::Rule& rule) {
    std::uint64_t argb{}, present{};
    if (!Utf8(r,rule.original_prefix,63) || !r.Number(argb) || (argb>>24)!=255 ||
        !r.Number(present) || present>1) return false;
    rule.base_argb=static_cast<std::uint32_t>(argb); rule.chart_name.reset();
    if (present) {
        std::string name; if (!Utf8(r,name,63) || name.empty()) return false;
        rule.chart_name=std::move(name);
    }
    return true;
}
inline void Imports(Writer& w, ImportPreferences const& p) {
    Utf8(w,p.document,imports::maximum_rules_bytes);
    w.Number(p.high_water,8); Utf8(w,p.selected_id,80);
    if (p.bindings.size()>maximum_personal_bindings || p.diagnostics.size()>imports::maximum_rules ||
        p.charts.size()>imports::maximum_effective_charts) throw std::length_error("Import metadata count exceeds bound");
    w.Number(p.bindings.size());
    for (auto const& b:p.bindings) { w.Identity(b.application); Rule(w,b.rule); w.Number(b.explicit_edit); }
    w.Number(p.diagnostics.size());
    for (auto const& d:p.diagnostics) {
        w.Number(static_cast<unsigned>(d.match)); w.Number(d.candidates.size());
        for (auto const& i:d.candidates) w.Identity(i);
    }
    w.Number(p.charts.size());
    for (auto const& c:p.charts) {
        Utf8(w,c.id,80); Utf8(w,c.name,63); Utf8(w,c.hash,71); w.Number(c.revision,8);
        w.Number(c.width); w.Number(c.height);
    }
    if (w.bytes.size()>maximum_import_metadata_bytes) throw std::length_error("Import metadata exceeds 256 KiB");
}
inline bool Imports(Reader& r, ImportPreferences& p) {
    std::uint64_t count{}, flag{}, mode{};
    if (!Utf8(r,p.document,imports::maximum_rules_bytes) || !r.Number(p.high_water,8) ||
        !Utf8(r,p.selected_id,80) || !r.Number(count) || count>maximum_personal_bindings) return false;
    p.bindings.clear(); p.diagnostics.clear(); p.charts.clear();
    std::set<applications::AppIdentity> identities;
    for (std::size_t i=0;i<count;++i) {
        auto identity=r.Identity(); imports::Rule rule{};
        if (!identity || !identities.insert(*identity).second || !Rule(r,rule) || !r.Number(flag) || flag>1) return false;
        p.bindings.push_back({*identity,std::move(rule),flag==1});
    }
    if (!r.Number(count) || count>imports::maximum_rules) return false;
    for (std::size_t i=0;i<count;++i) {
        std::uint64_t candidates{};
        if (!r.Number(mode) || mode>2 || !r.Number(candidates) || candidates>4096 ||
            (mode==0 && candidates) || (mode==1 && candidates<2) || (mode==2 && candidates!=1)) return false;
        RuleDiagnostic d{static_cast<RuleMatch>(mode),{}};
        for (std::size_t j=0;j<candidates;++j) {
            auto identity=r.Identity(); if (!identity) return false;
            if (!d.candidates.empty() && !(d.candidates.back()<*identity)) return false;
            d.candidates.push_back(*identity);
        }
        p.diagnostics.push_back(std::move(d));
    }
    if (!r.Number(count) || count>imports::maximum_effective_charts) return false;
    std::set<std::string> ids,names;
    std::set<std::uint64_t> revisions;
    for (std::size_t i=0;i<count;++i) {
        ChartMetadata c; std::uint64_t width{},height{};
        if (!Utf8(r,c.id,80) || c.id.size()!=80 || !c.id.starts_with("imported:sha256:") ||
            !Utf8(r,c.name,63) || c.name.empty() || !Utf8(r,c.hash,71) ||
            c.hash.size()!=71 || !c.hash.starts_with("sha256:") ||
            !r.Number(c.revision,8) || !c.revision || c.revision>p.high_water ||
            !revisions.insert(c.revision).second || !r.Number(width) || !width || width>256 ||
            !r.Number(height) || !height || height>256 || !ids.insert(c.id).second || !names.insert(c.name).second) return false;
        c.width=static_cast<std::uint32_t>(width); c.height=static_cast<std::uint32_t>(height);
        p.charts.push_back(std::move(c));
    }
    if (!p.selected_id.empty() && !ids.contains(p.selected_id)) return false;
    return true;
}
inline void ImportEdit(Writer& w, ImportMutation const& edit) {
    w.Number(edit.index());
    std::visit([&](auto const& e) {
        using T=std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T,SetRulesText>) Utf8(w,e.document,imports::maximum_rules_bytes);
        else if constexpr (std::is_same_v<T,ReadRulesFile>) w.Text(e.path,32760);
        else if constexpr (std::is_same_v<T,AddPng>) { Utf8(w,e.name,63); w.Text(e.path,32760); }
        else if constexpr (std::is_same_v<T,ReplacePng>) {
            Utf8(w,e.id,80); w.Text(e.path,32760); w.Number(e.name.has_value());
            if (e.name) Utf8(w,*e.name,63);
        } else if constexpr (std::is_same_v<T,SetAppColor>) {
            w.Identity(e.application); w.Number(e.argb); w.Number(e.chart_id.has_value());
            if (e.chart_id) Utf8(w,*e.chart_id,80);
        } else Utf8(w,e.id,80);
    },edit);
}
inline bool ImportEdit(Reader& r, ImportMutation& edit) {
    std::uint64_t kind{},present{},argb{}; std::string text; std::wstring path;
    if (!r.Number(kind) || kind>6) return false;
    if (kind==0) { if (!Utf8(r,text,imports::maximum_rules_bytes)) return false; edit=SetRulesText{std::move(text)}; }
    else if (kind==1) { if (!r.Text(path,32760)) return false; edit=ReadRulesFile{std::move(path)}; }
    else if (kind==2) {
        if (!Utf8(r,text,63) || !r.Text(path,32760)) return false; edit=AddPng{std::move(text),std::move(path)};
    } else if (kind==3) {
        if (!Utf8(r,text,80) || !r.Text(path,32760) || !r.Number(present) || present>1) return false;
        std::optional<std::string> name;
        if (present) { std::string n; if (!Utf8(r,n,63)) return false; name=std::move(n); }
        edit=ReplacePng{std::move(text),std::move(path),std::move(name)};
    } else if (kind==4 || kind==5) {
        if (!Utf8(r,text,80)) return false;
        if (kind==4) edit=RemovePng{std::move(text)}; else edit=SelectPng{std::move(text)};
    } else {
        auto identity=r.Identity();
        if (!identity || !r.Number(argb) || (argb>>24)!=255 || !r.Number(present) || present>1) return false;
        std::optional<std::string> chart;
        if (present) { if (!Utf8(r,text,80)) return false; chart=std::move(text); }
        edit=SetAppColor{*identity,static_cast<std::uint32_t>(argb),std::move(chart)};
    }
    return true;
}
}
}
