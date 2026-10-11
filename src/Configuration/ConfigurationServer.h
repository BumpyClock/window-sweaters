#pragma once
#include "..\Contracts\Protocol.h"
#include "ImportState.h"
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
    contracts::ImportFailure import_failure;
};
struct CommittedState {
    contracts::Configuration configuration;
    CatalogueSnapshot catalogue;
};
// Caller must hold cross-session OS write ownership for the entire lifetime; a session-only singleton is insufficient.
class ConfigurationServer {
public:
    explicit ConfigurationServer(ServerOptions options);
    ~ConfigurationServer();
    ConfigurationServer(ConfigurationServer const&) = delete;
    ConfigurationServer& operator=(ConfigurationServer const&) = delete;
    void Start();
    void Stop() noexcept;
    contracts::Configuration Snapshot() const;
    CommittedState OwnedSnapshot() const;
    std::uint64_t Revision() const;
    std::uint64_t EligibilityRevision(applications::AppIdentity const& identity) const;
    void PublishApplications(std::vector<applications::RunningApplicationWindow> windows);
    std::uint64_t ApplicationBuilds() const;
    std::future<MutationResult> SubmitLocal(bool paused, float width, std::uint64_t expected_revision);
    std::future<MutationResult> SubmitLocal(contracts::Configuration const& expected);
    std::future<MutationResult> SubmitEligibility(applications::EligibilityMutation mutation,std::uint64_t expected_revision);
    std::future<MutationResult> SubmitImport(contracts::ImportMutation mutation,std::uint64_t expected_revision);
    std::wstring Endpoint() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
