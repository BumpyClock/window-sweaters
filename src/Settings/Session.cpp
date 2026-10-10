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
    struct Desired { Message request; Configuration displayed; };
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
    bool stop=false,ever_connected=false,launch_attempted=false;
    std::thread worker;
    ConnectionState view;
    Message last_snapshot;
    std::optional<Desired> in_flight;
    std::vector<OwnCommit> own_commits;
    std::uint64_t own_clock=0;
    void Publish() noexcept {
        try { publish(view); } catch (...) { OutputDebugStringW(L"WindowSweaters: settings status publication failed.\n"); }
    }
    void DiscardRequests() {
        in_flight.reset();
        own_commits.clear();
        view.pending=false;
        std::lock_guard lock(mutex); desired.reset();
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
                        view.configuration=snapshot.configuration; view.server_instance=snapshot.server; view.client_lease=0;
                        view.detail=L"Connection expired. Pending changes were discarded.";
                        Publish(); continue;
                    }
                    if (!EmptyIdentity(last_snapshot.server) && snapshot.server!=last_snapshot.server) DiscardRequests();
                    last_snapshot=snapshot; view.connected=true; ever_connected=true;
                    view.configuration=snapshot.configuration;
                    view.server_instance=snapshot.server; view.client_lease=snapshot.client_lease;
                    if (view.status==Status::disconnected || snapshot.status!=Status::saved) view.status=snapshot.status;
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
                    if (result.status==Status::saved && result.server==in_flight->request.server &&
                        result.client_lease==in_flight->request.client_lease &&
                        in_flight->request.configuration.revision!=UINT64_MAX &&
                        result.configuration.revision==in_flight->request.configuration.revision+1) {
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
                    view.pending=false; view.status=result.status;
                    if (result.server!=last_snapshot.server || result.configuration.revision>=view.configuration.revision) {
                        view.configuration=result.configuration; last_snapshot=result;
                        view.server_instance=result.server; view.client_lease=result.client_lease;
                    }
                    in_flight.reset();
                    if (result.status==Status::conflict || result.status==Status::wrong_instance ||
                        result.status==Status::client_expired) DiscardRequests();
                    if (result.status==Status::wrong_instance || result.status==Status::client_expired) {
                        last_snapshot={}; view.connected=false; view.client_lease=0;
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
void Session::Change(ConnectionState const& displayed,bool paused,float width) {
    Message request; request.configuration=displayed.configuration;
    request.configuration.paused=paused; request.configuration.border_width_dip=width;
    if (!Valid(request.configuration) || !displayed.connected || displayed.client_lease==0 ||
        EmptyIdentity(displayed.server_instance)) return;
    request.operation=Operation::mutate; request.client=impl_->client;
    request.server=displayed.server_instance; request.client_lease=displayed.client_lease;
    std::lock_guard lock(impl_->mutex);
    impl_->desired=Impl::Desired{request,displayed.configuration};
    impl_->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(150);
    impl_->wake.notify_one();
}
void Session::Reset(ConnectionState const& displayed) {
    if (!displayed.connected || displayed.client_lease==0 || EmptyIdentity(displayed.server_instance)) return;
    Message request; request.operation=Operation::reset; request.client=impl_->client;
    request.server=displayed.server_instance; request.client_lease=displayed.client_lease;
    request.configuration=displayed.configuration;
    std::lock_guard lock(impl_->mutex);
    impl_->desired=Impl::Desired{request,displayed.configuration};
    impl_->due=std::chrono::steady_clock::now(); impl_->wake.notify_one();
}
void Session::Stop() noexcept {
    { std::lock_guard lock(impl_->mutex); impl_->stop=true; impl_->wake.notify_one(); }
    if (impl_->worker.joinable()) impl_->worker.join();
}
}
