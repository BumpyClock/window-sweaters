#include "..\..\src\Settings\Session.h"
#include "..\..\src\Configuration\Pipe.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace window_sweaters;
using namespace window_sweaters::contracts;
using namespace window_sweaters::configuration;
using namespace window_sweaters::settings;
namespace {
int checks=0;
void Check(bool value,char const* message) { ++checks; if (!value) throw std::runtime_error(message); }
std::filesystem::path Self() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768); return path;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered=false,released=false;
    void Block() {
        std::unique_lock lock(mutex); entered=true; wake.notify_all();
        if (!wake.wait_for(lock,std::chrono::seconds(10),[&]{return released;}))
            throw std::runtime_error("Session test exchange gate timed out");
    }
    void Wait() {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock,std::chrono::seconds(10),[&]{return entered;}),"Session reaches gated real exchange");
    }
    void Release() { std::lock_guard lock(mutex); released=true; wake.notify_all(); }
    ~Gate() { Release(); }
};
struct Observations {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<ConnectionState> states;
    void Add(ConnectionState state) {
        std::lock_guard lock(mutex); states.push_back(std::move(state)); wake.notify_all();
    }
    template<class Predicate> ConnectionState Wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        auto find=[&]() {
            for (auto i=states.rbegin();i!=states.rend();++i) if (predicate(*i)) return true;
            return false;
        };
        Check(wake.wait_for(lock,std::chrono::seconds(25),find),"Session publishes expected observable state");
        for (auto i=states.rbegin();i!=states.rend();++i) if (predicate(*i)) return *i;
        throw std::runtime_error("Expected session state unavailable");
    }
    ConnectionState Connected() {
        return Wait([](auto const& state){return state.connected&&!state.pending&&state.status==Status::saved;});
    }
};
Options TestOptions(std::filesystem::path const& directory,wchar_t const* name) {
    return {true,directory/name,std::wstring(name)+L"-"+std::to_wstring(GetCurrentProcessId())};
}
void ConcurrentTrayIsNotRebased(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-conflict");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; Gate gate;
    std::atomic<unsigned> snapshots=0,launches=0;
    SessionTransport io;
    io.exchange=[&](Message const& request) {
        if (request.operation==Operation::snapshot && ++snapshots==2) gate.Block();
        return Exchange(server.Endpoint(),Self(),request);
    };
    io.start_host=[&]{++launches;return false;};
    Session session(options,[&](auto state){observed.Add(std::move(state));},io);
    auto displayed=observed.Connected();
    Check(displayed.configuration.revision==0,"Session displays host default revision");
    session.Change(displayed,false,23);
    gate.Wait();
    auto tray=server.SubmitLocal(true,16,0).get();
    Check(tray.status==Status::saved && tray.configuration.revision==1,"Concurrent tray commit is successful");
    gate.Release();
    auto conflict=observed.Wait([](auto const& state){return state.status==Status::conflict;});
    Check(conflict.configuration.revision==1 && conflict.configuration.paused &&
          conflict.configuration.border_width_dip==16,"Session reports conflict instead of rebasing old displayed values");
    Check(server.Snapshot()==tray.configuration,"Queued width cannot silently undo concurrent tray pause");
    session.Stop(); server.Stop();
    Check(launches==0,"Connected settings session does not launch companion host");
}
void OwnPendingSliderIsChained(std::filesystem::path const& directory,bool after_reply) {
    auto options=TestOptions(directory,after_reply?L"session-after-reply":L"session-in-flight");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; Gate gate;
    std::atomic<unsigned> mutations=0;
    std::atomic<bool> queue_after_reply=false;
    SessionTransport io;
    io.exchange=[&](Message const& request) {
        if (request.operation==Operation::mutate && ++mutations==1 && !after_reply) gate.Block();
        return Exchange(server.Endpoint(),Self(),request);
    };
    io.start_host=[]{return false;};
    Session session(options,[&](ConnectionState state) {
        observed.Add(state);
        if (after_reply && state.status==Status::saved && !state.pending &&
            state.configuration.revision==1 && !queue_after_reply.exchange(true)) gate.Block();
    },io);
    auto displayed=observed.Connected();
    session.Change(displayed,false,20);
    gate.Wait();
    session.Change(displayed,false,24);
    gate.Release();
    auto final=observed.Wait([](auto const& state) {
        return state.status==Status::saved && !state.pending && state.configuration.border_width_dip==24;
    });
    Check(final.configuration.revision==2 && server.Snapshot().border_width_dip==24,
          "Latest slider value chains only own acknowledged predecessor even before UI applies reply");
    Check(mutations==2,"Coalesced latest width needs exactly two successful own commits");
    session.Stop(); server.Stop();
}
void OwnCommitDoesNotHideForeignCommit(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-own-foreign");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; Gate gate;
    std::atomic<bool> first_reply=false;
    SessionTransport io;
    io.exchange=[&](Message const& request){return Exchange(server.Endpoint(),Self(),request);};
    io.start_host=[]{return false;};
    Session session(options,[&](ConnectionState state) {
        observed.Add(state);
        if (state.status==Status::saved && !state.pending && state.configuration.revision==1 &&
            !first_reply.exchange(true)) gate.Block();
    },io);
    auto displayed=observed.Connected();
    session.Change(displayed,false,20);
    gate.Wait();
    session.Change(displayed,false,24);
    auto tray=server.SubmitLocal(true,20,1).get();
    Check(tray.status==Status::saved,"Tray writes after settings own predecessor");
    gate.Release();
    auto conflict=observed.Wait([](auto const& state){return state.status==Status::conflict;});
    Check(conflict.configuration==tray.configuration && server.Snapshot()==tray.configuration,
          "Own predecessor permits only own revision transition, not unrelated tray revision");
    session.Stop(); server.Stop();
}
void RetiredSessionDiscardsRequests(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-retired");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; Gate gate;
    std::atomic<unsigned> snapshots=0,mutations=0;
    SessionTransport io;
    io.exchange=[&](Message const& request) {
        if (request.operation==Operation::snapshot && ++snapshots==2) gate.Block();
        if (request.operation==Operation::mutate) ++mutations;
        return Exchange(server.Endpoint(),Self(),request);
    };
    io.start_host=[]{return false;};
    Session session(options,[&](auto state){observed.Add(std::move(state));},io);
    auto displayed=observed.Connected();
    session.Change(displayed,false,27);
    gate.Wait();
    for (std::size_t i=0;i<maximum_client_leases;++i) {
        Message request; request.client=NewIdentity(); request.request_id=1;
        Exchange(server.Endpoint(),Self(),request);
    }
    gate.Release();
    observed.Wait([](auto const& state){return state.status==Status::client_expired;});
    auto renewed=observed.Wait([&](auto const& state) {
        return state.connected && state.client_lease!=0 && state.client_lease!=displayed.client_lease;
    });
    Check(mutations==0 && renewed.configuration.revision==0,"Expired admission discards unsent old change before explicit renewal");
    session.Change(renewed,false,28);
    auto saved=observed.Wait([](auto const& state){return state.status==Status::saved && state.configuration.border_width_dip==28;});
    Check(saved.configuration.revision==1 && mutations==1,"Renewed session accepts a new user change but never replays old work");
    session.Stop(); server.Stop();
}
void HostRestartDiscardsOldWork(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-host-restart");
    auto server=std::make_unique<ConfigurationServer>(ServerOptions{options.storage_directory,options.endpoint_suffix,Self()});
    server->Start();
    auto endpoint=server->Endpoint();
    Observations observed; Gate gate;
    std::atomic<unsigned> mutations=0,launches=0;
    SessionTransport io;
    io.exchange=[&](Message const& request) {
        auto response=Exchange(endpoint,Self(),request);
        if (request.operation==Operation::mutate && ++mutations==1) {
            gate.Block();
            throw std::runtime_error("Injected lost response after real committed save");
        }
        return response;
    };
    io.start_host=[&]{++launches;return false;};
    Session session(options,[&](auto state){observed.Add(std::move(state));},io);
    auto displayed=observed.Connected();
    session.Change(displayed,false,20);
    gate.Wait();
    session.Change(displayed,false,27);
    Check(server->Snapshot().revision==1,"Host commits before simulated response loss");
    server->Stop(); server.reset();
    auto replacement=std::make_unique<ConfigurationServer>(ServerOptions{options.storage_directory,options.endpoint_suffix,Self()});
    replacement->Start();
    gate.Release();
    observed.Wait([](auto const& state){return state.status==Status::wrong_instance;});
    auto reconnected=observed.Wait([&](auto const& state) {
        return state.connected && state.server_instance!=displayed.server_instance && state.client_lease!=0;
    });
    Check(reconnected.configuration.revision==1 && reconnected.configuration.border_width_dip==20 &&
          mutations==1 && launches==0,"Reconnection discards old-instance retry and queued work without restarting host");
    session.Change(reconnected,false,30);
    auto saved=observed.Wait([](auto const& state){return state.status==Status::saved && state.configuration.border_width_dip==30;});
    Check(saved.configuration.revision==2 && mutations==2,"New user edit uses new host identity and admission");
    session.Stop(); replacement->Stop();
}
void LongOwnChainRetainsDisplayedRoot(std::filesystem::path const& directory,bool foreign_commit) {
    auto options=TestOptions(directory,foreign_commit?L"session-long-foreign":L"session-long-own");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed;
    std::atomic<Session*> active=nullptr;
    std::atomic<unsigned> saved_count=0;
    std::mutex display_mutex;
    ConnectionState displayed;
    Configuration foreign{};
    constexpr unsigned saves=70;
    auto width=[](unsigned index){return 2.0f+0.5f*static_cast<float>(index);};
    SessionTransport io;
    io.exchange=[&](Message const& request) {
        auto result=Exchange(server.Endpoint(),Self(),request);
        if (request.operation==Operation::mutate && result.status==Status::saved) {
            auto count=++saved_count;
            if (count<saves || foreign_commit) {
                ConnectionState original;
                { std::lock_guard lock(display_mutex); original=displayed; }
                active.load()->Change(original,false,width(count+1));
            }
            if (foreign_commit && count==saves) {
                auto tray=server.SubmitLocal(true,width(count),count).get();
                if (tray.status!=Status::saved) throw std::runtime_error("Long-chain foreign tray save failed");
                foreign=tray.configuration;
            }
        }
        return result;
    };
    io.start_host=[]{return false;};
    Session session(options,[&](auto state){observed.Add(std::move(state));},io);
    active=&session;
    auto initial=observed.Connected();
    { std::lock_guard lock(display_mutex); displayed=initial; }
    session.Change(initial,false,width(1));
    if (foreign_commit) {
        auto conflict=observed.Wait([](auto const& state){return state.status==Status::conflict;});
        Check(saved_count==saves && conflict.configuration==foreign && foreign.revision==saves+1 &&
              foreign.paused && server.Snapshot()==foreign,
              "Compacted >64-save own chain still conflicts with an intervening foreign commit");
    } else {
        auto final=observed.Wait([&](auto const& state) {
            return state.status==Status::saved&&!state.pending&&state.configuration.revision==saves;
        });
        Check(saved_count==saves && final.configuration.border_width_dip==37 &&
              server.Snapshot().revision==70 && server.Snapshot().border_width_dip==37,
              "Original displayed root survives >64 contiguous coalesced own saves without losing latest slider value");
    }
    session.Stop(); server.Stop();
}
}
int RunSessionTests(std::filesystem::path const& directory) {
    checks=0;
    ConcurrentTrayIsNotRebased(directory);
    OwnPendingSliderIsChained(directory,false);
    OwnPendingSliderIsChained(directory,true);
    OwnCommitDoesNotHideForeignCommit(directory);
    RetiredSessionDiscardsRequests(directory);
    HostRestartDiscardsOldWork(directory);
    LongOwnChainRetainsDisplayedRoot(directory,false);
    LongOwnChainRetainsDisplayedRoot(directory,true);
    return checks;
}
