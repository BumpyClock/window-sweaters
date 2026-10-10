#pragma once

#include "MetadataWorker.h"

#include <map>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace window_sweaters::applications {

struct SetApplicationPreference { AppIdentity identity; bool enabled; };
struct RemoveApplicationPreference { AppIdentity identity; };
using ApplicationPreferenceEdit = std::variant<SetApplicationPreference, RemoveApplicationPreference>;
struct EligibilityMutation {
    // A default-only edit preserves explicit choices, even ones now equal to the default.
    // All-apps-off/on uses {defaultEnabled=false/true, clearExceptions=true} as one transaction.
    std::optional<bool> defaultEnabled;
    bool clearExceptions{};
    std::vector<ApplicationPreferenceEdit> edits;
};
enum class PreferenceMutationError { TooManyExceptions, DuplicateIdentityEdit };

class EligibilityPreferences final {
public:
    using ExceptionMap = std::map<AppIdentity, bool>;
    bool DefaultEnabled() const noexcept { return defaultEnabled_; }
    const ExceptionMap& Exceptions() const noexcept { return exceptions_; }
    // This is the preference predicate for the central tracker/Host gate, not another window gate.
    // Unknown metadata fails closed without creating an exception or erasing an existing choice.
    bool Allows(const AppIdentity& identity) const noexcept;
    bool Allows(const MetadataResult& metadata) const noexcept;
    bool operator==(const EligibilityPreferences&) const = default;
    std::variant<EligibilityPreferences, PreferenceMutationError> Apply(
        const EligibilityMutation& mutation) const;
    static constexpr std::size_t MaximumExceptions = 4096;

private:
    bool defaultEnabled_{true};
    ExceptionMap exceptions_;
};

// Input is the central tracker's supported ordinary-window set, including preference-disabled owners.
// Host supplies only accepted, lifetime-current metadata at this revision. No window enumeration or
// eligibility evaluation occurs here. Metadata-unavailable owners get no persistent mutation key.
// Verified window properties may identify different applications within the same host process;
// processCount is distinct root process lifetimes per application, not exclusive process ownership.
struct RunningApplicationWindow {
    TargetToken token;
    ImmutableMetadata metadata;
};
enum class ApplicationPresence { Running, SavedNotRunning };
struct ApplicationSnapshotRow {
    AppIdentity identity;
    ApplicationPresence presence;
    bool enabled;
    bool explicitPreference;
    std::size_t windowCount;
    std::size_t processCount;
    ImmutableMetadata metadata; // Absent for a saved exception; never fabricate display/alias metadata.
};
struct UnavailableOwner {
    ProcessInstance process;
    MetadataUnavailable unavailable;
    std::size_t windowCount;
};
struct ApplicationSnapshot {
    std::uint64_t configurationRevision;
    std::vector<ApplicationSnapshotRow> applications;
    std::vector<UnavailableOwner> unavailableOwners;
};
enum class SnapshotError {
    TooManyWindows, StaleRevision, ConflictingWindowLifetime, ConflictingMetadata, MissingMetadata
};
using SnapshotResult = std::variant<ApplicationSnapshot, SnapshotError>;

SnapshotResult BuildApplicationSnapshot(const EligibilityPreferences& preferences,
    std::uint64_t configurationRevision, std::span<const RunningApplicationWindow> windows);

} // namespace window_sweaters::applications
