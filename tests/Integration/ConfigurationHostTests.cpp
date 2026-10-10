#include "..\..\src\Integration\ConfigurationHost.h"
#include "..\..\src\Configuration\Pipe.h"
#include "..\..\src\Configuration\Storage.h"
#include <winioctl.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using namespace window_sweaters;
using namespace window_sweaters::integration;
using configuration::Handle;
namespace {
int checks = 0;
void Require(bool value, char const* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
std::filesystem::path Self() {
    return configuration::SiblingExecutable(L"WindowSweaters.ConfigurationHost.Tests.exe");
}
std::filesystem::path ScratchRoot() {
    return Self().parent_path().parent_path().parent_path() / L"test";
}
ConfigurationHostOptions Options(std::filesystem::path const& directory, std::wstring suffix = L"native") {
    return {true, directory, L"test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + suffix, Self()};
}
std::vector<char> Bytes(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void WaitRevision(ConfigurationHost const& host, std::uint64_t revision) {
    auto deadline = GetTickCount64() + 5000;
    while (host.Snapshot().revision != revision && GetTickCount64() < deadline) Sleep(5);
    Require(host.Snapshot().revision == revision, "Committed revision reaches the consumer without draining completions");
}
std::vector<LocalCompletion> DrainAll(ConfigurationHost& host) {
    std::vector<LocalCompletion> all;
    auto deadline = GetTickCount64() + 5000;
    do {
        auto drain = host.DrainLocalCompletions();
        all.insert(all.end(), drain.completions.begin(), drain.completions.end());
        if (!host.Status().pending_local) break;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    Require(host.Status().pending_local == 0, "All asynchronous local completions drain within a deadline");
    return all;
}
contracts::Message RemoteSnapshot(ConfigurationHost const& host) {
    contracts::Message request;
    request.client = contracts::NewIdentity();
    request.request_id = 1;
    return configuration::Exchange(host.Endpoint(), Self(), request);
}
contracts::Message RemoteSave(ConfigurationHost const& host, float width) {
    auto request = RemoteSnapshot(host);
    Require(request.status == contracts::Status::saved, "Native test peer obtains a real pipe snapshot");
    request.operation = contracts::Operation::mutate;
    request.request_id = 2;
    request.configuration.border_width_dip = width;
    return configuration::Exchange(host.Endpoint(), Self(), request);
}
void RunChild(wchar_t const* mode, std::filesystem::path const& directory, DWORD expected,
              std::filesystem::path image = Self()) {
    std::wstring command = L"\"" + image.native() + L"\" " + mode + L" \"" + directory.native() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, image.parent_path().c_str(), &startup, &process) != FALSE,
        "Launch only a native test helper, never Settings UI");
    Handle thread(process.hThread), child(process.hProcess);
    auto wait = WaitForSingleObject(child.get(), 15000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.get(), 99);
        WaitForSingleObject(child.get(), 5000);
    }
    Require(wait == WAIT_OBJECT_0, "Native helper terminates within a deadline");
    DWORD result = 99;
    Require(GetExitCodeProcess(child.get(), &result) && result == expected, "Native helper reports the expected ownership outcome");
}
void Junction(std::filesystem::path const& alias, std::filesystem::path const& target) {
    std::filesystem::create_directory(alias);
    Handle directory(CreateFileW(alias.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Require(bool(directory), "Open an isolated directory for a canonical alias");
    struct Header {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    };
    static_assert(sizeof(Header) == 16);
    auto substitute = L"\\??\\" + target.native();
    auto print = target.native();
    auto substitute_bytes = (substitute.size() + 1) * sizeof(wchar_t);
    auto print_bytes = (print.size() + 1) * sizeof(wchar_t);
    std::vector<std::byte> bytes(sizeof(Header) + substitute_bytes + print_bytes);
    Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(bytes.size() - 8), 0, 0,
        static_cast<WORD>(substitute.size() * sizeof(wchar_t)), static_cast<WORD>(substitute_bytes),
        static_cast<WORD>(print.size() * sizeof(wchar_t))};
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), substitute.c_str(), substitute_bytes);
    std::memcpy(bytes.data() + sizeof(header) + substitute_bytes, print.c_str(), print_bytes);
    DWORD returned = 0;
    Require(DeviceIoControl(directory.get(), FSCTL_SET_REPARSE_POINT, bytes.data(),
        static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr) != FALSE,
        "Create a real junction alias without registry or elevation");
}
void ValidationCases(std::filesystem::path const& root) {
    auto options = Options(root / L"validation");
    options.test_mode = false;
    ConfigurationHost production_override(options);
    Require(production_override.Start().state == HostState::invalid_options,
        "Production cannot accept storage, endpoint or peer overrides");
    Require(!std::filesystem::exists(options.storage_directory), "Invalid options never create storage");
    options = Options(root / L"validation");
    options.storage_directory = L"relative";
    ConfigurationHost relative(options);
    Require(relative.Start().state == HostState::invalid_options, "Test storage must be absolute");
    options = Options(root / L"validation");
    options.endpoint_suffix = L"unsafe\" --other-option";
    ConfigurationHost injected(options);
    Require(injected.Start().state == HostState::invalid_options, "Endpoint suffix cannot inject launch arguments");
    options.endpoint_suffix.clear();
    ConfigurationHost no_suffix(options);
    Require(no_suffix.Start().state == HostState::invalid_options, "Isolated storage requires an explicit safe endpoint suffix");
    options = Options(root / L"validation");
    options.test_settings_path = L"relative.exe";
    ConfigurationHost relative_peer(options);
    Require(relative_peer.Start().state == HostState::invalid_options, "Test peer override must be absolute");
    auto restricted_image = root / L"OtherNativeCaller.exe";
    Require(CopyFileW(Self().c_str(), restricted_image.c_str(), TRUE) != FALSE,
        "Create only an isolated native test caller with a non-test image name");
    RunChild(L"--restricted-peer", root / L"restricted-caller", 0, restricted_image);
    options = Options(root / L"validation");
    options.storage_directory = configuration::DefaultStorageDirectory();
    ConfigurationHost user_data(options);
    Require(user_data.Start().state == HostState::invalid_options, "Test binding cannot claim canonical production storage");
    options.storage_directory /= L"test-child";
    ConfigurationHost user_data_child(options);
    Require(user_data_child.Start().state == HostState::invalid_options, "Test storage cannot overlap a production subtree");
    options.storage_directory = configuration::DefaultStorageDirectory().parent_path();
    ConfigurationHost user_data_parent(options);
    Require(user_data_parent.Start().state == HostState::invalid_options, "A shared production ancestor is not isolated test storage");
    options = Options(root / L"validation");
    options.storage_directory = L"\\\\server\\share\\storage";
    ConfigurationHost network(options);
    Require(network.Start().state == HostState::invalid_options, "User storage is local, never a network/device path");
    ConfigurationHost stopped(Options(root / L"unstarted"));
    Require(stopped.SubmitLocal(true, 20, 0).state == LocalSubmissionState::not_owner,
        "A nonowner never submits writes");
    Require(stopped.SettingsLaunch().error == ERROR_INVALID_STATE, "A stopped host cannot build a launch binding");
}
void OwnershipCases(std::filesystem::path const& root) {
    auto directory = root / L"writer";
    ConfigurationHost first(Options(directory, L"owner"));
    Require(first.Start().state == HostState::running, "First host owns isolated user storage");
    Require(first.Start().state == HostState::running, "Start is idempotent for the owner");
    Require(first.Snapshot().revision == 0 && first.Snapshot().border_width_dip == 16 &&
            !first.Snapshot().paused, "Consumer sees shared committed defaults");
    ConfigurationHost second(Options(directory, L"other-logon-endpoint"));
    auto conflict = second.Start();
    Require(conflict.state == HostState::already_running && conflict.error == ERROR_SHARING_VIOLATION,
        "Storage conflicts remain distinct even when endpoint/logon suffix differs");
    Require(second.SubmitLocal(true, 24, 0).state == LocalSubmissionState::not_owner,
        "Conflicting host cannot start a second writer");
    RunChild(L"--contend", directory, 0);
    auto casing = directory.native();
    std::transform(casing.begin(), casing.end(), casing.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    ConfigurationHost case_alias(Options(casing, L"case-alias"));
    Require(case_alias.Start().state == HostState::already_running, "Case aliases contend for the same physical lock");
    ConfigurationHost lexical_alias(Options(directory / L"unused" / L".." / L".", L"lexical-alias"));
    Require(lexical_alias.Start().state == HostState::already_running, "Dot aliases contend for the same canonical storage");
    auto alias = root / L"junction";
    Junction(alias, directory);
    ConfigurationHost junction_alias(Options(alias, L"junction-alias"));
    Require(junction_alias.Start().state == HostState::already_running, "Real junction aliases cannot introduce another writer");
    auto ticket = first.SubmitLocal(true, 24.5f, 0);
    Require(ticket.state == LocalSubmissionState::queued && ticket.id != 0, "Local intent receives a queued, not saved, ticket");
    auto completions = DrainAll(first);
    Require(completions.size() == 1 && completions[0].id == ticket.id &&
            completions[0].result.status == contracts::Status::saved, "Local completion identifies a real persisted save");
    Require(configuration::Storage(directory).Load().configuration == first.Snapshot() &&
            first.Snapshot().revision == 1, "Owned snapshot is the committed disk value");
    auto endpoint = first.Endpoint();
    first.Quit();
    first.Quit();
    Require(first.Status().state == HostState::stopped && first.Endpoint().empty() &&
            !WaitNamedPipeW(endpoint.c_str(), 0), "Explicit Quit stops IPC and never restarts the host");
    Require(std::filesystem::is_regular_file(directory / L"configuration.writer.lock"),
        "Ordinary persistent lock file is not a permanent ownership marker");
    Require(second.Start().state == HostState::running && second.Snapshot().revision == 1 &&
            second.Snapshot().paused && second.Snapshot().border_width_dip == 24.5f,
        "A nonowner can reacquire after stop and load committed user values");
    second.Quit();
    Require(junction_alias.Start().state == HostState::running &&
            junction_alias.StorageDirectory() == directory,
        "An alias owner binds both server storage and Settings launch to the resolved canonical directory");
    junction_alias.Quit();
    Require(RemoveDirectoryW(alias.c_str()) != FALSE, "Remove only the isolated junction, not its target");

    auto crash_directory = root / L"crashed-writer";
    RunChild(L"--crash-owner", crash_directory, 23);
    ConfigurationHost after_crash(Options(crash_directory, L"after-crash"));
    Require(after_crash.Start().state == HostState::running,
        "OS handles release on abrupt process exit; stale lock file does not deny startup");
}
void FailureCases(std::filesystem::path const& root) {
    auto directory = root / L"bad-lock";
    auto lock_path = directory / L"configuration.writer.lock";
    std::filesystem::create_directories(lock_path);
    ConfigurationHost bad_lock(Options(directory, L"bad-lock"));
    Require(bad_lock.Start().state == HostState::access_denied, "A directory/reparse lock is not treated as ownership");
    Require(!std::filesystem::exists(directory / L"configuration.bin"), "Lock failure never starts the configuration writer");
    std::filesystem::remove(lock_path);
    Require(bad_lock.Start().state == HostState::running, "Failed lock acquisition releases all resources for retry");
    bad_lock.Quit();

    auto options = Options(root / L"pipe-failure", L"pipe-failure");
    auto endpoint = configuration::EndpointName(options.endpoint_suffix);
    Handle squatter(CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 1024, 1024, 0, nullptr));
    Require(bool(squatter), "Inject a real endpoint startup failure");
    ConfigurationHost failed(options);
    Require(failed.Start().state == HostState::start_failed, "Server startup failure is observable");
    {
        Handle reacquired(CreateFileW((options.storage_directory / L"configuration.writer.lock").c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(bool(reacquired), "Server startup failure releases the exclusive writer handle");
    }
    squatter = Handle{};
    Require(failed.Start().state == HostState::running, "Server startup can retry after the endpoint is repaired");
    failed.Quit();

    options = Options(root / L"peer-restriction", L"peer-restriction");
    options.test_settings_path = root / L"not-the-test-peer.exe";
    ConfigurationHost restricted(options);
    Require(restricted.Start().state == HostState::running, "Native tests can bind a fake expected peer");
    bool rejected = false;
    try { RemoteSnapshot(restricted); } catch (std::runtime_error const&) { rejected = true; }
    Require(rejected && restricted.Snapshot().revision == 0, "Real server rejects a process that is not the expected Settings image");
}
void LocalCases(std::filesystem::path const& root) {
    auto directory = root / L"local";
    ConfigurationHost host(Options(directory, L"local"));
    Require(host.Start().state == HostState::running, "Local command host starts");
    Require(!host.PollSnapshot(0), "An unchanged committed revision produces no consumer update");
    auto initial = host.SubmitLocal(false, 20, 0);
    auto completed = DrainAll(host);
    Require(completed.size() == 1 && completed[0].id == initial.id &&
            completed[0].result.status == contracts::Status::saved, "Save completion is delivered through the host API");
    Require(host.PollSnapshot(0)->revision == 1 && !host.PollSnapshot(1), "Consumer polling copies only changed committed revisions");
    auto before = host.Snapshot();
    auto file = configuration::Storage(directory).File();
    auto bytes = Bytes(file);
    {
        Handle blocker(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(bool(blocker), "Hold a real replacement-denying lock, without changing existing bytes");
        auto began = std::chrono::steady_clock::now();
        auto ticket = host.SubmitLocal(true, 40, before.revision);
        auto elapsed = std::chrono::steady_clock::now() - began;
        Require(ticket.state == LocalSubmissionState::queued && elapsed < std::chrono::milliseconds(75),
            "Submission returns without waiting for disk replacement retries");
        Require(host.Snapshot() == before, "Pending intent never publishes optimistic success");
        auto failure = DrainAll(host);
        Require(failure.size() == 1 && failure[0].id == ticket.id &&
                failure[0].result.status == contracts::Status::storage_error &&
                failure[0].result.configuration == before && host.Snapshot() == before &&
                Bytes(file) == bytes, "Failed save leaves both consumer and durable revision unchanged");
        Require(host.Status().last_completion == contracts::Status::storage_error,
            "Sparse error status reports failure only after actual save completion");
    }
    auto retry = host.SubmitLocal(true, 40, before.revision);
    auto success = DrainAll(host);
    Require(success.size() == 1 && success[0].id == retry.id &&
            success[0].result.status == contracts::Status::saved && host.Snapshot().revision == 2,
        "A repaired disk allows an explicit local retry to commit");
    auto stale = host.SubmitLocal(false, 30, 0);
    auto stale_results = DrainAll(host);
    Require(stale.state == LocalSubmissionState::queued && stale_results.size() == 1 &&
            stale_results[0].result.status == contracts::Status::conflict && host.Snapshot().revision == 2,
        "Local writes retain expected-revision conflict semantics");
    Require(host.SubmitLocal(false, std::numeric_limits<float>::quiet_NaN(), 2).state == LocalSubmissionState::invalid &&
            host.Status().pending_local == 0, "Invalid local values never allocate pending work");

    auto a = host.SubmitLocal(false, 21, 2);
    WaitRevision(host, 3);
    auto b = host.SubmitLocal(true, 22, 3);
    WaitRevision(host, 4);
    auto remote = RemoteSave(host, 23);
    Require(remote.status == contracts::Status::saved && remote.configuration.revision == 5,
        "Actual Settings protocol mutations share the committed host snapshot");
    auto coalesced = host.DrainLocalCompletions();
    Require(coalesced.completions.size() == 2 && coalesced.completions[0].id == a.id &&
            coalesced.completions[0].result.configuration.revision == 3 &&
            coalesced.completions[1].id == b.id &&
            coalesced.completions[1].result.configuration.revision == 4 &&
            coalesced.current.revision == 5 && coalesced.current.border_width_dip == 23,
        "Draining older local completions coalesces to current state, never regresses a newer remote commit");
    Require(configuration::Storage(directory).Load().configuration == coalesced.current,
        "Coalesced consumer snapshot is a persisted value");

    for (unsigned i = 0; i < 32; ++i)
        Require(host.SubmitLocal(false, 25, 5).state == LocalSubmissionState::queued,
            "Host admits local commands up to the bounded undrained capacity");
    WaitRevision(host, 6);
    Require(host.Status().pending_local == 32 &&
            host.SubmitLocal(true, 26, 6).state == LocalSubmissionState::busy,
        "Even completed futures count toward the 32-entry bound until drained");
    auto bounded = DrainAll(host);
    Require(bounded.size() == 32 &&
            std::count_if(bounded.begin(), bounded.end(), [](auto const& item) {
                return item.result.status == contracts::Status::saved;
            }) == 1 && host.Snapshot().revision == 6, "Bounded queue preserves all completions and serial revision conflicts");
    auto accepted = host.SubmitLocal(true, 27, 6);
    Require(accepted.state == LocalSubmissionState::queued, "Draining releases local admission capacity");
    host.Quit();
    auto restart = host.Start();
    Require(restart.state == HostState::start_failed && restart.error == ERROR_BUSY && restart.pending_local == 1,
        "Explicit restart cannot silently discard undrained completion tickets");
    auto quitting = DrainAll(host);
    Require(quitting.size() == 1 && quitting[0].id == accepted.id &&
            (quitting[0].result.status == contracts::Status::saved ||
             quitting[0].result.status == contracts::Status::shutting_down),
        "Quit settles accepted futures without claiming unsaved success");
    Require(host.SubmitLocal(false, 28, host.Snapshot().revision).state == LocalSubmissionState::not_owner,
        "Explicit Quit does not automatically restart a writer");
}
void BindingCases(std::filesystem::path const& root) {
    auto options = Options(root / L"binding with spaces", L"safe_ABC-123");
    ConfigurationHost host(options);
    Require(host.Start().state == HostState::running, "Isolated launch binding host starts");
    auto binding = host.SettingsLaunch();
    int argc = 0;
    auto argv = CommandLineToArgvW(binding.command_line.c_str(), &argc);
    Require(binding.error == ERROR_SUCCESS && argv && argc == 6, "Known Settings options form exactly six arguments");
    Require(binding.executable == Self().parent_path() / L"WindowSweaters.Settings.exe" &&
            std::filesystem::path(argv[0]) == binding.executable,
        "Launch target is the exact absolute sibling, never PATH or the test peer override");
    Require(std::wstring(argv[1]) == L"--test-mode" && std::wstring(argv[2]) == L"--storage-dir" &&
            std::filesystem::path(argv[3]) == host.StorageDirectory() &&
            std::wstring(argv[4]) == L"--endpoint-suffix" && std::wstring(argv[5]) == options.endpoint_suffix,
        "Test storage and safe endpoint are passed as verified, correctly quoted options");
    LocalFree(argv);
    // Deliberately do not call LaunchSettings: native tests never open UI.
    host.Quit();
    Require(host.SettingsLaunch().error == ERROR_INVALID_STATE, "A quit host cannot relaunch Settings");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3) {
            auto directory = std::filesystem::weakly_canonical(argv[2]);
            auto scratch = std::filesystem::weakly_canonical(ScratchRoot()).native() + L"\\";
            Require(directory.native().starts_with(scratch), "Native helpers are confined to repository out\\test storage");
            ConfigurationHost host(Options(directory, L"child-different-endpoint"));
            if (std::wstring(argv[1]) == L"--restricted-peer")
                return host.Start().state == HostState::invalid_options ? 0 : 1;
            if (std::wstring(argv[1]) == L"--contend")
                return host.Start().state == HostState::already_running ? 0 : 1;
            if (std::wstring(argv[1]) == L"--crash-owner") {
                Require(host.Start().state == HostState::running, "Crash helper acquires an actual writer handle");
                ExitProcess(23);
            }
            return 2;
        }
        auto root = ScratchRoot() / (L"configuration-host-" + std::to_wstring(GetCurrentProcessId()) +
                                    L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        try {
            ValidationCases(root);
            OwnershipCases(root);
            FailureCases(root);
            LocalCases(root);
            BindingCases(root);
        } catch (...) {
            std::filesystem::remove_all(root);
            throw;
        }
        std::filesystem::remove_all(root);
        std::cout << "ConfigurationHost: " << checks << " checks passed\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "ConfigurationHost failed: " << error.what() << '\n';
        return 1;
    }
}
