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
    struct Job {
        Message request;
        std::promise<MutationResult> result;
        bool pause_width_only=false;
        std::size_t charge{};
    };
    struct CommittedRequest {
        Identity client;
        std::uint64_t lease,request_id;
        std::vector<std::byte> wire;
        MutationResult result;
        std::size_t charge;
    };
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
    std::size_t pending_bytes{};
    std::vector<CommittedRequest> deduplicated;
    std::size_t deduplicated_bytes{};
    std::vector<ClientLease> leases;
    std::uint64_t last_lease = 0, lease_clock = 0, local_highest_committed = 0;
    Configuration committed{};
    std::vector<applications::RunningApplicationWindow> application_windows;
    ApplicationsSnapshot application_snapshot;
    // The snapshot is a pure function of committed and application_windows; publication rebuilds eagerly.
    std::optional<std::uint64_t> application_revision;
    std::uint64_t application_builds{};
    std::map<applications::AppIdentity,std::uint64_t> eligibility_revisions;
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
        std::lock_guard lock(mutex);
        Message response=request;
        response.operation=Operation::response; response.response_to=request.operation; response.server=server;
        response.status=storage_state==StorageState::ready?Status::saved:
            storage_state==StorageState::io_error?Status::storage_error:Status::recovery_required;
        response.configuration=committed; response.error=storage_error;
        if (request.operation==Operation::applications_snapshot) {
            if (application_revision!=committed.revision) RefreshApplications();
            response.applications=application_snapshot;
        }
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
    void RefreshApplications(bool observation=false) {
        std::vector<applications::RunningApplicationWindow> windows;
        windows.reserve(application_windows.size());
        for (auto const& window:application_windows) {
            auto token=applications::TargetToken::FromTarget(window.token.Target(),committed.revision);
            if (!token) throw std::runtime_error("Invalid current application target");
            windows.push_back({*token,window.metadata});
        }
        auto built=applications::BuildApplicationSnapshot(committed.eligibility,committed.revision,windows);
        auto snapshot=std::get_if<applications::ApplicationSnapshot>(&built);
        if (!snapshot) throw std::runtime_error("Invalid host application snapshot");
        ApplicationsSnapshot next;
        next.observation_generation=application_snapshot.observation_generation;
        next.configuration_revision=committed.revision; next.default_enabled=committed.eligibility.DefaultEnabled();
        for (auto const& row:snapshot->applications) {
            auto metadata=row.metadata?std::get_if<applications::AppMetadata>(row.metadata.get()):nullptr;
            next.applications.push_back({row.identity,metadata?metadata->DisplayName():L"",row.presence,
                row.enabled,row.explicitPreference,static_cast<std::uint32_t>(row.windowCount),
                static_cast<std::uint32_t>(row.processCount),metadata?std::optional{metadata->Provenance()}:std::nullopt});
        }
        for (auto const& row:snapshot->unavailableOwners)
            next.unavailable.push_back({static_cast<std::uint32_t>(row.windowCount),row.unavailable});
        Message bounded; bounded.operation=Operation::response; bounded.response_to=Operation::applications_snapshot;
        bounded.configuration=committed; bounded.applications=next; (void)Encode(bounded);
        if (observation || next!=application_snapshot) {
            if (next.observation_generation==UINT64_MAX) throw std::overflow_error("Application observations exhausted");
            ++next.observation_generation; application_snapshot=std::move(next);
        }
        application_revision=committed.revision; ++application_builds;
    }
    std::future<MutationResult> Enqueue(Message const& request,bool pause_width_only=false) {
        auto job=std::make_unique<Job>(); job->request=request; job->pause_width_only=pause_width_only;
        auto future=job->result.get_future();
        if (!Valid(request.configuration)) {
            job->result.set_value({Status::invalid,SnapshotResult().configuration}); return future;
        }
        try { job->charge=Encode(request).size(); }
        catch (std::invalid_argument const&) {
            job->result.set_value({Status::invalid,SnapshotResult().configuration}); return future;
        } catch (std::length_error const&) {
            job->result.set_value({Status::invalid,SnapshotResult().configuration}); return future;
        }
        std::lock_guard lock(mutex);
        if (!running || stopping) job->result.set_value({Status::shutting_down,committed});
        else if (pending.size()>=maximum_pending_mutations ||
            job->charge>maximum_pending_mutation_bytes-pending_bytes)
            job->result.set_value({Status::busy,committed});
        else {
            auto charge=job->charge;
            pending.push_back(std::move(job)); pending_bytes+=charge; wake.notify_one();
        }
        return future;
    }
    MutationResult Apply(Message const& request,bool pause_width_only) {
        auto current=SnapshotResult();
        if (!Valid(request.configuration) || request.request_id==0 || EmptyIdentity(request.client) ||
            request.status!=Status::saved ||
            (request.operation!=Operation::mutate && request.operation!=Operation::reset &&
                request.operation!=Operation::eligibility))
            return {Status::invalid,current.configuration};
        if (request.server!=server) return {Status::wrong_instance,current.configuration};
        // Only an explicit snapshot handshake can admit a client. Retired tokens never
        // become new sessions, so bounded retirement cannot revive an old mutation.
        std::unique_lock lease_lock(mutex);
        bool local=request.client==local_client && request.client_lease==0;
        ClientLease* lease=local?nullptr:FindLease(request);
        if (!local && !lease) return {Status::client_expired,current.configuration};
        if (lease) lease->last_used=++lease_clock;
        auto wire=Encode(request);
        for (auto const& entry:deduplicated)
            if (entry.client==request.client && entry.lease==request.client_lease &&
                entry.request_id==request.request_id)
                return entry.wire==wire?entry.result:MutationResult{Status::invalid,current.configuration};
        auto highest=local?local_highest_committed:lease->highest_committed;
        if (request.request_id<=highest)
            return {Status::duplicate_expired,current.configuration};
        if (request.configuration.revision!=current.configuration.revision)
            return {Status::conflict,current.configuration};
        if (current.status!=Status::saved && request.operation!=Operation::reset) return current;
        if (current.configuration.revision==std::numeric_limits<std::uint64_t>::max())
            return {Status::invalid,current.configuration};
        Configuration next=request.operation==Operation::reset?Configuration{}:request.configuration;
        if (request.operation==Operation::mutate) next.eligibility=current.configuration.eligibility;
        if (request.operation==Operation::eligibility) {
            auto applied=current.configuration.eligibility.Apply(request.eligibility);
            auto preferences=std::get_if<applications::EligibilityPreferences>(&applied);
            if (!preferences || !BoundedPreferences(*preferences)) return {Status::invalid,current.configuration};
            next=current.configuration; next.eligibility=std::move(*preferences);
        }
        if (pause_width_only) {
            next=current.configuration;
            next.paused=request.configuration.paused;
            next.border_width_dip=request.configuration.border_width_dip;
        }
        next.revision=current.configuration.revision+1;
        MutationResult result{Status::saved,next};
        Message retained; retained.configuration=next;
        auto charge=wire.size()+Encode(retained).size();
        CommittedRequest record{request.client,request.client_lease,request.request_id,std::move(wire),
            {Status::saved,next},charge};
        lease_lock.unlock();
        auto error=storage.Save(next,request.operation==Operation::reset);
        if (error!=ERROR_SUCCESS) return {Status::storage_error,current.configuration,error};
        {
            std::lock_guard lock(mutex);
            for (auto& [identity,revision]:eligibility_revisions)
                if (committed.eligibility.Allows(identity)!=next.eligibility.Allows(identity))
                    revision=next.revision;
            committed=std::move(next); storage_state=StorageState::ready; storage_error=ERROR_SUCCESS;
            if (local) local_highest_committed=request.request_id;
            else if (auto active=FindLease(request)) active->highest_committed=request.request_id;
        }
        deduplicated_bytes+=record.charge;
        deduplicated.push_back(std::move(record));
        while (deduplicated.size()>256 || deduplicated_bytes>maximum_deduplicated_bytes) {
            deduplicated_bytes-=deduplicated.front().charge;
            deduplicated.erase(deduplicated.begin());
        }
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
                    pending.clear(); pending_bytes=0; return;
                }
                job=std::move(pending.front()); pending.pop_front(); pending_bytes-=job->charge;
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
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,
            65536,65536,io_timeout_ms,&security));
        LocalFree(descriptor);
        if (!pipe) throw std::runtime_error("Cannot own settings pipe");
        return pipe;
    }
    void Listen(Handle pipe) noexcept {
        try {
            while (WaitForSingleObject(stop.get(),0)!=WAIT_OBJECT_0) {
                if (Connect(pipe.get(),stop.get())) {
                    std::vector<std::byte> bytes;
                    Message request;
                    if (ReadFrame(pipe.get(),bytes,stop.get()) &&
                        VerifyPeer(pipe.get(),false,options.expected_settings_path) &&
                        Decode(bytes,request) && request.operation!=Operation::response &&
                        request.request_id!=0 && !EmptyIdentity(request.client) && request.status==Status::saved) {
                        Message response;
                        if (request.operation==Operation::snapshot || request.operation==Operation::applications_snapshot)
                            response=ReadSnapshot(request);
                        else {
                            auto future=Enqueue(request);
                            while (future.wait_for(std::chrono::milliseconds(25))!=std::future_status::ready &&
                                   WaitForSingleObject(stop.get(),0)!=WAIT_OBJECT_0) {}
                            auto result=future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready?
                                future.get():MutationResult{Status::shutting_down,SnapshotResult().configuration};
                            response.operation=Operation::response; response.status=result.status;
                            response.response_to=request.operation;
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
std::uint64_t ConfigurationServer::Revision() const {
    std::lock_guard lock(impl_->mutex); return impl_->committed.revision;
}
std::uint64_t ConfigurationServer::EligibilityRevision(applications::AppIdentity const& identity) const {
    std::lock_guard lock(impl_->mutex);
    auto found=impl_->eligibility_revisions.find(identity);
    return found==impl_->eligibility_revisions.end()?impl_->committed.revision:found->second;
}
void ConfigurationServer::PublishApplications(std::vector<applications::RunningApplicationWindow> windows) {
    if (windows.size()>4096) throw std::length_error("Too many running windows");
    std::lock_guard lock(impl_->mutex);
    std::map<applications::AppIdentity,std::uint64_t> revisions;
    std::size_t retained=0;
    for (auto const& window:windows) {
        if (!window.metadata) throw std::invalid_argument("Missing owner metadata");
        if (auto known=std::get_if<applications::AppMetadata>(window.metadata.get())) {
            if (known->RetainedBytes()>maximum_running_metadata_bytes-retained)
                throw std::length_error("Running metadata exceeds aggregate bound");
            retained+=known->RetainedBytes();
            auto previous=impl_->eligibility_revisions.find(known->Identity());
            revisions.try_emplace(known->Identity(),previous==impl_->eligibility_revisions.end()?
                impl_->committed.revision:previous->second);
        }
    }
    impl_->eligibility_revisions=std::move(revisions);
    impl_->application_windows=std::move(windows); impl_->RefreshApplications(true);
}
std::uint64_t ConfigurationServer::ApplicationBuilds() const {
    std::lock_guard lock(impl_->mutex); return impl_->application_builds;
}
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
std::future<MutationResult> ConfigurationServer::SubmitEligibility(applications::EligibilityMutation mutation,
    std::uint64_t expected_revision) {
    Message request; request.operation=Operation::eligibility; request.server=impl_->server;
    request.client=impl_->local_client; request.request_id=++impl_->local_request;
    request.configuration.revision=expected_revision; request.eligibility=std::move(mutation);
    return impl_->Enqueue(request);
}
}
