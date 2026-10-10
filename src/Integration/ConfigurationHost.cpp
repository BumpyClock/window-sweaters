#include "ConfigurationHost.h"
#include "..\Configuration\Pipe.h"
#include "..\Configuration\Storage.h"
#include <aclapi.h>
#include <deque>
#include <limits>
#include <mutex>
#include <sddl.h>

namespace window_sweaters::integration {
namespace {
using configuration::Handle;
constexpr wchar_t test_executable[] = L"WindowSweaters.ConfigurationHost.Tests.exe";
constexpr wchar_t lock_filename[] = L"configuration.writer.lock";

bool EqualPath(std::filesystem::path const& a, std::filesystem::path const& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool OverlappingPaths(std::filesystem::path const& a, std::filesystem::path const& b) {
    auto first = a.native(), second = b.native();
    while (first.size() > 3 && first.back() == L'\\') first.pop_back();
    while (second.size() > 3 && second.back() == L'\\') second.pop_back();
    if (first.size() > second.size()) first.swap(second);
    if (CompareStringOrdinal(first.c_str(), static_cast<int>(first.size()),
        second.c_str(), static_cast<int>(first.size()), TRUE) != CSTR_EQUAL) return false;
    return first.size() == second.size() || first.back() == L'\\' || second[first.size()] == L'\\';
}
bool LocalAbsolutePath(std::filesystem::path const& path) {
    auto text = path.native();
    if (text.size() < 3 || text.size() >= 32767 || !path.is_absolute() ||
        !((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) ||
        text[1] != L':' || (text[2] != L'\\' && text[2] != L'/')) return false;
    for (std::size_t i = 2; i < text.size(); ++i)
        if (text[i] < L' ' || text[i] == L'"' || text[i] == L':' ||
            text[i] == L'*' || text[i] == L'?' || text[i] == L'|' ||
            text[i] == L'<' || text[i] == L'>') return false;
    return true;
}
bool SafeSuffix(std::wstring const& suffix) {
    if (suffix.empty() || suffix.size() > 48) return false;
    for (auto c : suffix)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')) return false;
    return true;
}
std::filesystem::path FinalPath(HANDLE handle) {
    std::wstring text(32768, L'\0');
    auto size = GetFinalPathNameByHandleW(handle, text.data(), static_cast<DWORD>(text.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size >= text.size()) throw std::runtime_error("Cannot resolve storage identity");
    text.resize(size);
    if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    std::filesystem::path path(text);
    if (!LocalAbsolutePath(path)) throw std::invalid_argument("Local storage required");
    return path.lexically_normal();
}
std::filesystem::path ImagePath() {
    wchar_t text[32768]{};
    auto size = GetModuleFileNameW(nullptr, text, static_cast<DWORD>(std::size(text)));
    if (!size || size >= std::size(text)) throw std::runtime_error("Cannot locate host image");
    return std::filesystem::weakly_canonical(text);
}
bool IsTestExecutable(std::filesystem::path const& image) {
    return EqualPath(image.filename(), test_executable);
}
bool OwnedBy(HANDLE handle, std::wstring const& sid) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto error = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                 &owner, nullptr, nullptr, nullptr, &descriptor);
    if (error != ERROR_SUCCESS) return false;
    LPWSTR text = nullptr;
    bool same = owner && ConvertSidToStringSidW(owner, &text) &&
                CompareStringOrdinal(text, -1, sid.c_str(), -1, FALSE) == CSTR_EQUAL;
    LocalFree(text);
    LocalFree(descriptor);
    return same;
}
bool OrdinaryFile(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
std::wstring Quote(std::wstring const& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
void DebugFailure(wchar_t const* message) noexcept {
    OutputDebugStringW(message);
}
}

struct ConfigurationHost::Impl {
    explicit Impl(ConfigurationHostOptions value) : options(std::move(value)) {}
    struct Pending {
        std::uint64_t id;
        std::future<configuration::MutationResult> future;
    };
    ConfigurationHostOptions options;
    mutable std::mutex mutex;
    HostStatus status;
    std::filesystem::path directory, settings_image;
    bool test_exe = false;
    // Declaration order also releases the server before either ownership handle.
    Handle directory_handle, writer_lock;
    std::unique_ptr<configuration::ConfigurationServer> server;
    contracts::Configuration stopped_snapshot;
    std::deque<Pending> pending;
    std::uint64_t next_id = 0;
    Handle last_settings_process;

    contracts::Configuration Snapshot() const {
        return server ? server->Snapshot() : stopped_snapshot;
    }
    HostStatus CurrentStatus() const {
        auto result = status;
        result.pending_local = pending.size();
        // A signaled handle distinguishes a real STILL_ACTIVE exit code from a live process.
        if (last_settings_process && WaitForSingleObject(last_settings_process.get(), 0) == WAIT_OBJECT_0) {
            DWORD exit = 0;
            if (GetExitCodeProcess(last_settings_process.get(), &exit)) {
                result.last_launch_exited = true;
                result.last_launch_exit = exit;
            }
        }
        return result;
    }
    HostStatus Failure(HostState state, DWORD error) {
        status.state = state;
        status.error = error;
        DebugFailure(L"WindowSweaters: configuration ownership/start failed.\n");
        return CurrentStatus();
    }
    SettingsLaunchBinding Binding() const {
        SettingsLaunchBinding result;
        if (!server) { result.error = ERROR_INVALID_STATE; return result; }
        result.executable = settings_image;
        result.command_line = Quote(settings_image.native());
        if (options.test_mode) {
            result.command_line += L" --test-mode --storage-dir " + Quote(directory.native()) +
                                   L" --endpoint-suffix " + Quote(options.endpoint_suffix);
        }
        if (result.command_line.size() >= 32767) result.error = ERROR_FILENAME_EXCED_RANGE;
        return result;
    }
};

ConfigurationHost::ConfigurationHost(ConfigurationHostOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
ConfigurationHost::~ConfigurationHost() { Quit(); }
HostStatus ConfigurationHost::Start() noexcept {
    std::lock_guard guard(impl_->mutex);
    if (impl_->server) return impl_->CurrentStatus();
    if (!impl_->pending.empty()) return impl_->Failure(HostState::start_failed, ERROR_BUSY);
    try {
        auto const& options = impl_->options;
        auto image = ImagePath();
        auto test_exe = IsTestExecutable(image);
        if ((!options.test_mode && (!options.storage_directory.empty() ||
             !options.endpoint_suffix.empty() || !options.test_settings_path.empty())) ||
            (options.test_mode && (!LocalAbsolutePath(options.storage_directory) ||
             !SafeSuffix(options.endpoint_suffix))) ||
            (!options.test_settings_path.empty() &&
             (!test_exe || !options.test_mode || !LocalAbsolutePath(options.test_settings_path))))
            return impl_->Failure(HostState::invalid_options, ERROR_INVALID_PARAMETER);

        auto identity = configuration::CurrentIdentity();
        if (identity.integrity < SECURITY_MANDATORY_MEDIUM_RID ||
            identity.integrity >= SECURITY_MANDATORY_HIGH_RID)
            return impl_->Failure(HostState::access_denied, ERROR_ACCESS_DENIED);
        auto production = configuration::DefaultStorageDirectory();
        auto requested = options.test_mode ? options.storage_directory : production;
        requested = requested.lexically_normal().make_preferred();
        auto production_canonical = std::filesystem::weakly_canonical(production);
        auto requested_canonical = std::filesystem::weakly_canonical(requested);
        if (!LocalAbsolutePath(requested) ||
            (options.test_mode && OverlappingPaths(requested_canonical, production_canonical)))
            return impl_->Failure(HostState::invalid_options, ERROR_INVALID_PARAMETER);
        std::filesystem::create_directories(requested);
        Handle directory(CreateFileW(requested.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!directory) return impl_->Failure(HostState::access_denied, GetLastError());
        // Package filesystem redirection can differ from the logical known-folder path.
        auto canonical = FinalPath(directory.get());
        if (!OwnedBy(directory.get(), identity.user_sid))
            return impl_->Failure(HostState::access_denied, ERROR_ACCESS_DENIED);
        if (options.test_mode && OverlappingPaths(canonical, production_canonical))
            return impl_->Failure(HostState::invalid_options, ERROR_INVALID_PARAMETER);

        auto lock_path = canonical / lock_filename;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        auto sddl = L"D:P(A;;GA;;;" + identity.user_sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                  &descriptor, nullptr))
            return impl_->Failure(HostState::access_denied, GetLastError());
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        Handle writer(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
            0, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        auto lock_error = writer ? ERROR_SUCCESS : GetLastError();
        LocalFree(descriptor);
        if (!writer)
            return impl_->Failure(lock_error == ERROR_SHARING_VIOLATION ?
                                  HostState::already_running : HostState::access_denied, lock_error);
        if (!OrdinaryFile(writer.get()) || !OwnedBy(writer.get(), identity.user_sid))
            return impl_->Failure(HostState::access_denied, ERROR_ACCESS_DENIED);

        auto settings = image.parent_path() / L"WindowSweaters.Settings.exe";
        configuration::ServerOptions server_options{
            canonical, options.endpoint_suffix,
            options.test_settings_path.empty() ? settings :
                std::filesystem::weakly_canonical(options.test_settings_path)};
        // Construction loads storage: ownership must already be held here.
        auto server = std::make_unique<configuration::ConfigurationServer>(std::move(server_options));
        try { server->Start(); }
        catch (...) { server->Stop(); throw; }
        impl_->directory = std::move(canonical);
        impl_->settings_image = std::move(settings);
        impl_->test_exe = test_exe;
        impl_->directory_handle = std::move(directory);
        impl_->writer_lock = std::move(writer);
        impl_->server = std::move(server);
        impl_->status.state = HostState::running;
        impl_->status.error = ERROR_SUCCESS;
        impl_->status.last_completion = contracts::Status::disconnected;
        return impl_->CurrentStatus();
    } catch (std::filesystem::filesystem_error const& error) {
        return impl_->Failure(HostState::start_failed, static_cast<DWORD>(error.code().value()));
    } catch (std::invalid_argument const&) {
        return impl_->Failure(HostState::invalid_options, ERROR_INVALID_PARAMETER);
    } catch (...) {
        return impl_->Failure(HostState::start_failed, ERROR_GEN_FAILURE);
    }
}
void ConfigurationHost::Quit() noexcept {
    std::lock_guard guard(impl_->mutex);
    if (impl_->server) {
        impl_->server->Stop();
        impl_->stopped_snapshot = impl_->server->Snapshot();
        impl_->server.reset();
    }
    impl_->writer_lock = Handle{};
    impl_->directory_handle = Handle{};
    impl_->status.state = HostState::stopped;
    impl_->status.error = ERROR_SUCCESS;
}
HostStatus ConfigurationHost::Status() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->CurrentStatus();
}
contracts::Configuration ConfigurationHost::Snapshot() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->Snapshot();
}
std::uint64_t ConfigurationHost::Revision() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->server?impl_->server->Revision():impl_->stopped_snapshot.revision;
}
std::uint64_t ConfigurationHost::EligibilityRevision(applications::AppIdentity const& identity) const {
    std::lock_guard guard(impl_->mutex);
    return impl_->server?impl_->server->EligibilityRevision(identity):impl_->stopped_snapshot.revision;
}
std::optional<contracts::Configuration> ConfigurationHost::PollSnapshot(std::uint64_t observed_revision) const {
    std::lock_guard guard(impl_->mutex);
    if (impl_->server && impl_->server->Revision()==observed_revision) return std::nullopt;
    auto current = impl_->Snapshot();
    return current.revision == observed_revision ? std::nullopt :
                                                  std::optional<contracts::Configuration>(current);
}
void ConfigurationHost::PublishApplications(std::vector<applications::RunningApplicationWindow> windows) {
    std::lock_guard guard(impl_->mutex);
    if (impl_->server) impl_->server->PublishApplications(std::move(windows));
}
std::filesystem::path ConfigurationHost::StorageDirectory() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->directory;
}
std::wstring ConfigurationHost::Endpoint() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->server ? impl_->server->Endpoint() : std::wstring{};
}
LocalSubmission ConfigurationHost::SubmitLocal(bool paused, float width, std::uint64_t expected_revision) {
    std::lock_guard guard(impl_->mutex);
    if (!impl_->server) return {};
    contracts::Configuration proposed;
    proposed.paused = paused;
    proposed.border_width_dip = width;
    proposed.revision = expected_revision;
    if (!contracts::Valid(proposed)) return {LocalSubmissionState::invalid};
    if (impl_->pending.size() >= contracts::maximum_pending_mutations ||
        impl_->next_id == std::numeric_limits<std::uint64_t>::max())
        return {LocalSubmissionState::busy};
    // Allocate the queue entry before submitting so allocation failure cannot orphan an accepted save.
    impl_->pending.push_back({impl_->next_id + 1, {}});
    try {
        impl_->pending.back().future = impl_->server->SubmitLocal(paused, width, expected_revision);
    } catch (...) {
        impl_->pending.pop_back();
        throw;
    }
    return {LocalSubmissionState::queued, ++impl_->next_id};
}
LocalDrain ConfigurationHost::DrainLocalCompletions() {
    std::lock_guard guard(impl_->mutex);
    LocalDrain result;
    result.completions.reserve(impl_->pending.size());
    for (auto it = impl_->pending.begin(); it != impl_->pending.end();) {
        if (it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it; continue;
        }
        configuration::MutationResult completion;
        try { completion = it->future.get(); }
        catch (...) { completion = {contracts::Status::storage_error, impl_->Snapshot()}; }
        result.completions.push_back({it->id, completion});
        impl_->status.last_completion = completion.status;
        if (completion.status == contracts::Status::storage_error)
            DebugFailure(L"WindowSweaters: local configuration save failed.\n");
        it = impl_->pending.erase(it);
    }
    result.current = impl_->Snapshot();
    return result;
}
SettingsLaunchBinding ConfigurationHost::SettingsLaunch() const {
    std::lock_guard guard(impl_->mutex);
    return impl_->Binding();
}
DWORD ConfigurationHost::LaunchSettings() noexcept {
    std::lock_guard guard(impl_->mutex);
    DWORD error = ERROR_SUCCESS;
    try {
        auto binding = impl_->Binding();
        error = binding.error;
        if (!error && impl_->test_exe) error = ERROR_NOT_SUPPORTED;
        Handle image;
        if (!error) {
            image = Handle(CreateFileW(binding.executable.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!image) error = GetLastError();
            else if (!OrdinaryFile(image.get()) ||
                     !EqualPath(FinalPath(image.get()), binding.executable))
                error = ERROR_BAD_EXE_FORMAT;
        }
        if (!error) {
            DWORD binary_type = 0;
            if (!GetBinaryTypeW(binding.executable.c_str(), &binary_type)) error = GetLastError();
            else if (binary_type != SCS_64BIT_BINARY) error = ERROR_BAD_EXE_FORMAT;
        }
        if (!error) {
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(binding.executable.c_str(), binding.command_line.data(), nullptr,
                nullptr, FALSE, 0, nullptr, binding.executable.parent_path().c_str(), &startup, &process))
                error = GetLastError();
            else {
                Handle thread(process.hThread);
                impl_->last_settings_process = Handle(process.hProcess);
                impl_->status.last_launch_pid = process.dwProcessId;
                impl_->status.last_launch_exited = false;
                impl_->status.last_launch_exit = 0;
            }
        }
    } catch (...) { error = ERROR_GEN_FAILURE; }
    impl_->status.last_launch_error = error;
    if (error) DebugFailure(L"WindowSweaters: Settings launch failed.\n");
    return error;
}
}
