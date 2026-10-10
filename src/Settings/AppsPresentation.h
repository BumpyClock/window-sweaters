#pragma once
#include "Session.h"
#include <algorithm>
#include <span>
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
        a.stitch_rows==b.stitch_rows && a.yarn_argb==b.yarn_argb;
}
enum class AppItemChange { remove, insert, move };
enum class AppFocusControl { toggle, remove_choice, defaults };
inline AppFocusControl FocusAfterApplicationUpdate(AppFocusControl focused,bool ownerPresent,bool choiceVisible) noexcept {
    if (!ownerPresent) return AppFocusControl::defaults;
    return focused==AppFocusControl::remove_choice && !choiceVisible?AppFocusControl::toggle:focused;
}
inline double PreservedViewportOffset(double captured,double scrollableHeight) noexcept {
    return std::clamp(captured,0.0,std::max(0.0,scrollableHeight));
}
struct AppItemEdit {
    AppItemChange change;
    std::size_t index,destination;
};
// Native controls stay keyed by AppIdentity; only membership/order changes
// touch the items collection. Metadata, toggles and acknowledgments do not.
inline std::vector<AppItemEdit> ReconcileApplicationItems(std::vector<applications::AppIdentity> current,
    std::span<contracts::ApplicationRow const> desired) {
    std::vector<AppItemEdit> edits;
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
