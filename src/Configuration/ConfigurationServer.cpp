#include "ConfigurationServer.h"
#include "Storage.h"
#include "Pipe.h"
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <sddl.h>
#include <thread>
#include <vector>

namespace window_sweaters::configuration {
using namespace contracts;
struct ConfigurationServer::Impl {
    explicit Impl(ServerOptions value) : options(std::move(value)), storage(options.storage_directory),
        endpoint(EndpointName(options.endpoint_suffix)), server(NewIdentity()), local_client(NewIdentity()) {
        auto loaded=storage.Load();
        committed=loaded.configuration; storage_state=loaded.state; storage_error=loaded.error;
        deduplicated.reserve(257);
        leases.reserve(maximum_client_leases);
        if (options.expected_settings_path.empty()) options.expected_settings_path=SiblingExecutable(L"WindowSweaters.Settings.exe");
    }
    struct Job { Message request; std::promise<MutationResult> result; bool pause_width_only=false; };
    struct CommittedRequest { Message request; MutationResult result; };
    struct ClientLease {
        Identity client;
        std::uint64_t token;
        std::uint64_t highest_committed = 0;
        std::uint64_t last_used = 0;
    };
    ServerOptions options;
    Storage storage;
    std::wstring endpoint;
    Identity server, local_client;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::unique_ptr<Job>> pending;
    std::vector<CommittedRequest> deduplicated;
    std::vector<ClientLease> leases;
    std::uint64_t last_lease = 0, lease_clock = 0, local_highest_committed = 0;
    Configuration committed{};
    StorageState storage_state=StorageState::ready;
    DWORD storage_error=ERROR_SUCCESS;
    Handle stop;
    std::thread worker, listener;
    bool running=false;
    bool stopping=false;
    std::atomic<std::uint64_t> local_request{0};

