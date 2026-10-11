#pragma once
#include "Session.h"
#include <algorithm>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace window_sweaters::settings {
inline bool ApplicationPresentationReady(bool loaded,bool hasRoot,double width,double height) noexcept {
    return loaded && hasRoot && width>0 && height>0;
}
inline void RetainApplicationPublication(std::optional<AppsConnectionState>& pending,AppsConnectionState const& state) {
    if (!pending || state.connection.publication_sequence>pending->connection.publication_sequence) pending=state;
}
inline bool SameAppearance(contracts::Configuration const& a,contracts::Configuration const& b) noexcept {
    return a.paused==b.paused && a.border_width_dip==b.border_width_dip &&
        a.design_selection==b.design_selection && a.design_id==b.design_id &&
        a.stitch_style==b.stitch_style && a.material_style==b.material_style &&
        a.stitch_rows==b.stitch_rows && a.yarn_argb==b.yarn_argb && a.imports==b.imports;
}
enum class AppItemChange { remove, insert, move };
enum class AppFocusControl { toggle, remove_choice, defaults };
inline AppFocusControl FocusAfterApplicationUpdate(AppFocusControl focused,bool ownerPresent,bool choiceVisible) noexcept {
    if (!ownerPresent) return AppFocusControl::defaults;
    return focused==AppFocusControl::remove_choice && !choiceVisible?AppFocusControl::toggle:focused;
}
enum class ChartMutation { none, add, replace, remove, select, clear };
inline ChartMutation ClassifyChartMutation(contracts::ImportMutation const& mutation) noexcept {
    if (std::holds_alternative<contracts::AddPng>(mutation)) return ChartMutation::add;
    if (std::holds_alternative<contracts::ReplacePng>(mutation)) return ChartMutation::replace;
    if (std::holds_alternative<contracts::RemovePng>(mutation)) return ChartMutation::remove;
    if (auto select=std::get_if<contracts::SelectPng>(&mutation))
        return select->id.empty()?ChartMutation::clear:ChartMutation::select;
    return ChartMutation::none;
}
struct ChartActionState { bool use_chart,manage_chart,clear_chart; };
// The placeholder (no known chosen chart) can never queue an empty SelectPng;
// clearing a manual import is only the separate Clear imported chart action.
inline ChartActionState ChartActions(bool editable,bool chosen_known,bool chosen_is_active,bool manual_chart_active) noexcept {
    return {editable && chosen_known && !chosen_is_active,editable && chosen_known,editable && manual_chart_active};
}
// Chart to highlight (never apply) once the host acknowledges an add or replace;
// a user choice made after the mutation was queued always wins.
inline std::string ChartToHighlight(ChartMutation kind,std::string const& added_name,std::string const& replaced_id,
    std::span<contracts::ChartMetadata const> before,std::span<contracts::ChartMetadata const> after,
    bool user_chose_since_queued,bool saved) {
    if (!saved || user_chose_since_queued) return {};
    if (kind==ChartMutation::replace)
        return std::ranges::any_of(after,[&](auto const& chart) { return chart.id==replaced_id; })?replaced_id:std::string{};
    if (kind!=ChartMutation::add) return {};
    std::vector<contracts::ChartMetadata const*> added;
    for (auto const& chart:after)
        if (std::ranges::none_of(before,[&](auto const& old) { return old.id==chart.id; })) added.push_back(&chart);
    for (auto const* chart:added) if (chart->name==added_name) return chart->id;
    return added.size()==1?added.front()->id:std::string{};
}
inline double PreservedViewportOffset(double captured,double scrollableHeight) noexcept {
    return std::clamp(captured,0.0,std::max(0.0,scrollableHeight));
}
// Tracks whether the rules editor holds text the host has not saved. Only a
// settled SetRulesText or ReadRulesFile may replace it, and never over a
// newer user edit made after that mutation was queued.
class RulesDraft {
public:
    enum class Kind { none, other, save_text, read_file };
    static Kind KindOf(contracts::ImportMutation const& mutation) noexcept {
        return std::holds_alternative<contracts::SetRulesText>(mutation)?Kind::save_text:
            std::holds_alternative<contracts::ReadRulesFile>(mutation)?Kind::read_file:Kind::other;
    }
    bool Dirty() const noexcept { return dirty_; }
    void Edit() noexcept { ++revision_; dirty_=true; }
    void Begin(Kind kind) noexcept { pending_=kind; queued_revision_=revision_; }
    void Settle(bool saved) noexcept {
        auto kind=std::exchange(pending_,Kind::none);
        if (saved && (kind==Kind::save_text || kind==Kind::read_file) && revision_==queued_revision_) dirty_=false;
    }
private:
    std::uint64_t revision_{},queued_revision_{};
    Kind pending_{Kind::none};
    bool dirty_{};
};
struct AppItemEdit {
    AppItemChange change;
    std::size_t index,destination;
};
inline std::vector<AppItemEdit> ReconcileApplicationItems(std::vector<applications::AppIdentity> current,
    std::span<contracts::ApplicationRow const> desired) {
    std::vector<AppItemEdit> edits;
    if (std::ranges::equal(current,desired,{},{},&contracts::ApplicationRow::identity)) return edits;
    for (std::size_t index=0;index<desired.size();++index) {
        auto found=std::find(current.begin()+std::min(index,current.size()),current.end(),desired[index].identity);
        if (found==current.end()) {
            current.insert(current.begin()+index,desired[index].identity);
            edits.push_back({AppItemChange::insert,index,index});
        } else {
            auto previous=static_cast<std::size_t>(found-current.begin());
            if (previous!=index) {
                auto identity=*found;
                current.erase(found); current.insert(current.begin()+index,std::move(identity));
                edits.push_back({AppItemChange::move,previous,index});
            }
        }
    }
    while (current.size()>desired.size()) {
        auto index=current.size()-1;
        current.pop_back(); edits.push_back({AppItemChange::remove,index,index});
    }
    return edits;
}
}
