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
    ImmutableMetadata metadata;
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

}
