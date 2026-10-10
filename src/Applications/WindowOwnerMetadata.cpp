#include "WindowOwnerMetadata.h"
#include "ProcessMetadata.h"

#include <array>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <wrl/client.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace window_sweaters::applications {
namespace {
MetadataUnavailable WindowFailure(UnavailableReason reason, QueryStage stage, DWORD error = 0) {
    return {reason, stage, error};
}

std::variant<bool, MetadataUnavailable> IsSystemFrameHost(const AppIdentity& identity) {
    if (identity.Kind() != IdentityKind::NormalizedExecutablePath) return false;
    std::array<wchar_t, MAX_PATH> directory{};
    const auto size = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (size == 0 || size >= directory.size())
        return WindowFailure(UnavailableReason::QueryFailed, QueryStage::ExecutablePath,
            size == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    const auto host = AppIdentity::FromExecutablePath(
        std::wstring(directory.data(), size) + L"\\ApplicationFrameHost.exe");
    if (!host) return WindowFailure(UnavailableReason::InvalidMetadata, QueryStage::ExecutablePath);
    return identity == *host;
}

class NativeWindowOwnerSource final : public WindowOwnerSource {
public:
    explicit NativeWindowOwnerSource(HANDLE process) : process_(process) {}
    WindowLifetimeResult ReadLifetime(const TargetToken& expected) override {
        if (const auto failure = ValidateProcessLifetime(process_, expected.Process())) return *failure;
        const auto hwnd = reinterpret_cast<HWND>(expected.Target().hwnd);
        DWORD pid{};
        const auto thread = GetWindowThreadProcessId(hwnd, &pid);
        if (!thread || pid != expected.Target().pid || GetAncestor(hwnd, GA_ROOT) != hwnd ||
            GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow())
            return WindowFailure(UnavailableReason::LifetimeMismatch, QueryStage::WindowLifetime);
        const auto atom = static_cast<std::uint16_t>(GetClassLongPtrW(hwnd, GCW_ATOM));
        if (!atom || !IsWindow(hwnd))
            return WindowFailure(UnavailableReason::LifetimeMismatch, QueryStage::WindowLifetime);
        if (const auto failure = ValidateProcessLifetime(process_, expected.Process())) return *failure;
        return WindowOwnerLifetime{expected.Process(), thread, atom};
    }
    PackagedIdentityQuery ReadApplicationId(const TargetToken& expected) override {
        Microsoft::WRL::ComPtr<IPropertyStore> store;
        const auto opened = SHGetPropertyStoreForWindow(reinterpret_cast<HWND>(expected.Target().hwnd),
            IID_PPV_ARGS(store.GetAddressOf()));
        if (FAILED(opened))
            return WindowFailure(opened == E_ACCESSDENIED ? UnavailableReason::AccessDenied :
                UnavailableReason::QueryFailed, QueryStage::WindowApplicationId,
                static_cast<DWORD>(opened));
        struct Property final {
            PROPVARIANT value{};
            ~Property() { PropVariantClear(&value); }
        } property;
        const auto read = store->GetValue(PKEY_AppUserModel_ID, &property.value);
        if (FAILED(read))
            return WindowFailure(read == E_ACCESSDENIED ? UnavailableReason::AccessDenied :
                UnavailableReason::QueryFailed, QueryStage::WindowApplicationId, static_cast<DWORD>(read));
        if (property.value.vt == VT_EMPTY) return NoPackagedIdentity{};
        if (property.value.vt != VT_LPWSTR || !property.value.pwszVal)
            return WindowFailure(UnavailableReason::InvalidMetadata, QueryStage::WindowApplicationId);
        std::size_t size{};
        while (size <= 130 && property.value.pwszVal[size] != L'\0') ++size;
        if (size == 0 || size > 130)
            return WindowFailure(UnavailableReason::InvalidMetadata, QueryStage::WindowApplicationId);
        return PackagedAumid{std::wstring(property.value.pwszVal, size)};
    }
    MetadataResult ReadProcessMetadata(const ProcessInstance& expected) override {
        return QueryProcessMetadata(process_, expected);
    }
private:
    HANDLE process_;
};

struct ProcessHandle final {
    HANDLE value;
    ~ProcessHandle() { if (value) CloseHandle(value); }
};

MetadataUnavailable OpenFailure() {
    const auto error = GetLastError();
    return WindowFailure(error == ERROR_ACCESS_DENIED ? UnavailableReason::AccessDenied :
        UnavailableReason::ProcessUnavailable, QueryStage::OpenProcess, error);
}

bool Matches(const WindowOwnerLifetime& lifetime, const TargetToken& expected) {
    return lifetime.process == expected.Process() && lifetime.threadId != 0 && lifetime.classAtom != 0;
}
}

TargetToken::TargetToken(tracking::TargetId target, std::uint64_t configurationRevision)
    : target_(target), configurationRevision_(configurationRevision) {}
std::optional<TargetToken> TargetToken::FromTarget(tracking::TargetId target, std::uint64_t revision) {
    if (target.hwnd == 0 || target.generation == 0 ||
        !ProcessInstance::FromValues(target.pid, target.processStart)) return std::nullopt;
    return TargetToken(target, revision);
}
ProcessInstance TargetToken::Process() const {
    return *ProcessInstance::FromValues(target_.pid, target_.processStart);
}

MetadataResult ResolveWindowOwnerMetadata(
    const PackagedIdentityQuery& windowApplicationId, const MetadataResult& rootProcess) {
    if (const auto* failure = std::get_if<MetadataUnavailable>(&windowApplicationId)) return *failure;
    if (const auto* failure = std::get_if<MetadataUnavailable>(&rootProcess)) return *failure;
    const auto& root = std::get<AppMetadata>(rootProcess);
    if (root.Provenance() == MetadataProvenance::WindowApplicationId)
        return WindowFailure(UnavailableReason::InvalidMetadata, QueryStage::Resolver);
    // Unpackaged apps may set an explicit window AUMID (Chromium: "MSEdge.udd.Default") that names no package.
    const auto* packaged = std::get_if<PackagedAumid>(&windowApplicationId);
    if (auto identity = packaged ? AppIdentity::FromPackagedAumid(packaged->value) : std::nullopt) {
        const bool packagedRoot = root.Identity().Kind() == IdentityKind::PackagedAumid;
        if (packagedRoot && root.Identity() != *identity)
            return WindowFailure(UnavailableReason::ConflictingOwner, QueryStage::WindowApplicationId);
        auto alias = packagedRoot ? root.ExecutableAlias() : std::nullopt;
        auto display = alias ? *alias : identity->Key().substr(identity->Key().find(L'!') + 1);
        return AppMetadata(std::move(*identity), MetadataProvenance::WindowApplicationId,
            std::move(display), alias ? DisplayNameProvenance::ExecutableBasename :
            DisplayNameProvenance::PackageRelativeApplicationId, std::move(alias));
    }
    const auto hosted = IsSystemFrameHost(root.Identity());
    if (const auto* failure = std::get_if<MetadataUnavailable>(&hosted)) return *failure;
    if (std::get<bool>(hosted))
        return WindowFailure(UnavailableReason::OwnerUnverified, QueryStage::WindowApplicationId);
    return root;
}

MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected, WindowOwnerSource& source) {
    const auto before = source.ReadLifetime(expected);
    if (const auto* failure = std::get_if<MetadataUnavailable>(&before)) return *failure;
    if (!Matches(std::get<WindowOwnerLifetime>(before), expected))
        return WindowFailure(UnavailableReason::LifetimeMismatch, QueryStage::WindowLifetime);
    const auto applicationId = source.ReadApplicationId(expected);
    const auto process = source.ReadProcessMetadata(expected.Process());
    const auto after = source.ReadLifetime(expected);
    if (const auto* failure = std::get_if<MetadataUnavailable>(&after)) return *failure;
    if (before != after)
        return WindowFailure(UnavailableReason::LifetimeMismatch, QueryStage::WindowLifetime);
    return ResolveWindowOwnerMetadata(applicationId, process);
}

MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected, HANDLE process) {
    NativeWindowOwnerSource source(process);
    return QueryWindowOwnerMetadata(expected, source);
}

MetadataResult QueryWindowOwnerMetadata(const TargetToken& expected) {
    ProcessHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expected.Target().pid)};
    if (!process.value) return OpenFailure();
    return QueryWindowOwnerMetadata(expected, process.value);
}

WindowLifetimeResult QueryWindowOwnerLifetime(const TargetToken& expected) {
    ProcessHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expected.Target().pid)};
    if (!process.value) return OpenFailure();
    NativeWindowOwnerSource source(process.value);
    return source.ReadLifetime(expected);
}

}
