#include "Session.h"
#include "..\Configuration\Pipe.h"
#include <shellapi.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace window_sweaters::settings {
using namespace contracts;
using namespace configuration;
Options ParseOptions() {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if (!args) throw std::runtime_error("Cannot parse launch options");
    Options options;
    try {
        for (int i=1;i<count;++i) {
            std::wstring argument(args[i]);
            if (argument==L"--test-mode") options.test_mode=true;
            else if (argument==L"--startup-probe") options.startup_probe=true;
            else if (argument==L"--storage-dir" && i+1<count) options.storage_directory=args[++i];
            else if (argument==L"--endpoint-suffix" && i+1<count) options.endpoint_suffix=args[++i];
            else throw std::invalid_argument("Unsupported settings launch option");
        }
        if (!options.test_mode && (!options.storage_directory.empty() || !options.endpoint_suffix.empty()))
            throw std::invalid_argument("Storage/endpoint overrides require --test-mode");
        if (options.test_mode && options.storage_directory.empty())
            throw std::invalid_argument("--test-mode requires --storage-dir");
        if (options.test_mode) {
            options.storage_directory=std::filesystem::weakly_canonical(std::filesystem::absolute(options.storage_directory));
            if (options.endpoint_suffix.empty()) options.endpoint_suffix=TestEndpointSuffix(options.storage_directory);
        }
        EndpointName(options.endpoint_suffix);
    } catch (...) { LocalFree(args); throw; }
    LocalFree(args); return options;
}
bool StartHost(Options const& options) {
    auto host=SiblingExecutable(L"WindowSweaters.Host.exe");
    if (!std::filesystem::is_regular_file(host)) return false;
    std::wstring command=L"\""+host.native()+L"\"";
    if (options.test_mode)
        command+=L" --test-mode --storage-dir \""+options.storage_directory.native()+
            L"\" --endpoint-suffix \""+options.endpoint_suffix+L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(host.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,
                        host.parent_path().c_str(),&startup,&process)) return false;
    Handle thread(process.hThread),handle(process.hProcess); return true;
}
struct Session::Impl {
    struct Desired { Message request; Configuration displayed; std::uint64_t edit_generation; };
    struct OwnCommit {
        Identity server;
        std::uint64_t lease;
        Configuration before,after;
        std::uint64_t last_used;
    };
    Impl(Options value,std::function<void(ConnectionState)> callback,SessionTransport io)
        : options(std::move(value)),publish(std::move(callback)),endpoint(EndpointName(options.endpoint_suffix)),
          host(SiblingExecutable(L"WindowSweaters.Host.exe")),client(NewIdentity()),transport(std::move(io)) {
        if (!transport.exchange) transport.exchange=[name=endpoint,image=host](Message const& request) {
            return Exchange(name,image,request);
        };
        if (!transport.start_host) transport.start_host=[launch_options=options] {return StartHost(launch_options);};
        own_commits.reserve(32);
    }
    Options options;
    std::function<void(ConnectionState)> publish;
    std::wstring endpoint;
    std::filesystem::path host;
    Identity client;
    SessionTransport transport;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Desired> desired;
    std::chrono::steady_clock::time_point due{};
    std::uint64_t request_id=0;
    std::uint64_t last_edit_generation=0;
    bool stop=false,ever_connected=false,launch_attempted=false;
    std::thread worker;
    ConnectionState view;
    Message last_snapshot;
    std::optional<Desired> in_flight;
    std::vector<OwnCommit> own_commits;
    std::uint64_t own_clock=0;
    void Publish() noexcept {
        ++view.publication_sequence;
        try { publish(view); } catch (...) { OutputDebugStringW(L"WindowSweaters: settings status publication failed.\n"); }
    }
    void DiscardRequests() {
        in_flight.reset();
        own_commits.clear();
        view.pending=false;
        std::lock_guard lock(mutex);
        desired.reset(); view.settled_edit_generation=last_edit_generation;
    }
    void Run() noexcept {
        for (;;) {
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(150),[&]{return stop;});
                if (stop) return;
            }
            try {
                bool refresh=!view.connected || !in_flight;
                if (refresh) {
                    Message request; request.client=client; request.request_id=++request_id;
                    request.server=last_snapshot.server; request.client_lease=last_snapshot.client_lease;
                    auto snapshot=transport.exchange(request);
                    if (snapshot.status==Status::client_expired || snapshot.status==Status::wrong_instance) {
                        DiscardRequests(); last_snapshot={};
                        view.connected=false; view.status=snapshot.status;
                        view.configuration_authoritative=false;
                        view.configuration=snapshot.configuration; view.server_instance=snapshot.server; view.client_lease=0;
                        view.detail=L"Connection expired. Pending changes were discarded.";
                        Publish(); continue;
                    }
                    if (!EmptyIdentity(last_snapshot.server) && snapshot.server!=last_snapshot.server) DiscardRequests();
                    last_snapshot=snapshot; view.connected=true; ever_connected=true;
                    view.configuration=snapshot.configuration;
                    view.configuration_authoritative=snapshot.status==Status::saved;
                    view.server_instance=snapshot.server; view.client_lease=snapshot.client_lease;
                    if (view.status==Status::disconnected || view.status==Status::wrong_instance ||
                        view.status==Status::client_expired || snapshot.status!=Status::saved) {
                        view.status=snapshot.status; view.error=snapshot.error;
                    }
                    view.detail=L"Connected";
                }
                if (!in_flight) {
                    std::lock_guard lock(mutex);
                    if (desired && std::chrono::steady_clock::now()>=due) {
                        auto command=*desired; command.request.request_id=++request_id;
                        // Each record compacts a contiguous proven own chain from an original
                        // displayed base. Refreshing a snapshot never advances these chains.
                        for (auto& commit:own_commits) {
                            if (command.request.server==commit.server && command.request.client_lease==commit.lease &&
                                command.displayed==commit.before) {
                                command.request.configuration.revision=commit.after.revision;
                                command.displayed=commit.after;
                                commit.last_used=++own_clock;
                                break;
                            }
                        }
                        in_flight=command; desired.reset();
                    }
                }
                if (in_flight) {
                    view.pending=true; view.detail=L"Saving…"; Publish();
                    auto result=transport.exchange(in_flight->request);
                    auto acknowledged=in_flight->request.operation==Operation::reset
                        ? Configuration{} : in_flight->request.configuration;
                    acknowledged.revision=in_flight->request.configuration.revision+1;
                    if (result.status==Status::saved && result.server==in_flight->request.server &&
                        result.client_lease==in_flight->request.client_lease &&
                        in_flight->request.configuration.revision!=UINT64_MAX &&
                        result.configuration==acknowledged) {
                        for (auto& commit:own_commits) {
                            if (commit.server==result.server && commit.lease==result.client_lease &&
                                commit.after==in_flight->displayed) commit.after=result.configuration;
                        }
                        OwnCommit transition{result.server,result.client_lease,in_flight->displayed,
                                             result.configuration,++own_clock};
                        if (own_commits.size()==32) {
                            auto inactive=std::min_element(own_commits.begin(),own_commits.end(),[](auto const& a,auto const& b) {
                                return a.last_used<b.last_used;
                            });
                            *inactive=transition;
                        } else own_commits.push_back(transition);
                    }
                    view.pending=false; view.status=result.status; view.error=result.error;
                    view.settled_edit_generation=std::max(view.settled_edit_generation,in_flight->edit_generation);
                    if (result.server!=last_snapshot.server || result.configuration.revision>=view.configuration.revision) {
                        view.configuration=result.configuration; last_snapshot=result;
                        view.server_instance=result.server; view.client_lease=result.client_lease;
                        view.configuration_authoritative=result.status==Status::saved || result.status==Status::conflict ||
                            (result.status==Status::storage_error && view.configuration_authoritative);
                    }
                    in_flight.reset();
                    if (result.status==Status::conflict || result.status==Status::wrong_instance ||
                        result.status==Status::client_expired) DiscardRequests();
                    if (result.status==Status::wrong_instance || result.status==Status::client_expired) {
                        last_snapshot={}; view.connected=false; view.client_lease=0;
                        view.configuration_authoritative=false;
                    }
                    view.detail=result.status==Status::saved?L"Saved":L"Change was not saved";
                }
                {
                    std::lock_guard lock(mutex);
                    view.pending=view.pending||desired.has_value();
                }
                Publish();
                std::unique_lock lock(mutex);
                wake.wait_for(lock,std::chrono::milliseconds(850),[&]{return stop || desired.has_value();});
                if (stop) return;
            } catch (...) {
                view.connected=false; view.pending=false; view.status=Status::disconnected;
                view.configuration_authoritative=false;
                view.detail=ever_connected?L"Host disconnected. Restart it explicitly; settings will reconnect.":
                    L"Host unavailable or identity verification failed.";
                Publish();
                if (!ever_connected && !launch_attempted) {
                    launch_attempted=true;
                    try {
                        if (!transport.start_host()) { view.detail=L"Cannot start the trusted companion host."; Publish(); }
                    } catch (...) { view.detail=L"Companion host launch failed."; Publish(); }
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock,std::chrono::seconds(1),[&]{return stop;});
                if (stop) return;
            }
        }
    }
};
Session::Session(Options options,std::function<void(ConnectionState)> publish,SessionTransport transport)
    : impl_(std::make_unique<Impl>(std::move(options),std::move(publish),std::move(transport))) {
    impl_->worker=std::thread([state=impl_.get()]{state->Run();});
}
Session::~Session() { Stop(); }
std::uint64_t Session::Change(ConnectionState const& displayed,bool paused,float width) {
    auto appearance=displayed.configuration;
    appearance.paused=paused; appearance.border_width_dip=width;
    return Change(displayed,appearance);
}
std::uint64_t Session::Change(ConnectionState const& displayed,Configuration const& appearance) {
    Message request; request.configuration=appearance;
    request.configuration.eligibility=displayed.configuration.eligibility;
    request.configuration.revision=displayed.configuration.revision;
    if (!Valid(request.configuration) || !displayed.connected || displayed.client_lease==0 ||
        EmptyIdentity(displayed.server_instance)) return 0;
    request.operation=Operation::mutate; request.client=impl_->client;
    request.server=displayed.server_instance; request.client_lease=displayed.client_lease;
    std::lock_guard lock(impl_->mutex);
    auto generation=++impl_->last_edit_generation;
    impl_->desired=Impl::Desired{request,displayed.configuration,generation};
    impl_->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(150);
    impl_->wake.notify_one();
    return generation;
}
std::uint64_t Session::Reset(ConnectionState const& displayed) {
    if (!displayed.connected || displayed.client_lease==0 || EmptyIdentity(displayed.server_instance)) return 0;
    Message request; request.operation=Operation::reset; request.client=impl_->client;
    request.server=displayed.server_instance; request.client_lease=displayed.client_lease;
    request.configuration=displayed.configuration;
    std::lock_guard lock(impl_->mutex);
    auto generation=++impl_->last_edit_generation;
    impl_->desired=Impl::Desired{request,displayed.configuration,generation};
    impl_->due=std::chrono::steady_clock::now(); impl_->wake.notify_one();
    return generation;
}
void Session::Stop() noexcept {
    { std::lock_guard lock(impl_->mutex); impl_->stop=true; impl_->wake.notify_one(); }
    if (impl_->worker.joinable()) impl_->worker.join();
}
DraftState::Update DraftState::Apply(ConnectionState const& state) {
    if (state.publication_sequence<=current_.publication_sequence) return {};
    current_=state;
    if (state.connected && state.configuration_authoritative) last_confirmed_=state.configuration;
    if (Pending() || state.pending) return {true,false};
    displayed_=state;
    controls_=!state.connected && last_confirmed_?*last_confirmed_:state.configuration;
    return {true,true};
}
void DraftState::Begin(std::uint64_t generation,Configuration const& appearance) {
    if (!generation || generation<=edit_generation_) return;
    if (!Valid(appearance)) throw std::invalid_argument("Invalid settings draft");
    edit_generation_=generation; controls_=appearance;
}
struct AppsSession::Impl {
    struct Edit { Message request; std::uint64_t generation; };
    Impl(Options const& options,std::function<void(AppsConnectionState)> callback,SessionTransport transport)
        :publish(std::move(callback)),exchange(std::move(transport.exchange)),client(NewIdentity()) {
        if (!exchange) exchange=[endpoint=EndpointName(options.endpoint_suffix),
            image=SiblingExecutable(L"WindowSweaters.Host.exe")](Message const& request) {
            return Exchange(endpoint,image,request);
        };
    }
    std::function<void(AppsConnectionState)> publish;
    std::function<Message(Message const&)> exchange;
    Identity client;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Edit> queued,in_flight;
    bool stop=false,busy=false;
    std::uint64_t request_id{},generation{};
    AppsConnectionState view;
    Message last_snapshot;
    std::thread worker;
    void Publish() noexcept {
        ++view.connection.publication_sequence;
        try { publish(view); } catch (...) { OutputDebugStringW(L"WindowSweaters: apps publication failed.\n"); }
    }
    void Discard() {
        std::lock_guard lock(mutex);
        queued.reset(); in_flight.reset(); busy=false;
        view.connection.pending=false; view.connection.settled_edit_generation=generation;
    }
    void Run() noexcept {
        for (;;) {
            {
                std::unique_lock lock(mutex);
                if (stop) return;
                if (!in_flight && queued) { in_flight=std::move(queued); queued.reset(); }
            }
            try {
                if (in_flight) {
                    view.connection.pending=true; view.connection.detail=L"Saving application choice…"; Publish();
                    auto result=exchange(in_flight->request);
                    if (result.operation!=Operation::response || result.response_to!=Operation::eligibility ||
                        result.client!=client || result.request_id!=in_flight->request.request_id ||
                        (result.server!=in_flight->request.server && result.status!=Status::wrong_instance) ||
                        result.client_lease!=in_flight->request.client_lease)
                        throw std::runtime_error("Invalid eligibility acknowledgement");
                    if (result.status==Status::saved) {
                        auto expected=in_flight->request.configuration;
                        auto applied=expected.eligibility.Apply(in_flight->request.eligibility);
                        auto preferences=std::get_if<applications::EligibilityPreferences>(&applied);
                        if (!preferences || expected.revision==UINT64_MAX) throw std::runtime_error("Invalid acknowledged edit");
                        expected.eligibility=*preferences; ++expected.revision;
                        if (result.configuration!=expected) throw std::runtime_error("Invalid committed eligibility result");
                    }
                    view.connection.configuration=result.configuration;
                    view.connection.configuration_authoritative=result.status==Status::saved || result.status==Status::conflict;
                    view.connection.status=result.status; view.connection.error=result.error;
                    view.connection.detail=result.status==Status::saved?L"Application choice saved":L"Application choice was not saved";
                    view.connection.settled_edit_generation=in_flight->generation;
                    view.connection.pending=false;
                    {
                        std::lock_guard lock(mutex);
                        in_flight.reset(); busy=false;
                    }
                    if (result.status==Status::wrong_instance || result.status==Status::client_expired) {
                        last_snapshot={}; Discard(); view.connection.connected=false;
                    }
                    // Keep the displayed list coherent with the typed terminal
                    // reply until a new host-owned observation arrives.
                    view.snapshot.configuration_revision=result.configuration.revision;
                    view.snapshot.default_enabled=result.configuration.eligibility.DefaultEnabled();
                    for (auto& row:view.snapshot.applications) {
                        row.enabled=result.configuration.eligibility.Allows(row.identity);
                        row.explicit_preference=result.configuration.eligibility.Exceptions().contains(row.identity);
                    }
                    std::erase_if(view.snapshot.applications,[](auto const& row) {
                        return row.presence==applications::ApplicationPresence::SavedNotRunning && !row.explicit_preference;
                    });
                    Publish();
                }
                Message request; request.operation=Operation::applications_snapshot;
                request.client=client;
                { std::lock_guard lock(mutex); request.request_id=++request_id; }
                request.server=last_snapshot.server; request.client_lease=last_snapshot.client_lease;
                auto result=exchange(request);
                if (result.operation!=Operation::response || result.response_to!=Operation::applications_snapshot ||
                    result.request_id!=request.request_id || result.client!=client || EmptyIdentity(result.server) ||
                    result.applications.configuration_revision!=result.configuration.revision)
                    throw std::runtime_error("Invalid applications observation");
                if (result.status==Status::wrong_instance || result.status==Status::client_expired) {
                    Discard(); last_snapshot={}; view.connection.connected=false;
                    view.connection.configuration_authoritative=false;
                    view.connection.status=result.status; Publish();
                } else {
                    if (!EmptyIdentity(last_snapshot.server) && result.server!=last_snapshot.server) Discard();
                    last_snapshot=result; view.connection.connected=true;
                    view.connection.server_instance=result.server; view.connection.client_lease=result.client_lease;
                    view.connection.configuration=result.configuration;
                    view.connection.configuration_authoritative=result.status==Status::saved;
                    if (view.connection.status==Status::disconnected || view.connection.status==Status::client_expired ||
                        view.connection.status==Status::wrong_instance ||
                        view.connection.status==Status::recovery_required || result.status!=Status::saved) {
                        view.connection.status=result.status; view.connection.error=result.error;
                    }
                    view.snapshot=std::move(result.applications);
                    { std::lock_guard lock(mutex); view.connection.pending=busy; }
                    Publish();
                }
            } catch (...) {
                view.connection.connected=false; view.connection.configuration_authoritative=false;
                view.connection.status=Status::disconnected;
                view.connection.detail=L"Host disconnected. Application choices remain unchanged.";
                Publish();
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock,std::chrono::milliseconds(750),[&]{return stop || queued.has_value();});
            if (stop) return;
        }
    }
};
AppsSession::AppsSession(Options options,std::function<void(AppsConnectionState)> publish,SessionTransport transport)
    :impl_(std::make_unique<Impl>(options,std::move(publish),std::move(transport))) {
    impl_->worker=std::thread([state=impl_.get()]{state->Run();});
}
AppsSession::~AppsSession() { Stop(); }
std::uint64_t AppsSession::Change(AppsConnectionState const& displayed,applications::EligibilityMutation mutation) {
    auto const& connection=displayed.connection;
    if (!connection.connected || !connection.configuration_authoritative || !connection.client_lease ||
        EmptyIdentity(connection.server_instance) ||
        displayed.snapshot.configuration_revision!=connection.configuration.revision) return 0;
    auto proposed=connection.configuration.eligibility.Apply(mutation);
    auto preferences=std::get_if<applications::EligibilityPreferences>(&proposed);
    if (!preferences || !BoundedPreferences(*preferences)) return 0;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stop || impl_->busy || impl_->generation==UINT64_MAX || impl_->request_id==UINT64_MAX) return 0;
    Message request; request.operation=Operation::eligibility; request.client=impl_->client;
    request.server=connection.server_instance; request.client_lease=connection.client_lease;
    request.request_id=++impl_->request_id; request.configuration=connection.configuration;
    request.eligibility=std::move(mutation);
    auto generation=++impl_->generation;
    impl_->queued=Impl::Edit{std::move(request),generation}; impl_->busy=true; impl_->wake.notify_one();
    return generation;
}
void AppsSession::Stop() noexcept {
    { std::lock_guard lock(impl_->mutex); impl_->stop=true; impl_->wake.notify_one(); }
    if (impl_->worker.joinable()) impl_->worker.join();
}
bool AppsDraftState::Apply(AppsConnectionState const& state) {
    if (state.connection.publication_sequence<=current_.connection.publication_sequence) return false;
    current_=state;
    if (Pending() || state.connection.pending) return false;
    if (!state.connection.connected || !state.connection.configuration_authoritative) return false;
    displayed_=state; controls_=state.snapshot; return true;
}
void AppsDraftState::Begin(std::uint64_t generation,applications::EligibilityMutation const& mutation) {
    if (!generation || generation<=generation_) return;
    auto applied=displayed_.connection.configuration.eligibility.Apply(mutation);
    auto preferences=std::get_if<applications::EligibilityPreferences>(&applied);
    if (!preferences) throw std::invalid_argument("Invalid apps draft");
    generation_=generation; controls_.default_enabled=preferences->DefaultEnabled();
    for (auto& row:controls_.applications) {
        row.enabled=preferences->Allows(row.identity);
        row.explicit_preference=preferences->Exceptions().contains(row.identity);
    }
}
}
