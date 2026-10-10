#include "ProcessMetadata.h"

#include <appmodel.h>
#include <array>
#include <vector>

namespace window_sweaters::applications {
namespace {
MetadataUnavailable Failure(QueryStage stage, DWORD error) {
    const auto reason = error == ERROR_ACCESS_DENIED ? UnavailableReason::AccessDenied :
        (error == ERROR_INVALID_HANDLE || error == ERROR_INVALID_PARAMETER) ?
        UnavailableReason::ProcessUnavailable : UnavailableReason::QueryFailed;
    return {reason, stage, error};
}

PackagedIdentityQuery QueryAumid(HANDLE process) {
    std::array<wchar_t, 131> buffer{};
    UINT32 size = static_cast<UINT32>(buffer.size());
    const auto error = GetApplicationUserModelId(process, &size, buffer.data());
    if (error == APPMODEL_ERROR_NO_APPLICATION) return NoPackagedIdentity{};
    if (error != ERROR_SUCCESS) return Failure(QueryStage::ApplicationId, static_cast<DWORD>(error));
    if (size < 2 || size > buffer.size() || buffer[size - 1] != L'\0')
        return MetadataUnavailable{UnavailableReason::InvalidMetadata, QueryStage::ApplicationId};
    return PackagedAumid{std::wstring(buffer.data(), size - 1)};
}

ExecutablePathQuery QueryImage(HANDLE process) {
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
        return Failure(QueryStage::ExecutablePath, GetLastError());
    if (size == 0 || size >= buffer.size())
        return MetadataUnavailable{UnavailableReason::InvalidMetadata, QueryStage::ExecutablePath};
    return ExecutablePath{std::wstring(buffer.data(), size)};
}
}

std::optional<MetadataUnavailable> ValidateProcessLifetime(HANDLE process, const ProcessInstance& expected) {
    const DWORD pid = GetProcessId(process);
    if (pid == 0) return Failure(QueryStage::ProcessTimes, GetLastError());
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
        return Failure(QueryStage::ProcessTimes, GetLastError());
    const std::uint64_t start = (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
    if (pid != expected.Pid() || start != expected.CreationTime())
        return MetadataUnavailable{UnavailableReason::LifetimeMismatch, QueryStage::ProcessTimes};
    if (exit.dwHighDateTime != 0 || exit.dwLowDateTime != 0)
        return MetadataUnavailable{UnavailableReason::ProcessUnavailable, QueryStage::ProcessTimes};
    return std::nullopt;
}

MetadataResult QueryProcessMetadata(HANDLE process, const ProcessInstance& expected) {
    if (const auto failure = ValidateProcessLifetime(process, expected)) return *failure;
    const auto packaged = QueryAumid(process);
    const auto image = std::holds_alternative<MetadataUnavailable>(packaged) ?
        ExecutablePathQuery{MetadataUnavailable{UnavailableReason::QueryFailed, QueryStage::ExecutablePath}} :
        QueryImage(process);
    if (const auto failure = ValidateProcessLifetime(process, expected)) return *failure;
    return ResolveMetadata(packaged, image);
}

MetadataResult QueryProcessMetadata(const ProcessInstance& expected) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, expected.Pid());
    if (!process) return Failure(QueryStage::OpenProcess, GetLastError());
    struct Close final {
        HANDLE handle;
        ~Close() { CloseHandle(handle); }
    } close{process};
    return QueryProcessMetadata(process, expected);
}

}
