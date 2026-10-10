#include "TrackingModel.h"

#include <algorithm>
#include <array>
#include <utility>
#include <stdexcept>

namespace window_sweaters::tracking {
namespace {

WindowHandle HandleOf(const Observation& observation) {
    return std::visit([](const auto& value) { return value.hwnd; }, observation);
}

bool ExcludedClass(const std::wstring& name, const Policy& policy) {
    static constexpr std::array classes{
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"DV2ControlHost", L"tooltips_class32", L"#32768", L"IME", L"MSCTFIME UI",
        L"WindowSweaters.Host", L"WindowSweaters.Overlay", L"WindowSweaters.Settings",
    };
    if (std::find(classes.begin(), classes.end(), name) != classes.end()) {
        return true;
    }
    if (policy.mode == TrackingMode::Production
        && (name == L"WindowSweaters.TestFixture" || name == L"WindowSweaters.TestFixtureReceiver")) {
        return true;
    }
    return std::find(policy.excludedClasses.begin(), policy.excludedClasses.end(), name)
        != policy.excludedClasses.end();
}

} // namespace

Eligibility Evaluate(const Observation& observation, const Policy& policy) {
    if (std::holds_alternative<DestroyedWindow>(observation)) {
        return Eligibility::Destroyed;
    }
    if (std::holds_alternative<UncertainWindow>(observation)) {
        return Eligibility::Uncertain;
    }
    const auto& window = std::get<AliveWindow>(observation);
    if (!window.hwnd || !window.identity.pid || !window.identity.processStart
        || !window.identity.observedIdentity || window.className.empty() || !window.dpi
        || window.rect.Width() <= 0 || window.rect.Height() <= 0) {
        return Eligibility::Uncertain;
    }
    if (window.identity.pid == policy.hostPid
        || std::find(policy.excludedPids.begin(), policy.excludedPids.end(), window.identity.pid)
            != policy.excludedPids.end()
        || !window.ordinaryTopLevel || ExcludedClass(window.className, policy)
        || (policy.mode == TrackingMode::FixtureOnly
            && (window.hwnd != policy.fixtureTarget
                || (policy.fixtureIdentity && window.identity != *policy.fixtureIdentity)))
        || (policy.mode == TrackingMode::SelectedOnly &&
            std::none_of(policy.selectedTargets.begin(), policy.selectedTargets.end(), [&](const auto& selected) {
                return selected.hwnd == window.hwnd && selected.identity == window.identity;
            }))) {
        return Eligibility::Unsupported;
    }
    if (!window.visible || window.minimized || window.cloaked || window.maximized || window.fullscreen) {
        return Eligibility::Suppressed;
    }
    return Eligibility::Eligible;
}

bool MatchesFixtureSelection(const AliveWindow& window, const WindowIdentity& selected,
                             std::wstring_view title) noexcept {
    return window.identity == selected && selected.pid && selected.processStart && selected.observedIdentity
        && window.className == L"WindowSweaters.TestFixture" && title == L"WindowSweaters Fixture";
}

TrackingModel::TrackingModel(Policy policy) : policy_(std::move(policy)) {
    if (policy_.mode != TrackingMode::SelectedOnly) {
        if (!policy_.selectedTargets.empty()) throw std::invalid_argument("Selected targets require SelectedOnly tracking");
        return;
    }
    if (policy_.selectedTargets.empty() || policy_.selectedTargets.size() > MaximumSelectedTargets ||
        policy_.selectedTargets.size() > policy_.maxTargets)
        throw std::invalid_argument("Invalid selected target count");
    for (std::size_t i=0;i<policy_.selectedTargets.size();++i) {
        const auto& selected=policy_.selectedTargets[i];
        if (!selected.hwnd || !selected.identity.pid || !selected.identity.processStart || !selected.identity.observedIdentity)
            throw std::invalid_argument("Invalid selected target lifetime");
        for (std::size_t j=0;j<i;++j)
            if (policy_.selectedTargets[j].hwnd==selected.hwnd) throw std::invalid_argument("Duplicate selected HWND");
    }
    retiredSelections_.reserve(policy_.selectedTargets.size());
}

bool TrackingModel::SelectionActive(WindowHandle hwnd) const noexcept {
    return policy_.mode != TrackingMode::SelectedOnly ||
        (std::any_of(policy_.selectedTargets.begin(),policy_.selectedTargets.end(),
            [hwnd](const auto& selected) { return selected.hwnd==hwnd; }) &&
         std::find(retiredSelections_.begin(),retiredSelections_.end(),hwnd)==retiredSelections_.end());
}
void TrackingModel::RetireSelection(WindowHandle hwnd) {
    if (policy_.mode==TrackingMode::SelectedOnly && SelectionActive(hwnd)) retiredSelections_.push_back(hwnd);
}

Action TrackingModel::MakeAction(ActionKind kind, const Entry& entry, bool rebuild) {
    return {kind, entry.id, entry.snapshot.rect, entry.snapshot.dpi,
            entry.appearanceRevision, rebuild, entry.snapshot.topmost};
}

void TrackingModel::Hide(Entry& entry, ActionBatch& actions) {
    if (entry.shown) {
        actions.push_back(MakeAction(ActionKind::Hide, entry));
        entry.shown = false;
    }
}

ActionBatch TrackingModel::Observe(const Observation& observation, Dirty dirty, Milliseconds now,
                                   std::optional<Milliseconds> createdAt) {
    ActionBatch actions;
    const auto eligibility = Evaluate(observation, policy_);
    const auto hwnd = HandleOf(observation);
    if (policy_.mode==TrackingMode::SelectedOnly && !SelectionActive(hwnd)) return actions;
    auto found = targets_.find(hwnd);
    if (eligibility == Eligibility::Destroyed) {
        RetireSelection(hwnd);
        if (found != targets_.end()) {
            if (found->second.created) {
                actions.push_back(MakeAction(ActionKind::Remove, found->second));
            }
            targets_.erase(found);
        }
        return actions;
    }
    if (eligibility == Eligibility::Uncertain) {
        if (found != targets_.end()) {
            found->second.eligibility = eligibility;
            if (Has(dirty, Dirty::Appearance)) {
                ++found->second.appearanceRevision;
            }
            Hide(found->second, actions);
        }
        return actions;
    }

    const auto& snapshot = std::get<AliveWindow>(observation);
    if (policy_.mode==TrackingMode::SelectedOnly) {
        const auto selected=std::find_if(policy_.selectedTargets.begin(),policy_.selectedTargets.end(),
            [hwnd](const auto& value) { return value.hwnd==hwnd; });
        if (snapshot.identity!=selected->identity ||
            (found!=targets_.end() && createdAt && *createdAt>found->second.bornAt && *createdAt<=now)) {
            RetireSelection(hwnd);
            if (found!=targets_.end()) {
                if (found->second.created) actions.push_back(MakeAction(ActionKind::Remove,found->second));
                targets_.erase(found);
            }
            return actions;
        }
    }
    // A CREATE newer than our first observation also detects same-process/same-class HWND reuse.
    // Duplicate or delayed CREATE events predating this generation cannot replace it.
    if (found != targets_.end()
        && (found->second.snapshot.identity != snapshot.identity
            || (createdAt && *createdAt > found->second.bornAt && *createdAt <= now))) {
        if (found->second.created) {
            actions.push_back(MakeAction(ActionKind::Remove, found->second));
        }
        targets_.erase(found);
        found = targets_.end();
    }
    if (found == targets_.end()) {
        if (eligibility == Eligibility::Unsupported || targets_.size() >= policy_.maxTargets) {
            return actions;
        }
        Entry entry;
        entry.id = {hwnd, snapshot.identity.pid, snapshot.identity.processStart, nextGeneration_++};
        entry.snapshot = snapshot;
        entry.eligibility = eligibility;
        entry.bornAt = now;
        found = targets_.emplace(hwnd, std::move(entry)).first;
    }

    auto& entry = found->second;
    const auto previous = entry.snapshot;
    const bool sizeChanged = previous.rect.Width() != snapshot.rect.Width()
        || previous.rect.Height() != snapshot.rect.Height();
    const bool geometryChanged = previous.rect != snapshot.rect;
    const bool appearanceChanged = sizeChanged || previous.dpi != snapshot.dpi
        || Has(dirty, Dirty::Appearance);
    const bool orderChanged = previous.topmost != snapshot.topmost || Has(dirty, Dirty::Ordering);
    entry.snapshot = snapshot;
    entry.eligibility = eligibility;
    entry.orderPending = entry.orderPending || orderChanged;
    if (appearanceChanged) {
        ++entry.appearanceRevision;
    }
    if (sizeChanged) {
        entry.lastSizeChange = now;
    } else if (entry.lastSizeChange && now >= *entry.lastSizeChange
               && now - *entry.lastSizeChange >= policy_.resizeQuietPeriod) {
        entry.lastSizeChange.reset();
    }

    // MOVESIZESTART/END are hints only. A stable sample after a quiet interval ends resizing,
    // even when END is lost; END alone cannot prematurely expose a still-changing size.
    const bool mayShow = eligibility == Eligibility::Eligible && !entry.lastSizeChange;
    if (!mayShow) {
        Hide(entry, actions);
    }
    if (entry.created && (geometryChanged || appearanceChanged)) {
        const bool rebuild = mayShow && appearanceChanged;
        actions.push_back(MakeAction(ActionKind::Move, entry, rebuild));
        if (rebuild) {
            entry.publishedAppearanceRevision = entry.appearanceRevision;
        }
    }
    if (mayShow) {
        if (!entry.created) {
            actions.push_back(MakeAction(ActionKind::Create, entry, true));
            entry.created = true;
            entry.publishedAppearanceRevision = entry.appearanceRevision;
            entry.orderPending = true;
        }
        if (!entry.shown) {
            const bool rebuild = entry.publishedAppearanceRevision != entry.appearanceRevision;
            actions.push_back(MakeAction(ActionKind::Show, entry, rebuild));
            entry.publishedAppearanceRevision = entry.appearanceRevision;
            entry.shown = true;
            entry.orderPending = true;
        }
        if (entry.orderPending) {
            actions.push_back(MakeAction(ActionKind::Reorder, entry));
            entry.orderPending = false;
        }
    }
    return actions;
}

ActionBatch TrackingModel::Clear() {
    ActionBatch actions;
    for (const auto& [hwnd, entry] : targets_) {
        (void)hwnd;
        if (entry.created) {
            actions.push_back(MakeAction(ActionKind::Remove, entry));
        }
    }
    targets_.clear();
    if (policy_.mode==TrackingMode::SelectedOnly)
        for (const auto& selected:policy_.selectedTargets) RetireSelection(selected.hwnd);
    return actions;
}

bool TrackingModel::Accepts(const WorkToken& token) const noexcept {
    const auto found = targets_.find(token.target.hwnd);
    return found != targets_.end() && found->second.id == token.target
        && found->second.appearanceRevision == token.appearanceRevision
        && found->second.eligibility == Eligibility::Eligible
        && !found->second.lastSizeChange && found->second.created;
}

std::optional<TargetSnapshot> TrackingModel::CurrentSnapshot(const WorkToken& token) const noexcept {
    const auto found = targets_.find(token.target.hwnd);
    if (found == targets_.end() || found->second.id != token.target
        || found->second.appearanceRevision != token.appearanceRevision) return std::nullopt;
    const auto& entry = found->second;
    return TargetSnapshot{entry.id, entry.snapshot.rect, entry.snapshot.dpi, entry.appearanceRevision,
        entry.snapshot.topmost, entry.shown && entry.eligibility == Eligibility::Eligible
            && !entry.lastSizeChange};
}

std::optional<TargetId> TrackingModel::Current(WindowHandle hwnd) const {
    const auto found = targets_.find(hwnd);
    return found == targets_.end() ? std::nullopt : std::optional{found->second.id};
}

std::vector<WindowHandle> TrackingModel::Handles() const {
    std::vector<WindowHandle> result;
    result.reserve(targets_.size());
    for (const auto& [hwnd, entry] : targets_) {
        (void)entry;
        result.push_back(hwnd);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<WindowHandle> TrackingModel::SettlingHandles() const {
    std::vector<WindowHandle> result;
    for (const auto& [hwnd, entry] : targets_) {
        if (entry.lastSizeChange) {
            result.push_back(hwnd);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool TrackingModel::SealFixtureIdentity(const WindowIdentity& identity) {
    if (policy_.mode != TrackingMode::FixtureOnly || !targets_.empty()
        || !identity.pid || !identity.processStart || !identity.observedIdentity) {
        return false;
    }
    policy_.fixtureIdentity = identity;
    return true;
}

} // namespace window_sweaters::tracking
