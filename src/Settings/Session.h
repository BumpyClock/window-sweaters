#pragma once
#include "..\Configuration\ConfigurationServer.h"
#include <functional>
#include <memory>
#include <optional>

namespace window_sweaters::settings {
struct Options {
    bool test_mode=false;
    std::filesystem::path storage_directory;
    std::wstring endpoint_suffix;
    bool startup_probe=false;
};
struct ConnectionState {
    bool connected=false;
    bool pending=false;
    contracts::Status status=contracts::Status::disconnected;
    contracts::Configuration configuration{};
    contracts::Identity server_instance{};
    std::uint64_t client_lease=0;
    std::uint32_t error=0;
    std::uint64_t publication_sequence=0;
    std::uint64_t settled_edit_generation=0;
    bool configuration_authoritative=false;
    contracts::ImportFailure import_failure;
    std::wstring detail=L"Connecting to the host…";
};
struct SessionTransport {
    // cancel: manual-reset event signalled by Stop; implementations must abandon blocking waits promptly.
    std::function<contracts::Message(contracts::Message const&,HANDLE cancel)> exchange;
    std::function<bool()> start_host;
};
class DraftState {
public:
    struct Update { bool accepted=false; bool controls_changed=false; };
    Update Apply(ConnectionState const& state);
    void Begin(std::uint64_t edit_generation,contracts::Configuration const& appearance);
    bool Pending() const noexcept { return edit_generation_>current_.settled_edit_generation; }
    ConnectionState const& Current() const noexcept { return current_; }
    ConnectionState const& Displayed() const noexcept { return displayed_; }
    contracts::Configuration const& Controls() const noexcept { return controls_; }
    std::optional<contracts::Configuration> const& LastConfirmed() const noexcept { return last_confirmed_; }
private:
    ConnectionState current_,displayed_;
    contracts::Configuration controls_;
    std::optional<contracts::Configuration> last_confirmed_;
    std::uint64_t edit_generation_=0;
};
class Session {
public:
    Session(Options options,std::function<void(ConnectionState)> publish,SessionTransport transport={});
    ~Session();
    Session(Session const&)=delete;
    Session& operator=(Session const&)=delete;
    std::uint64_t Change(ConnectionState const& displayed,bool paused,float width);
    std::uint64_t Change(ConnectionState const& displayed,contracts::Configuration const& appearance);
    std::uint64_t Reset(ConnectionState const& displayed);
    std::uint64_t Import(ConnectionState const& displayed,contracts::ImportMutation mutation);
    std::uint64_t ChangeStartup(ConnectionState const& displayed,startup::DesiredPreference desired);
    void RequestStop() noexcept;
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class StartupPreferences final {
public:
    struct Update {
        startup::Completion completion;
        startup::DesiredPreference desired;
        std::uint64_t save_generation{};
    };
    explicit StartupPreferences(Session& session,std::shared_ptr<startup::Backend> backend={});
    startup::Submission Read();
    startup::Submission Request(ConnectionState const& displayed,startup::DesiredPreference desired,
        startup::UserIntent intent);
    std::optional<Update> Poll();
    void Observe(ConnectionState const& state) noexcept;
    startup::Snapshot Snapshot() const noexcept { return controller_->GetSnapshot(); }
    void Stop() noexcept { controller_->Shutdown(); pending_.reset(); }
private:
    Session& session_;
    std::unique_ptr<startup::Controller> controller_;
    std::optional<ConnectionState> pending_;
    startup::DesiredPreference desired_{startup::DesiredPreference::Off};
};
struct AppsConnectionState {
    ConnectionState connection;
    contracts::ApplicationsSnapshot snapshot;
};
class AppsDraftState {
public:
    bool Apply(AppsConnectionState const& state);
    void Begin(std::uint64_t generation,applications::EligibilityMutation const& mutation);
    bool Pending() const noexcept { return generation_>current_.connection.settled_edit_generation; }
    bool CanEdit() const noexcept {
        auto const& connection=current_.connection;
        return connection.connected && connection.configuration_authoritative &&
            connection.status!=contracts::Status::recovery_required && !connection.pending && !Pending();
    }
    bool CanInteract() const noexcept {
        auto const& connection=current_.connection;
        return connection.connected && connection.configuration_authoritative &&
            connection.status!=contracts::Status::recovery_required;
    }
    AppsConnectionState const& Current() const noexcept { return current_; }
    AppsConnectionState const& Displayed() const noexcept { return displayed_; }
    contracts::ApplicationsSnapshot const& Controls() const noexcept { return controls_; }
private:
    AppsConnectionState current_,displayed_;
    contracts::ApplicationsSnapshot controls_;
    std::uint64_t generation_{};
};
class AppsSession {
public:
    AppsSession(Options options,std::function<void(AppsConnectionState)> publish,SessionTransport transport={});
    ~AppsSession();
    std::uint64_t Change(AppsConnectionState const& displayed,applications::EligibilityMutation mutation);
    void RequestStop() noexcept;
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Options ParseOptions();
bool StartHost(Options const& options);
}
