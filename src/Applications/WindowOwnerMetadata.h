#pragma once

#include "AppIdentity.h"
#include "..\Tracking\TrackingModel.h"
#include <Windows.h>

namespace window_sweaters::applications {

class TargetToken final {
public:
    static std::optional<TargetToken> FromTarget(tracking::TargetId target, std::uint64_t configurationRevision);
    const tracking::TargetId& Target() const noexcept { return target_; }
    std::uint64_t ConfigurationRevision() const noexcept { return configurationRevision_; }
    ProcessInstance Process() const;
    bool operator==(const TargetToken&) const = default;

private:
    TargetToken(tracking::TargetId target, std::uint64_t configurationRevision);
    tracking::TargetId target_;
    std::uint64_t configurationRevision_;
};

struct WindowOwnerLifetime {
    ProcessInstance process;
    std::uint32_t threadId;
    std::uint16_t classAtom;
    bool operator==(const WindowOwnerLifetime&) const = default;
};
using WindowLifetimeResult = std::variant<WindowOwnerLifetime, MetadataUnavailable>;

class WindowOwnerSource {
public:
    virtual ~WindowOwnerSource() = default;
    virtual WindowLifetimeResult ReadLifetime(const TargetToken& expected) = 0;
    virtual PackagedIdentityQuery ReadApplicationId(const TargetToken& expected) = 0;
    virtual MetadataResult ReadProcessMetadata(const ProcessInstance& expected) = 0;
};

MetadataResult ResolveWindowOwnerMetadata(
    const PackagedIdentityQuery& windowApplicationId, const MetadataResult& rootProcess);
MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected, WindowOwnerSource& source);

MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected, HANDLE process);
MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected);
WindowLifetimeResult QueryWindowOwnerLifetime(const TargetToken& expected);

}