    MutationResult SnapshotResult() const {
        std::lock_guard lock(mutex);
        auto status=storage_state==StorageState::ready?Status::saved:
            storage_state==StorageState::io_error?Status::storage_error:Status::recovery_required;
        return {status,committed,storage_error};
    }
    ClientLease* FindLease(Message const& request) {
        auto lease = std::find_if(leases.begin(),leases.end(),[&](auto const& entry) {
            return entry.token == request.client_lease && entry.client == request.client;
        });
        return lease == leases.end() ? nullptr : &*lease;
    }
    Message ReadSnapshot(Message const& request) {
        auto snapshot=SnapshotResult();
        Message response=request;
        response.operation=Operation::response; response.server=server;
        response.status=snapshot.status; response.configuration=snapshot.configuration;
        response.error=snapshot.error;
        std::lock_guard lock(mutex);
        if (request.client_lease != 0) {
            if (request.server != server) {
                response.status=Status::wrong_instance; response.client_lease=0; return response;
            }
            auto lease=FindLease(request);
            if (!lease) { response.status=Status::client_expired; return response; }
            lease->last_used=++lease_clock;
        } else {
            if (!EmptyIdentity(request.server)) {
                response.status=Status::invalid; return response;
            }
            if (last_lease==std::numeric_limits<std::uint64_t>::max()) {
                response.status=Status::busy; return response;
            }
            if (leases.size()==maximum_client_leases) {
                auto oldest=std::min_element(leases.begin(),leases.end(),[](auto const& a,auto const& b) {
                    return a.last_used<b.last_used;
                });
                *oldest={request.client,++last_lease,0,++lease_clock};
                response.client_lease=oldest->token;
            } else {
                leases.push_back({request.client,++last_lease,0,++lease_clock});
                response.client_lease=last_lease;
            }
        }
        return response;
    }
    std::future<MutationResult> Enqueue(Message const& request,bool pause_width_only=false) {
        auto job=std::make_unique<Job>(); job->request=request; job->pause_width_only=pause_width_only;
        auto future=job->result.get_future();
        std::lock_guard lock(mutex);
        if (!running || stopping) job->result.set_value({Status::shutting_down,committed});
        else if (pending.size()>=maximum_pending_mutations) job->result.set_value({Status::busy,committed});
        else { pending.push_back(std::move(job)); wake.notify_one(); }
        return future;
    }
    MutationResult Apply(Message const& request,bool pause_width_only) {
        auto current=SnapshotResult();
        if (!Valid(request.configuration) || request.request_id==0 || EmptyIdentity(request.client) ||
            request.status!=Status::saved ||
            (request.operation!=Operation::mutate && request.operation!=Operation::reset))
            return {Status::invalid,current.configuration};
        if (request.server!=server) return {Status::wrong_instance,current.configuration};
        // Only an explicit snapshot handshake can admit a client. Retired tokens never
        // become new sessions, so bounded retirement cannot revive an old mutation.
        std::unique_lock lease_lock(mutex);
        bool local=request.client==local_client && request.client_lease==0;
        ClientLease* lease=local?nullptr:FindLease(request);
        if (!local && !lease) return {Status::client_expired,current.configuration};
        if (lease) lease->last_used=++lease_clock;
        for (auto const& entry:deduplicated)
            if (entry.request.client==request.client && entry.request.client_lease==request.client_lease &&
                entry.request.request_id==request.request_id)
                return entry.request==request?entry.result:MutationResult{Status::invalid,current.configuration};
        auto highest=local?local_highest_committed:lease->highest_committed;
        if (request.request_id<=highest)
            return {Status::duplicate_expired,current.configuration};
        if (request.configuration.revision!=current.configuration.revision)
            return {Status::conflict,current.configuration};
        if (current.status!=Status::saved && request.operation!=Operation::reset) return current;
        if (current.configuration.revision==std::numeric_limits<std::uint64_t>::max())
            return {Status::invalid,current.configuration};
        Configuration next=request.operation==Operation::reset?Configuration{}:request.configuration;
        if (pause_width_only) {
            next=current.configuration;
            next.paused=request.configuration.paused;
            next.border_width_dip=request.configuration.border_width_dip;
        }
        next.revision=current.configuration.revision+1;
        lease_lock.unlock();
        auto error=storage.Save(next,request.operation==Operation::reset);
        if (error!=ERROR_SUCCESS) return {Status::storage_error,current.configuration,error};
        {
            std::lock_guard lock(mutex);
            committed=next; storage_state=StorageState::ready; storage_error=ERROR_SUCCESS;
            if (local) local_highest_committed=request.request_id;
            else if (auto active=FindLease(request)) active->highest_committed=request.request_id;
        }
        MutationResult result{Status::saved,next};
        deduplicated.push_back({request,result});
        if (deduplicated.size()>256) deduplicated.erase(deduplicated.begin());
        return result;
    }
    void Worker() noexcept {
        for (;;) {
            std::unique_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return stopping||!pending.empty();});
                if (stopping) {
                    for (auto& remaining:pending) remaining->result.set_value({Status::shutting_down,committed});
                    pending.clear(); return;
                }
                job=std::move(pending.front()); pending.pop_front();
            }
            try { job->result.set_value(Apply(job->request,job->pause_width_only)); }
            catch (...) { job->result.set_value({Status::storage_error,SnapshotResult().configuration}); }
        }
    }
    Handle CreatePipe() {
        auto sddl=PipeSecuritySddl();
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))
            throw std::runtime_error("Cannot create settings pipe security descriptor");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
        Handle pipe(CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,
            static_cast<DWORD>(frame_size),static_cast<DWORD>(frame_size),io_timeout_ms,&security));
        LocalFree(descriptor);
        if (!pipe) throw std::runtime_error("Cannot own settings pipe");
        return pipe;
    }
    void Listen(Handle pipe) noexcept {
        try {
            while (WaitForSingleObject(stop.get(),0)!=WAIT_OBJECT_0) {
                if (Connect(pipe.get(),stop.get())) {
                    std::array<std::byte,frame_size> bytes{};
                    Message request;
                    if (Transfer(pipe.get(),bytes.data(),static_cast<DWORD>(bytes.size()),false,stop.get()) &&
                        VerifyPeer(pipe.get(),false,options.expected_settings_path) &&
                        Decode(bytes,request) && request.operation!=Operation::response &&
                        request.request_id!=0 && !EmptyIdentity(request.client) && request.status==Status::saved) {
                        Message response;
                        if (request.operation==Operation::snapshot) response=ReadSnapshot(request);
                        else {
                            auto future=Enqueue(request);
                            while (future.wait_for(std::chrono::milliseconds(25))!=std::future_status::ready &&
                                   WaitForSingleObject(stop.get(),0)!=WAIT_OBJECT_0) {}
                            auto result=future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready?
                                future.get():MutationResult{Status::shutting_down,SnapshotResult().configuration};
                            response.operation=Operation::response; response.status=result.status;
                            response.request_id=request.request_id; response.client=request.client; response.server=server;
                            response.client_lease=request.client_lease; response.configuration=result.configuration;
                            response.error=result.error;
                        }
                        bytes=Encode(response);
                        if (Transfer(pipe.get(),bytes.data(),static_cast<DWORD>(bytes.size()),true,stop.get())) {
                            std::byte receipt{};
                            Transfer(pipe.get(),&receipt,1,false,stop.get());
                        }
                    }
                }
                DisconnectNamedPipe(pipe.get());
                // Reuse the owned pipe handle so a same-logon peer cannot squat between clients.
            }
        } catch (...) { OutputDebugStringW(L"WindowSweaters: settings IPC listener failed.\n"); }
    }
};
ConfigurationServer::ConfigurationServer(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
ConfigurationServer::~ConfigurationServer() { Stop(); }
void ConfigurationServer::Start() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) return;
    auto pipe=impl_->CreatePipe();
    impl_->stop=Handle(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if (!impl_->stop) throw std::runtime_error("Cannot create settings stop event");
    impl_->stopping=false; impl_->running=true;
    try {
        impl_->worker=std::thread([state=impl_.get()]{state->Worker();});
        impl_->listener=std::thread([state=impl_.get(),pipe=std::move(pipe)]() mutable {state->Listen(std::move(pipe));});
    } catch (...) {
        impl_->stopping=true; impl_->wake.notify_all();
        // Start failures are exceptional; the destructor joins a worker that was created.
        throw;
    }
}
void ConfigurationServer::Stop() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) return;
        impl_->stopping=true; SetEvent(impl_->stop.get()); impl_->wake.notify_all();
    }
    if (impl_->listener.joinable()) impl_->listener.join();
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard lock(impl_->mutex);
    impl_->running=false;
}
Configuration ConfigurationServer::Snapshot() const { return impl_->SnapshotResult().configuration; }
std::wstring ConfigurationServer::Endpoint() const { return impl_->endpoint; }
std::future<MutationResult> ConfigurationServer::SubmitLocal(bool paused,float width,std::uint64_t expected_revision) {
    Message request;
    request.operation=Operation::mutate; request.server=impl_->server; request.client=impl_->local_client;
    request.request_id=++impl_->local_request; request.configuration.revision=expected_revision;
    request.configuration.paused=paused; request.configuration.border_width_dip=width;
    return impl_->Enqueue(request,true);
}
std::future<MutationResult> ConfigurationServer::SubmitLocal(Configuration const& expected) {
    Message request;
    request.operation=Operation::mutate; request.server=impl_->server; request.client=impl_->local_client;
    request.request_id=++impl_->local_request; request.configuration=expected;
    return impl_->Enqueue(request);
}
}
