#pragma once
#include "..\Configuration\ConfigurationServer.h"
#include <optional>
#include <vector>

namespace window_sweaters::integration {
struct ConfigurationHostOptions {
    bool test_mode = false;
    std::filesystem::path storage_directory;
    std::wstring endpoint_suffix;
    std::filesystem::path test_settings_path;
};
enum class HostState { stopped, running, already_running, invalid_options, access_denied, start_failed };
struct HostStatus {
    HostState state = HostState::stopped;
    DWORD error = ERROR_SUCCESS;
    std::size_t pending_local = 0;
    contracts::Status last_completion = contracts::Status::disconnected;
    DWORD last_launch_error = ERROR_SUCCESS;
    DWORD last_launch_pid = 0;
    bool last_launch_exited = false;
    DWORD last_launch_exit = 0;
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

class ConfigurationHost {
public:
    explicit ConfigurationHost(ConfigurationHostOptions options = {});
    ~ConfigurationHost();
    ConfigurationHost(ConfigurationHost const&) = delete;
    ConfigurationHost& operator=(ConfigurationHost const&) = delete;

    HostStatus Start() noexcept;
    void Quit() noexcept;
    HostStatus Status() const;
    contracts::Configuration Snapshot() const;
    std::uint64_t Revision() const;
    std::uint64_t EligibilityRevision(applications::AppIdentity const& identity) const;
    std::optional<contracts::Configuration> PollSnapshot(std::uint64_t observed_revision) const;
    void PublishApplications(std::vector<applications::RunningApplicationWindow> windows);
    std::filesystem::path StorageDirectory() const;
    std::wstring Endpoint() const;

    LocalSubmission SubmitLocal(bool paused, float width, std::uint64_t expected_revision);
    LocalDrain DrainLocalCompletions();

    SettingsLaunchBinding SettingsLaunch() const;
    DWORD LaunchSettings() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
