#include "..\..\src\Settings\Session.h"
#include "..\..\src\Configuration\Pipe.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

using namespace window_sweaters;
using namespace window_sweaters::contracts;
using namespace window_sweaters::configuration;
namespace {
int checks{};
void Check(bool value,char const* label) { ++checks; if (!value) throw std::runtime_error(label); }
std::filesystem::path Self() {
    wchar_t text[32768]{}; GetModuleFileNameW(nullptr,text,32768); return text;
}
struct Views {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<settings::AppsConnectionState> states;
    void Publish(settings::AppsConnectionState state) {
        std::lock_guard lock(mutex); states.push_back(std::move(state)); changed.notify_all();
    }
    template<class Predicate> settings::AppsConnectionState Wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        auto find=[&]() {
            for (auto it=states.rbegin();it!=states.rend();++it) if (predicate(*it)) return true;
            return false;
        };
        Check(changed.wait_for(lock,std::chrono::seconds(20),find),"AppsSession publishes expected real-IPC state");
        for (auto it=states.rbegin();it!=states.rend();++it) if (predicate(*it)) return *it;
        throw std::runtime_error("Missing apps state");
    }
};
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{},released{};
    void Block() {
        std::unique_lock lock(mutex); entered=true; changed.notify_all();
        if (!changed.wait_for(lock,std::chrono::seconds(15),[&]{return released;}))
            throw std::runtime_error("Apps exchange gate timed out");
    }
    void Wait() {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock,std::chrono::seconds(10),[&]{return entered;}),"Apps request reaches real exchange gate");
    }
    void Release() { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
    ~Gate() { Release(); }
};
applications::RunningApplicationWindow Fixture() {
    auto token=applications::TargetToken::FromTarget({100,50,5000,1},0);
    auto metadata=applications::ResolveMetadata(applications::NoPackagedIdentity{},
        applications::ExecutablePath{L"C:\\PrivateFixture\\app.exe"});
    return {*token,std::make_shared<applications::MetadataResult const>(std::move(metadata))};
}
void RawRejected(ConfigurationServer& server,std::vector<std::byte> bytes) {
    Check(WaitNamedPipeW(server.Endpoint().c_str(),io_timeout_ms)!=FALSE,"Malformed app frame connects to owned IPC");
    Handle pipe(CreateFileW(server.Endpoint().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    Check(bool(pipe),"Open scoped malformed IPC fixture");
    Check(Transfer(pipe.get(),bytes.data(),static_cast<DWORD>(bytes.size()),true),"Malformed app frame reaches real IPC");
    std::array<std::byte,12> reply;
    Check(!Transfer(pipe.get(),reply.data(),static_cast<DWORD>(reply.size()),false),"Malformed app frame has no success acknowledgement");
}
void SessionCases(std::filesystem::path const& root) {
    settings::Options options{true,root/L"apps-session",L"apps-session-"+std::to_wstring(GetCurrentProcessId())};
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()});
    server.Start(); server.PublishApplications({Fixture()});
    Views views; Gate gate;
    std::atomic<unsigned> launches{},commands{},lost_ack{};
    std::atomic<bool> block=true,lose=false;
    settings::SessionTransport transport;
    transport.start_host=[&]{++launches; return false;};
    transport.exchange=[&](Message const& request) {
        if (request.operation==Operation::eligibility) {
            ++commands;
            if (block.exchange(false)) gate.Block();
            auto reply=Exchange(server.Endpoint(),Self(),request);
            if (lose.exchange(false)) { ++lost_ack; throw std::runtime_error("Injected lost eligibility reply"); }
            return reply;
        }
        return Exchange(server.Endpoint(),Self(),request);
    };
    settings::AppsSession session(options,[&](auto state){views.Publish(std::move(state));},transport);
    auto initial=views.Wait([](auto const& state){return state.connection.connected && state.snapshot.applications.size()==1;});
    settings::AppsDraftState draft; Check(draft.Apply(initial),"Actual AppsSession feeds production draft reducer");
    auto identity=std::get<applications::AppMetadata>(*Fixture().metadata).Identity();
    applications::EligibilityMutation disable;
    disable.edits.emplace_back(applications::SetApplicationPreference{identity,false});
    auto generation=session.Change(draft.Displayed(),disable);
    Check(generation==1,"Native apps command returns accepted generation");
    draft.Begin(generation,disable); gate.Wait();
    Check(session.Change(initial,{false,true,{}})==0,"Pending application command cannot be overwritten by a second UI edit");
    auto foreign=server.SubmitLocal(false,29,0).get();
    Check(foreign.status==Status::saved,"Concurrent appearance mutation commits before gated app command");
    gate.Release();
    auto conflict=views.Wait([](auto const& state){return state.connection.status==Status::conflict &&
        state.connection.settled_edit_generation==1;});
    Check(conflict.connection.configuration.revision==1 && conflict.connection.configuration.border_width_dip==29 &&
        conflict.connection.configuration.eligibility.Exceptions().empty(),"App command conflicts against exact displayed revision, not refreshed appearance");
    auto current=views.Wait([&](auto const& state){return state.connection.configuration_authoritative &&
        state.connection.configuration.revision==1 && !state.connection.pending &&
        state.connection.settled_edit_generation==1 &&
        state.connection.publication_sequence>conflict.connection.publication_sequence;});
    Check(draft.Apply(current) && draft.Controls().applications[0].enabled && !draft.Pending(),
        "Terminal conflict refreshes native app toggle to committed owner state");
    Check(current.connection.status==Status::conflict && draft.CanEdit(),
        "Authoritative apps refresh retains relevant conflict text while restoring production edit controls");
    lose=true;
    auto next=session.Change(current,disable);
    Check(next==2,"New exact displayed Apps command accepted after conflict");
    auto saved=views.Wait([](auto const& state){return state.connection.status==Status::saved &&
        state.connection.configuration.revision==2 && state.connection.settled_edit_generation==2;});
    Check(lost_ack==1 && commands==3 && !server.Snapshot().eligibility.Allows(identity) &&
        server.Snapshot().revision==2,"Lost typed reply retries same request and cannot double-commit");
    auto observation=saved.snapshot.observation_generation;
    server.PublishApplications({});
    auto stopped=views.Wait([&](auto const& state){return state.snapshot.observation_generation>observation &&
        state.snapshot.applications.size()==1 && state.snapshot.applications[0].presence==applications::ApplicationPresence::SavedNotRunning;});
    Check(stopped.connection.configuration.revision==2 && stopped.snapshot.applications[0].explicit_preference,
        "Independent apps polling observes running-to-saved transition without config mutation");
    {
        Handle locked(CreateFileW((options.storage_directory/L"configuration.bin").c_str(),GENERIC_READ,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Check(bool(locked),"Hold private committed configuration to inject AppsSession save failure");
        auto failedGeneration=session.Change(stopped,{true,true,{}});
        Check(failedGeneration>0,"AppsSession accepts displayed save-failure fixture choice");
        auto failure=views.Wait([&](auto const& state) {
            return state.connection.status==Status::storage_error &&
                state.connection.settled_edit_generation==failedGeneration && !state.connection.pending;
        });
        auto refreshed=views.Wait([&](auto const& state) {
            return state.connection.configuration_authoritative && !state.connection.pending &&
                state.connection.publication_sequence>failure.connection.publication_sequence;
        });
        Check(refreshed.connection.status==Status::storage_error && refreshed.connection.error==failure.connection.error &&
            refreshed.connection.error!=0 && refreshed.connection.configuration.revision==2 &&
            draft.Apply(refreshed) && draft.CanEdit() &&
            refreshed.connection.configuration.eligibility.DefaultEnabled() &&
            !refreshed.connection.configuration.eligibility.Allows(identity),
            "Authoritative apps refresh retains relevant storage error and committed choices without disabling recovered controls");
    }
    server.Stop();
    views.Wait([](auto const& state){return !state.connection.connected;});
    Check(launches==0,"Apps worker never automatically restarts Host after Quit");
    server.Start();
    auto reconnected=views.Wait([&](auto const& state){return state.connection.connected &&
        state.connection.publication_sequence>stopped.connection.publication_sequence &&
        state.connection.configuration.revision==2;});
    Check(reconnected.connection.client_lease!=0,"Apps worker reconnects to explicit restarted Host");
    session.Stop();
    Message request; request.client=NewIdentity(); request.request_id=1;
    auto base=Exchange(server.Endpoint(),Self(),request);
    request=base; request.operation=Operation::eligibility; request.request_id=2;
    request.eligibility={false,true,{}};
    auto bytes=Encode(request); bytes[176]=std::byte{99}; RawRejected(server,bytes);
    bytes=Encode(request);
    auto maximum=static_cast<std::uint32_t>(maximum_payload_bytes+1);
    for (unsigned i=0;i<4;++i) bytes[8+i]=std::byte((maximum>>(8*i))&255);
    RawRejected(server,bytes);
    bytes=Encode(request); bytes.push_back(std::byte{0}); RawRejected(server,bytes);
    {
        Check(WaitNamedPipeW(server.Endpoint().c_str(),io_timeout_ms)!=FALSE,"Connect scoped slow-frame fixture");
        Handle slow(CreateFileW(server.Endpoint().c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
        bytes=Encode(request);
        Check(bool(slow) && Transfer(slow.get(),bytes.data(),13,true),"Slow frame supplies valid header and first body byte");
        bool connected=true;
        for (unsigned i=0;i<3 && connected;++i) {
            Sleep(1500);
            connected=Transfer(slow.get(),bytes.data()+13+i,1,true);
        }
        if (connected) connected=Transfer(slow.get(),bytes.data()+16,static_cast<DWORD>(bytes.size()-16),true);
        std::vector<std::byte> reply;
        Check(!connected || !ReadFrame(slow.get(),reply),"Partial progress cannot extend the total framed I/O deadline");
    }
    Check(server.Snapshot().revision==2 && !server.Snapshot().eligibility.Allows(identity),
        "Malformed, oversized and trailing-data IPC preserve committed app preferences");
    server.Stop();
}
void RecoveryCases(std::filesystem::path const& root,bool newer) {
    auto name=newer?L"apps-newer-recovery":L"apps-corrupt-recovery";
    settings::Options options{true,root/name,std::wstring(name)+L"-"+std::to_wstring(GetCurrentProcessId())};
    std::filesystem::create_directories(options.storage_directory);
    auto file=options.storage_directory/L"configuration.bin";
    std::vector<char> original;
    if (newer) {
        original.resize(12);
        auto put=[&](std::size_t at,std::uint32_t value) {
            for (unsigned i=0;i<4;++i) original[at+i]=static_cast<char>((value>>(8*i))&255);
        };
        put(0,0x31435357); put(4,configuration_schema+1); put(8,12);
    } else original={'c','o','r','r','u','p','t'};
    {
        std::ofstream output(file,std::ios::binary);
        output.write(original.data(),static_cast<std::streamsize>(original.size()));
    }
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()});
    server.Start(); server.PublishApplications({Fixture()});
    Views appsViews;
    std::atomic<unsigned> launches{};
    settings::SessionTransport transport;
    transport.exchange=[&](Message const& request){return Exchange(server.Endpoint(),Self(),request);};
    transport.start_host=[&]{++launches; return false;};
    settings::AppsSession apps(options,[&](auto state){appsViews.Publish(std::move(state));},transport);
    auto recovery=appsViews.Wait([](auto const& state) {
        return state.connection.connected && state.connection.status==Status::recovery_required;
    });
    settings::AppsDraftState draft;
    Check(!draft.Apply(recovery) && !draft.CanEdit() && !recovery.connection.configuration_authoritative,
        "Actual native Apps edit gate disables controls while corrupt/newer host configuration requires recovery");
    std::mutex mutex;
    std::condition_variable changed;
    settings::ConnectionState appearanceState;
    settings::Session appearance(options,[&](auto state) {
        std::lock_guard lock(mutex); appearanceState=std::move(state); changed.notify_all();
    },transport);
    auto waitAppearance=[&](Status status,std::uint64_t revision) {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock,std::chrono::seconds(20),[&] {
            return appearanceState.connected && !appearanceState.pending && appearanceState.status==status &&
                appearanceState.configuration.revision==revision;
        }),"Appearance Session publishes real recovery/reset acknowledgement");
        return appearanceState;
    };
    auto displayed=waitAppearance(Status::recovery_required,0);
    auto generation=appearance.Reset(displayed);
    Check(generation>0,"Actual appearance-session Preserve and reset command accepts displayed recovery base");
    auto reset=waitAppearance(Status::saved,1);
    Check(reset.settled_edit_generation==generation && reset.configuration_authoritative &&
        reset.configuration==server.Snapshot(),"Appearance reset acknowledges durable authoritative defaults");
    auto refreshed=appsViews.Wait([&](auto const& state) {
        return state.connection.publication_sequence>recovery.connection.publication_sequence &&
            state.connection.connected && state.connection.configuration_authoritative &&
            state.connection.configuration.revision==1;
    });
    Check(refreshed.connection.status==Status::saved && refreshed.connection.error==0 &&
        refreshed.connection.server_instance==recovery.connection.server_instance &&
        refreshed.connection.client_lease==recovery.connection.client_lease &&
        draft.Apply(refreshed) && draft.CanEdit() && draft.Controls().applications.size()==1,
        "Same native Apps Session and production edit gate recover after authoritative appearance reset without reopen");
    auto edit=apps.Change(draft.Displayed(),{false,true,{}});
    Check(edit>0,"Recovered Apps controls submit a new exact-revision eligibility edit without reopening");
    auto saved=appsViews.Wait([](auto const& state) {
        return state.connection.status==Status::saved && state.connection.configuration.revision==2 &&
            !state.connection.pending;
    });
    Check(saved.connection.configuration.eligibility.DefaultEnabled()==false,
        "Recovered native Apps choice persists through real IPC");
    appearance.Stop(); apps.Stop(); server.Stop();
    bool preserved=false;
    for (auto const& entry:std::filesystem::directory_iterator(options.storage_directory)) {
        if (entry.path().filename().wstring().find(L".preserved.")==std::wstring::npos) continue;
        std::ifstream input(entry.path(),std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(input),{}};
        if (bytes==original) preserved=true;
    }
    Check(preserved && launches==0,"Recovery retains exact corrupt/newer source bytes and neither session restarts Host");
}
}
int RunAppsSessionTests(std::filesystem::path const& root) {
    checks=0; SessionCases(root); RecoveryCases(root,false); RecoveryCases(root,true); return checks;
}
