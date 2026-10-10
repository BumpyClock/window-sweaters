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

// Narrow synchronous test seam. Sources read only the expected root, never enumerate other windows.
// ReadLifetime verifies root HWND/PID/process birth/live state; its fingerprint detects changes
// across lookup. The owner-thread CompletionGate additionally verifies the tracker generation.
class WindowOwnerSource {
public:
    virtual ~WindowOwnerSource() = default;
    virtual WindowLifetimeResult ReadLifetime(const TargetToken& expected) = 0;
    virtual PackagedIdentityQuery ReadApplicationId(const TargetToken& expected) = 0;
    virtual MetadataResult ReadProcessMetadata(const ProcessInstance& expected) = 0;
};

// Window AUMIDs are explicit public property-store facts, not package registration/publisher proof.
// A differing packaged root identity is ambiguous. Only a matching packaged process supplies an
// executable alias; a frame host's executable is never presented as its hosted application's alias.
// A window AUMID that names no package (an unpackaged app's explicit ID) defers to the root process.
MetadataResult ResolveWindowOwnerMetadata(
    const PackagedIdentityQuery& windowApplicationId, const MetadataResult& rootProcess);
MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected, WindowOwnerSource& source);

// Worker-only, caller initialized COM MTA. Public SHGetPropertyStoreForWindow/PKEY_AppUserModel_ID
// plus limited-information process APIs; no titles, mutation, private shell interfaces or child scan.
// Held process handle and root lifetime reads bracket lookup. No atomic HWND lease exists: the
// tracking generation must still be current at completion; a cache must repeat lifetime validation.
// Borrowed handle overload never closes the handle. Native calls examine only the supplied HWND/PID.
MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected, HANDLE process);
MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected);
WindowLifetimeResult QueryWindowOwnerLifetime(const TargetToken& expected);

} // namespace window_sweaters::applications
