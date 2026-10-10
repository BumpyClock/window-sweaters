#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace window_sweaters::applications {

enum class IdentityKind { PackagedAumid, NormalizedExecutablePath };

// Persistence stores Kind()+Key() and restores through the corresponding validated factory.
class AppIdentity final {
public:
    static std::optional<AppIdentity> FromPackagedAumid(std::wstring_view value);
    static std::optional<AppIdentity> FromExecutablePath(std::wstring_view value);
    IdentityKind Kind() const noexcept { return kind_; }
    const std::wstring& Key() const noexcept { return key_; }
    bool operator==(const AppIdentity& other) const noexcept;
    bool operator<(const AppIdentity& other) const noexcept;

private:
    AppIdentity(IdentityKind kind, std::wstring key);
    int Compare(const AppIdentity& other) const noexcept;
    IdentityKind kind_;
    std::wstring key_;
};

class ProcessInstance final {
public:
    static std::optional<ProcessInstance> FromValues(std::uint32_t pid, std::uint64_t creationTime);
    std::uint32_t Pid() const noexcept { return pid_; }
    std::uint64_t CreationTime() const noexcept { return creationTime_; }
    bool operator==(const ProcessInstance&) const = default;
    bool operator<(const ProcessInstance& other) const noexcept;

private:
    ProcessInstance(std::uint32_t pid, std::uint64_t creationTime);
    std::uint32_t pid_;
    std::uint64_t creationTime_;
};

enum class UnavailableReason {
    AccessDenied, ProcessUnavailable, LifetimeMismatch, QueryFailed, InvalidMetadata,
    ResourceLimit, QueryException, OwnerUnverified, ConflictingOwner
};
enum class QueryStage {
    OpenProcess, ProcessTimes, ApplicationId, ExecutablePath, Resolver, Worker,
    WindowLifetime, WindowApplicationId
};
struct MetadataUnavailable {
    UnavailableReason reason;
    QueryStage stage;
    std::uint32_t nativeError{};
    bool operator==(const MetadataUnavailable&) const = default;
};

struct NoPackagedIdentity {};
struct PackagedAumid { std::wstring value; };
struct ExecutablePath { std::wstring value; };
using PackagedIdentityQuery = std::variant<NoPackagedIdentity, PackagedAumid, MetadataUnavailable>;
using ExecutablePathQuery = std::variant<ExecutablePath, MetadataUnavailable>;

enum class MetadataProvenance { ProcessApplicationId, ProcessImageName, WindowApplicationId };
enum class DisplayNameProvenance { ExecutableBasename, PackageRelativeApplicationId };

class AppMetadata final {
public:
    const AppIdentity& Identity() const noexcept { return identity_; }
    MetadataProvenance Provenance() const noexcept { return provenance_; }
    const std::wstring& DisplayName() const noexcept { return displayName_; }
    DisplayNameProvenance DisplayProvenance() const noexcept { return displayProvenance_; }
    const std::optional<std::wstring>& ExecutableAlias() const noexcept { return executableAlias_; }
    std::wstring_view ExecutableSource() const noexcept {
        if (identity_.Kind() == IdentityKind::NormalizedExecutablePath) return identity_.Key();
        return executableSource_ ? std::wstring_view(*executableSource_) : std::wstring_view{};
    }
    AppMetadata WithVerifiedProcessSource(const AppMetadata& process) const;
    std::size_t RetainedBytes() const noexcept;

private:
    friend std::variant<AppMetadata, MetadataUnavailable> ResolveMetadata(
        const PackagedIdentityQuery&, const ExecutablePathQuery&);
    friend std::variant<AppMetadata, MetadataUnavailable> ResolveWindowOwnerMetadata(
        const PackagedIdentityQuery&, const std::variant<AppMetadata, MetadataUnavailable>&);
    AppMetadata(AppIdentity identity, MetadataProvenance provenance, std::wstring displayName,
                DisplayNameProvenance displayProvenance, std::optional<std::wstring> executableAlias,
                std::optional<std::wstring> executableSource = {});
    AppIdentity identity_;
    MetadataProvenance provenance_;
    std::wstring displayName_;
    DisplayNameProvenance displayProvenance_;
    std::optional<std::wstring> executableAlias_;
    std::optional<std::wstring> executableSource_;
};

using MetadataResult = std::variant<AppMetadata, MetadataUnavailable>;

MetadataResult ResolveMetadata(const PackagedIdentityQuery& packaged, const ExecutablePathQuery& image);

}
