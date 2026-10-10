#pragma once
#include "..\Configuration\ConfigurationServer.h"
#include <optional>
#include <vector>

namespace window_sweaters::integration {
struct ConfigurationHostOptions {
    bool test_mode = false;
    std::filesystem::path storage_directory;
    std::wstring endpoint_suffix;
    // Only the native integration-test executable can override peer verification.
    std::filesystem::path test_settings_path;
};
enum class HostState { stopped, running, already_running, invalid_options, access_denied, start_failed };
struct HostStatus {
    HostState state = HostState::stopped;
    DWORD error = ERROR_SUCCESS;
    std::size_t pending_local = 0;
    contracts::Status last_completion = contracts::Status::disconnected;
    DWORD last_launch_error = ERROR_SUCCESS;
    // Last successfully created process, not necessarily the activated Settings
    // singleton. Failed attempts and Quit preserve it; PID 0 means no launch.
    DWORD last_launch_pid = 0;
    bool last_launch_exited = false;
    DWORD last_launch_exit = 0; // Meaningful only when last_launch_exited is true.
};
enum class LocalSubmissionState { queued, busy, invalid, not_owner };
struct LocalSubmission {
    LocalSubmissionState state = LocalSubmissionState::not_owner;
    std::uint64_t id = 0;
};
struct LocalCompletion {
    std::uint64_t id = 0;
    configuration::MutationResult result;
};
struct LocalDrain {
    std::vector<LocalCompletion> completions;
    contracts::Configuration current;
};
struct SettingsLaunchBinding {
    std::filesystem::path executable;
    std::wstring command_line;
    DWORD error = ERROR_SUCCESS;
};

// Lifecycle, launch and local-command calls belong on the owner thread. Snapshot
// copies are safe on a consumer thread; poll them outside WinEvent callbacks.
class ConfigurationHost {
public:
    explicit ConfigurationHost(ConfigurationHostOptions options = {});
    ~ConfigurationHost();
    ConfigurationHost(ConfigurationHost const&) = delete;
    ConfigurationHost& operator=(ConfigurationHost const&) = delete;

    // After Quit, drain outstanding completions before starting again.
    HostStatus Start() noexcept;
    // Quit may wait for an in-flight save. It stops IPC before releasing ownership,
    // never kills Settings, and never launches/restarts either application.
    void Quit() noexcept;
    // Process status is sampled without waiting; no window/title/path diagnostics.
    HostStatus Status() const;
    contracts::Configuration Snapshot() const;
    std::optional<contracts::Configuration> PollSnapshot(std::uint64_t observed_revision) const;
    std::filesystem::path StorageDirectory() const;
    std::wstring Endpoint() const;

    // Queued is not saved. At most 32 undrained futures are retained. Submission
    // and draining never wait for a save; drain.current coalesces to the newest
    // committed snapshot, not an older completion's snapshot.
    LocalSubmission SubmitLocal(bool paused, float width, std::uint64_t expected_revision);
    LocalDrain DrainLocalCompletions();

    // Inspect the binding without launching UI. LaunchSettings uses this exact
    // sibling executable with CreateProcessW, not PATH or a shell.
    SettingsLaunchBinding SettingsLaunch() const;
    DWORD LaunchSettings() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
