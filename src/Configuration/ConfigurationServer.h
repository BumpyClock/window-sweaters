#pragma once
#include "..\Contracts\Protocol.h"
#include <filesystem>
#include <future>
#include <memory>

namespace window_sweaters::configuration {
struct ServerOptions {
    std::filesystem::path storage_directory;
    std::wstring endpoint_suffix;
    std::filesystem::path expected_settings_path;
};
struct MutationResult {
    contracts::Status status = contracts::Status::disconnected;
    contracts::Configuration configuration{};
    DWORD error=ERROR_SUCCESS;
};
// Caller must hold cross-session OS write ownership for the canonical storage directory
// and user for the entire lifetime. A session-only singleton is not sufficient.
class ConfigurationServer {
public:
    explicit ConfigurationServer(ServerOptions options);
    ~ConfigurationServer();
    ConfigurationServer(ConfigurationServer const&) = delete;
    ConfigurationServer& operator=(ConfigurationServer const&) = delete;
    void Start();
    void Stop() noexcept;
    contracts::Configuration Snapshot() const;
    std::uint64_t Revision() const;
    std::uint64_t EligibilityRevision(applications::AppIdentity const& identity) const;
    void PublishApplications(std::vector<applications::RunningApplicationWindow> windows);
    // Diagnostic: application snapshot rebuilds (publication or configuration revision change).
    std::uint64_t ApplicationBuilds() const;
    std::future<MutationResult> SubmitLocal(bool paused, float width, std::uint64_t expected_revision);
    std::future<MutationResult> SubmitLocal(contracts::Configuration const& expected);
    std::future<MutationResult> SubmitEligibility(applications::EligibilityMutation mutation,std::uint64_t expected_revision);
    std::wstring Endpoint() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
