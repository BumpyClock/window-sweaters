#include "EligibilityPreferences.h"

#include <set>

namespace window_sweaters::applications {

bool EligibilityPreferences::Allows(const AppIdentity& identity) const noexcept {
    const auto found = exceptions_.find(identity);
    return found == exceptions_.end() ? defaultEnabled_ : found->second;
}
bool EligibilityPreferences::Allows(const MetadataResult& metadata) const noexcept {
    const auto* known = std::get_if<AppMetadata>(&metadata);
    return known && Allows(known->Identity());
}
std::variant<EligibilityPreferences, PreferenceMutationError> EligibilityPreferences::Apply(
    const EligibilityMutation& mutation) const {
    if (mutation.edits.size() > MaximumExceptions)
        return PreferenceMutationError::TooManyExceptions;
    std::set<AppIdentity> edited;
    EligibilityPreferences next = *this;
    if (mutation.defaultEnabled) next.defaultEnabled_ = *mutation.defaultEnabled;
    if (mutation.clearExceptions) next.exceptions_.clear();
    for (const auto& edit : mutation.edits) {
        const auto& identity = std::visit([](const auto& value) -> const AppIdentity& {
            return value.identity;
        }, edit);
        if (!edited.insert(identity).second) return PreferenceMutationError::DuplicateIdentityEdit;
        if (const auto* set = std::get_if<SetApplicationPreference>(&edit))
            next.exceptions_.insert_or_assign(identity, set->enabled);
        else
            next.exceptions_.erase(identity);
    }
    if (next.exceptions_.size() > MaximumExceptions)
        return PreferenceMutationError::TooManyExceptions;
    return next;
}

SnapshotResult BuildApplicationSnapshot(const EligibilityPreferences& preferences,
    std::uint64_t configurationRevision, std::span<const RunningApplicationWindow> windows) {
    if (windows.size() > 4096) return SnapshotError::TooManyWindows;
    struct Running {
        ImmutableMetadata metadata;
        std::size_t count{};
        std::set<ProcessInstance> processes;
    };
    std::map<AppIdentity, Running> running;
    std::map<ProcessInstance, UnavailableOwner> unavailable;
    std::map<tracking::WindowHandle, const RunningApplicationWindow*> observed;
    std::map<ProcessInstance, ImmutableMetadata> processMetadata;
    for (const auto& window : windows) {
        if (window.token.ConfigurationRevision() != configurationRevision) return SnapshotError::StaleRevision;
        if (!window.metadata) return SnapshotError::MissingMetadata;
        const auto process = window.token.Process();
        const auto hwnd = window.token.Target().hwnd;
        const auto previous = observed.find(hwnd);
        if (previous != observed.end()) {
            if (previous->second->token != window.token) return SnapshotError::ConflictingWindowLifetime;
            const auto* previousMetadata = std::get_if<AppMetadata>(previous->second->metadata.get());
            const auto* currentMetadata = std::get_if<AppMetadata>(window.metadata.get());
            if ((previousMetadata == nullptr) != (currentMetadata == nullptr) ||
                (previousMetadata && currentMetadata && previousMetadata->Identity() != currentMetadata->Identity()))
                return SnapshotError::ConflictingMetadata;
            continue;
        }
        observed.emplace(hwnd, &window);
        if (const auto* metadata = std::get_if<AppMetadata>(window.metadata.get())) {
            // A process fact is shared; a verified window property is not.
            if (metadata->Provenance() != MetadataProvenance::WindowApplicationId) {
                const auto known = processMetadata.find(process);
                if (known != processMetadata.end() &&
                    std::get<AppMetadata>(*known->second).Identity() != metadata->Identity())
                    return SnapshotError::ConflictingMetadata;
                processMetadata.try_emplace(process, window.metadata);
            }
            auto& owner = running[metadata->Identity()];
            owner.metadata = window.metadata;
            ++owner.count;
            owner.processes.insert(process);
        } else {
            const auto failure = std::get<MetadataUnavailable>(*window.metadata);
            auto [found, inserted] = unavailable.try_emplace(process, UnavailableOwner{process, failure, 0});
            (void)inserted;
            ++found->second.windowCount;
        }
    }
    ApplicationSnapshot snapshot{configurationRevision, {}, {}};
    snapshot.applications.reserve(running.size() + preferences.Exceptions().size());
    for (const auto& [identity, owner] : running) {
        snapshot.applications.push_back({identity, ApplicationPresence::Running, preferences.Allows(identity),
            preferences.Exceptions().contains(identity), owner.count, owner.processes.size(), owner.metadata});
    }
    for (const auto& [identity, enabled] : preferences.Exceptions()) {
        if (!running.contains(identity)) {
            snapshot.applications.push_back({identity, ApplicationPresence::SavedNotRunning, enabled,
                true, 0, 0, {}});
        }
    }
    for (auto& [process, owner] : unavailable) {
        (void)process;
        snapshot.unavailableOwners.push_back(std::move(owner));
    }
    return snapshot;
}

} // namespace window_sweaters::applications
