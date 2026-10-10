#include "VisibilityPolicy.h"

#include <algorithm>

namespace window_sweaters::visibility {

bool DesktopId::IsValid() const noexcept {
    return std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte != 0; });
}

Suppression PolicySnapshot::Reason() const noexcept {
    if (!notificationsReady_) return Suppression::NotificationsUnavailable;
    if (paused_) return Suppression::Paused;
    switch (session_) {
    case SessionState::Unknown: return Suppression::SessionUnknown;
    case SessionState::Locked: return Suppression::Locked;
    case SessionState::Disconnected: return Suppression::Disconnected;
    case SessionState::Interactive: break;
    }
    switch (power_) {
    case PowerState::Unknown: return Suppression::PowerUnknown;
    case PowerState::Suspended: return Suppression::Suspended;
    case PowerState::Awake: break;
    }
    switch (display_) {
    case DisplayState::Unknown: return Suppression::DisplayUnknown;
    case DisplayState::Off: return Suppression::DisplayOff;
    case DisplayState::On:
    case DisplayState::Dimmed: return Suppression::None;
    }
    return Suppression::QueryUncertain;
}

PolicyChange VisibilityPolicy::Changed(const PolicySnapshot& previous) noexcept {
    if (snapshot_ == previous) return {snapshot_, false, false};
    ++snapshot_.revision_;
    const bool reconcile = snapshot_.MayWork() && !previous.MayWork();
    return {snapshot_, true, reconcile};
}

PolicyChange VisibilityPolicy::SetSession(SessionState state) noexcept {
    const auto previous = snapshot_;
    snapshot_.session_ = state;
    return Changed(previous);
}

PolicyChange VisibilityPolicy::SetPower(PowerState state) noexcept {
    const auto previous = snapshot_;
    snapshot_.power_ = state;
    if (state != PowerState::Awake) snapshot_.display_ = DisplayState::Unknown;
    return Changed(previous);
}

PolicyChange VisibilityPolicy::SetDisplay(DisplayState state) noexcept {
    const auto previous = snapshot_;
    snapshot_.display_ = state;
    return Changed(previous);
}

PolicyChange VisibilityPolicy::SetPaused(bool paused) noexcept {
    const auto previous = snapshot_;
    snapshot_.paused_ = paused;
    return Changed(previous);
}

PolicyChange VisibilityPolicy::SetNotificationsReady(bool ready) noexcept {
    const auto previous = snapshot_;
    snapshot_.notificationsReady_ = ready;
    return Changed(previous);
}

PolicyChange VisibilityPolicy::Reset() noexcept {
    const auto previous = snapshot_;
    snapshot_.session_ = SessionState::Unknown;
    snapshot_.power_ = PowerState::Unknown;
    snapshot_.display_ = DisplayState::Unknown;
    snapshot_.notificationsReady_ = false;
    return Changed(previous);
}

bool TargetVisibility::IsCurrent(const PolicySnapshot& policy, const ProbeToken& token) const noexcept {
    return token.target == target_ && token.policyRevision == policy.Revision()
        && policyRevision_ == policy.Revision() && token.sequence == sequence_ && sequence_ != 0;
}

Transition TargetVisibility::Suppress(Suppression reason, bool accepted) noexcept {
    const bool hide = shown_;
    shown_ = false;
    pendingAssociation_.reset();
    return {accepted, hide, false, false, reason, {target_, policyRevision_, sequence_}, std::nullopt};
}

Transition TargetVisibility::ApplyPolicy(const PolicySnapshot& policy) noexcept {
    if (policy.Revision() < policyRevision_) return {false};
    if (policyRevision_ == policy.Revision()) {
        return {true, false, false, false, policy.Reason(),
                {target_, policyRevision_, sequence_}, std::nullopt};
    }
    policyRevision_ = policy.Revision();
    ++sequence_;
    // Even an enabling change needs a fresh observation before old work can publish.
    const auto reason = policy.MayWork() ? Suppression::QueryUncertain : policy.Reason();
    return Suppress(reason);
}

ProbeToken TargetVisibility::BeginProbe(const PolicySnapshot& policy) noexcept {
    // ApplyPolicy's hide effect must be consumed by the host before beginning work.
    if (policyRevision_ != policy.Revision()) return {target_, policy.Revision(), 0};
    ++sequence_;
    pendingAssociation_.reset();
    return {target_, policyRevision_, sequence_};
}

Transition TargetVisibility::Reconcile(const PolicySnapshot& policy,
                                      const Observation& observation) noexcept {
    if (!IsCurrent(policy, observation.token)) return {false};
    if (!policy.MayWork()) return Suppress(policy.Reason());
    if (!observation.window || !observation.window->desktop.IsValid())
        return Suppress(Suppression::QueryUncertain);
    const auto& window = *observation.window;
    if (!window.onCurrentDesktop) return Suppress(Suppression::OffDesktop);
    if (!window.visible) return Suppress(Suppression::Hidden);
    if (window.minimized) return Suppress(Suppression::Minimized);
    if (window.cloaked) return Suppress(Suppression::Cloaked);
    if (window.maximized) return Suppress(Suppression::Maximized);
    if (window.fullscreen) return Suppress(Suppression::Fullscreen);
    if (window.overlay == OverlayAssociation::Uncertain)
        return Suppress(Suppression::QueryUncertain);
    if (window.overlay != OverlayAssociation::Matching) {
        auto transition = Suppress(Suppression::OverlayUnassociated);
        pendingAssociation_ = window.desktop;
        transition.associate = true;
        transition.desktop = window.desktop;
        return transition;
    }
    pendingAssociation_.reset();
    const bool show = !shown_;
    shown_ = true;
    return {true, false, false, show, Suppression::None, observation.token, window.desktop};
}

Transition TargetVisibility::CompleteAssociation(const PolicySnapshot& policy, const ProbeToken& token,
                                               const DesktopId& desktop, bool success) noexcept {
    if (!CanAssociate(policy, token, desktop)) return {false};
    pendingAssociation_.reset();
    if (!success) return Suppress(Suppression::QueryUncertain);
    const bool show = !shown_;
    shown_ = true;
    return {true, false, false, show, Suppression::None, token, desktop};
}

bool TargetVisibility::CanAssociate(const PolicySnapshot& policy, const ProbeToken& token,
                                   const DesktopId& desktop) const noexcept {
    return policy.MayWork() && IsCurrent(policy, token)
        && pendingAssociation_ && *pendingAssociation_ == desktop;
}

bool TargetVisibility::CanPublish(const PolicySnapshot& policy, const ProbeToken& token) const noexcept {
    return shown_ && policy.MayWork() && IsCurrent(policy, token);
}

bool PollGate::Due(Milliseconds now, const PolicySnapshot& policy) noexcept {
    if (policyRevision_ != policy.Revision()) {
        policyRevision_ = policy.Revision();
        lastPoll_.reset();
    }
    if (!policy.MayWork()) return false;
    if (lastPoll_ && now >= *lastPoll_ && now - *lastPoll_ < PollIntervalMs) return false;
    lastPoll_ = now;
    return true;
}

} // namespace window_sweaters::visibility
