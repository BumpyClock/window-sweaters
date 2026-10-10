#include "AppIdentity.h"

#include <Windows.h>
#include <appmodel.h>
#include <algorithm>
#include <array>
#include <utility>

namespace window_sweaters::applications {
namespace {
constexpr std::size_t maxPathUnits = 32767;
constexpr std::size_t maxAumidUnits = 130;

bool ValidUtf16(std::wstring_view value) noexcept {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto unit = static_cast<std::uint16_t>(value[i]);
        if (unit < 32 || unit == 127) return false;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (++i == value.size()) return false;
            const auto low = static_cast<std::uint16_t>(value[i]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

bool ReservedDosName(std::wstring_view component) noexcept {
    const auto stem = component.substr(0, component.find(L'.'));
    const auto equal = [stem](std::wstring_view name) {
        return CompareStringOrdinal(stem.data(), static_cast<int>(stem.size()),
            name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    };
    if (equal(L"CON") || equal(L"PRN") || equal(L"AUX") || equal(L"NUL") ||
        equal(L"CONIN$") || equal(L"CONOUT$")) return true;
    if (stem.size() != 4) return false;
    const auto prefix = stem.substr(0, 3);
    const bool numbered = (CompareStringOrdinal(prefix.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(prefix.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL);
    return numbered && ((stem[3] >= L'1' && stem[3] <= L'9') ||
        stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3');
}

std::optional<std::wstring> NormalizePath(std::wstring_view input) {
    if (input.empty() || input.size() > maxPathUnits || !ValidUtf16(input)) return std::nullopt;
    std::wstring path(input);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        path = L"\\\\" + path.substr(8);
    } else if (path.starts_with(L"\\\\?\\")) {
        path.erase(0, 4);
    }

    const bool drive = path.size() >= 4 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
    const bool unc = path.starts_with(L"\\\\") && !path.starts_with(L"\\\\.\\") &&
        !path.starts_with(L"\\\\?\\");
    if (!drive && !unc) return std::nullopt;
    std::size_t offset = drive ? 3 : 2;
    std::size_t components = 0;
    while (offset < path.size()) {
        const auto end = path.find(L'\\', offset);
        const auto count = (end == std::wstring::npos ? path.size() : end) - offset;
        const auto component = std::wstring_view(path).substr(offset, count);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ' ||
            component.find_first_of(L"<>:\"|?*") != std::wstring_view::npos) return std::nullopt;
        // UNC server/share labels are not filesystem leaf components.
        if ((drive || components >= 2) && ReservedDosName(component)) return std::nullopt;
        ++components;
        if (end == std::wstring::npos) break;
        offset = end + 1;
        if (offset == path.size()) return std::nullopt;
    }
    if ((drive && components < 1) || (unc && components < 3)) return std::nullopt;
    if (drive && path[0] >= L'a' && path[0] <= L'z') path[0] -= L'a' - L'A';
    return path;
}

std::wstring Basename(const AppIdentity& path) {
    return path.Key().substr(path.Key().find_last_of(L'\\') + 1);
}
} // namespace

AppIdentity::AppIdentity(IdentityKind kind, std::wstring key) : kind_(kind), key_(std::move(key)) {}

std::optional<AppIdentity> AppIdentity::FromPackagedAumid(std::wstring_view value) {
    if (value.empty() || value.size() > maxAumidUnits || !ValidUtf16(value)) return std::nullopt;
    std::wstring key(value);
    std::array<wchar_t, 256> family{};
    std::array<wchar_t, 256> relative{};
    UINT32 familySize = static_cast<UINT32>(family.size());
    UINT32 relativeSize = static_cast<UINT32>(relative.size());
    if (ParseApplicationUserModelId(key.c_str(), &familySize, family.data(),
                                   &relativeSize, relative.data()) != ERROR_SUCCESS ||
        family[0] == L'\0' || relative[0] == L'\0') return std::nullopt;
    return AppIdentity(IdentityKind::PackagedAumid, std::move(key));
}

std::optional<AppIdentity> AppIdentity::FromExecutablePath(std::wstring_view value) {
    auto normalized = NormalizePath(value);
    if (!normalized) return std::nullopt;
    return AppIdentity(IdentityKind::NormalizedExecutablePath, std::move(*normalized));
}

int AppIdentity::Compare(const AppIdentity& other) const noexcept {
    if (kind_ != other.kind_) return kind_ < other.kind_ ? -1 : 1;
    const int comparison = CompareStringOrdinal(
        key_.data(), static_cast<int>(key_.size()), other.key_.data(),
        static_cast<int>(other.key_.size()), kind_ == IdentityKind::NormalizedExecutablePath);
    return comparison - CSTR_EQUAL;
}
bool AppIdentity::operator==(const AppIdentity& other) const noexcept { return Compare(other) == 0; }
bool AppIdentity::operator<(const AppIdentity& other) const noexcept { return Compare(other) < 0; }

ProcessInstance::ProcessInstance(std::uint32_t pid, std::uint64_t creationTime)
    : pid_(pid), creationTime_(creationTime) {}
std::optional<ProcessInstance> ProcessInstance::FromValues(std::uint32_t pid, std::uint64_t creationTime) {
    if (pid == 0 || creationTime == 0) return std::nullopt;
    return ProcessInstance(pid, creationTime);
}
bool ProcessInstance::operator<(const ProcessInstance& other) const noexcept {
    return pid_ != other.pid_ ? pid_ < other.pid_ : creationTime_ < other.creationTime_;
}

AppMetadata::AppMetadata(AppIdentity identity, MetadataProvenance provenance, std::wstring displayName,
                         DisplayNameProvenance displayProvenance,
                         std::optional<std::wstring> executableAlias,
                         std::optional<std::wstring> executableSource)
    : identity_(std::move(identity)), provenance_(provenance), displayName_(std::move(displayName)),
      displayProvenance_(displayProvenance), executableAlias_(std::move(executableAlias)),
      executableSource_(std::move(executableSource)) {}

AppMetadata AppMetadata::WithVerifiedProcessSource(const AppMetadata& process) const {
    auto result = *this;
    if (process.Provenance() == MetadataProvenance::ProcessApplicationId &&
        identity_ == process.Identity() && !process.ExecutableSource().empty())
        result.executableSource_ = std::wstring(process.ExecutableSource());
    return result;
}

std::size_t AppMetadata::RetainedBytes() const noexcept {
    return sizeof(AppMetadata) + sizeof(wchar_t) * (identity_.Key().capacity() + 1 +
        displayName_.capacity() + 1 + (executableAlias_ ? executableAlias_->capacity() + 1 : 0) +
        (executableSource_ ? executableSource_->capacity() + 1 : 0));
}

MetadataResult ResolveMetadata(const PackagedIdentityQuery& packaged, const ExecutablePathQuery& image) {
    if (const auto* unavailable = std::get_if<MetadataUnavailable>(&packaged)) return *unavailable;
    std::optional<AppIdentity> path;
    if (const auto* executable = std::get_if<ExecutablePath>(&image)) {
        path = AppIdentity::FromExecutablePath(executable->value);
    }
    if (const auto* aumid = std::get_if<PackagedAumid>(&packaged)) {
        auto identity = AppIdentity::FromPackagedAumid(aumid->value);
        if (!identity) return MetadataUnavailable{UnavailableReason::InvalidMetadata, QueryStage::Resolver};
        auto alias = path ? std::optional<std::wstring>(Basename(*path)) : std::nullopt;
        auto display = alias ? *alias : identity->Key().substr(identity->Key().find(L'!') + 1);
        auto source = path && CompareStringOrdinal(alias->c_str(), -1,
            L"ApplicationFrameHost.exe", -1, TRUE) != CSTR_EQUAL ? std::optional(path->Key()) : std::nullopt;
        return AppMetadata(std::move(*identity), MetadataProvenance::ProcessApplicationId,
            std::move(display), alias ? DisplayNameProvenance::ExecutableBasename :
            DisplayNameProvenance::PackageRelativeApplicationId, std::move(alias),
            std::move(source));
    }
    if (!path) {
        if (const auto* unavailable = std::get_if<MetadataUnavailable>(&image)) return *unavailable;
        return MetadataUnavailable{UnavailableReason::InvalidMetadata, QueryStage::Resolver};
    }
    auto alias = Basename(*path);
    return AppMetadata(std::move(*path), MetadataProvenance::ProcessImageName,
        alias, DisplayNameProvenance::ExecutableBasename, alias);
}

} // namespace window_sweaters::applications
