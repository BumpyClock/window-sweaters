#include "..\..\src\Settings\Session.h"
#include "..\..\src\Configuration\Pipe.h"
#include "..\..\src\Configuration\Storage.h"
#include "StallPipe.h"
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
std::atomic<int> checks=0;
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
        auto found=wake.wait_for(lock,std::chrono::seconds(25),find);
        if (!found && !states.empty()) {
            auto const& last=states.back();
            throw std::runtime_error("Session state timeout: status "+std::to_string(static_cast<unsigned>(last.status))+
                ", revision "+std::to_string(last.configuration.revision)+", width "+
                std::to_string(last.configuration.border_width_dip)+", pending "+std::to_string(last.pending)+
                ", error "+std::to_string(last.error)+", design "+last.configuration.design_id);
        }
        Check(found,"Session publishes expected observable state");
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
    io.exchange=[&](Message const& request,HANDLE) {
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
    io.exchange=[&](Message const& request,HANDLE) {
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
    io.exchange=[&](Message const& request,HANDLE){return Exchange(server.Endpoint(),Self(),request);};
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
    io.exchange=[&](Message const& request,HANDLE) {
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
    io.exchange=[&](Message const& request,HANDLE) {
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
    auto appearance=[&](Configuration value,unsigned index) {
        value.border_width_dip=width(index);
        value.design_selection=index%2?DesignSelection::original:DesignSelection::plain;
        value.design_id=index%2?"spotify":"";
        value.stitch_rows=1.5f+.5f*static_cast<float>(index%22);
        value.stitch_style=static_cast<StitchStyle>(index%3);
        value.material_style=static_cast<MaterialStyle>(index%2);
        value.yarn_argb=0xff000000u+index*0x10101u;
        return value;
    };
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE) {
        auto result=Exchange(server.Endpoint(),Self(),request);
        for (unsigned attempt=0;request.operation==Operation::mutate && result.status==Status::storage_error &&
             (result.error==ERROR_SHARING_VIOLATION || result.error==ERROR_ACCESS_DENIED) && attempt<5;++attempt) {
            Check(result.configuration.revision==request.configuration.revision,
                  "Transient long-chain filesystem denial retains committed revision before exact idempotent retry");
            Sleep(25); result=Exchange(server.Endpoint(),Self(),request);
        }
        if (request.operation==Operation::mutate && result.status==Status::saved) {
            auto count=++saved_count;
            if (count<saves || foreign_commit) {
                ConnectionState original;
                { std::lock_guard lock(display_mutex); original=displayed; }
                active.load()->Change(original,appearance(original.configuration,count+1));
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
    session.Change(initial,appearance(initial.configuration,1));
    if (foreign_commit) {
        auto conflict=observed.Wait([](auto const& state){return state.status==Status::conflict;});
        Check(saved_count==saves && conflict.configuration==foreign && foreign.revision==saves+1 &&
              foreign.paused && server.Snapshot()==foreign,
              "Compacted >64-save own chain still conflicts with an intervening foreign commit");
    } else {
        auto final=observed.Wait([&](auto const& state) {
            return state.status==Status::saved&&!state.pending&&state.configuration.revision==saves;
        });
        auto expected=appearance(initial.configuration,saves); expected.revision=70;
        Check(saved_count==saves && final.configuration==expected && server.Snapshot()==expected,
              "Original displayed semantic root survives >64 contiguous own appearance saves with exact latest color/gauge/style/design/width");
    }
    session.Stop(); server.Stop();
}
void AllAppearanceInputsCoalesce(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-appearance-coalescing");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; Gate gate; std::atomic<unsigned> mutations=0;
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE) {
        if (request.operation==Operation::mutate && ++mutations==1) gate.Block();
        return Exchange(server.Endpoint(),Self(),request);
    };
    io.start_host=[]{return false;};
    Session session(options,[&](auto state){observed.Add(std::move(state));},io);
    auto displayed=observed.Connected();
    auto first=displayed.configuration;
    first.design_selection=DesignSelection::original; first.design_id="spotify";
    first.yarn_argb=0xffaa1122; first.border_width_dip=20;
    session.Change(displayed,first); gate.Wait();
    auto final=first;
    for (unsigned i=0;i<70;++i) {
        final.design_id=i%2?"notes":"finder"; final.border_width_dip=2.f+.5f*static_cast<float>(i);
        final.yarn_argb=0xff000000u+i*0x10101u; final.stitch_rows=1.5f+.5f*static_cast<float>(i%22);
        final.stitch_style=static_cast<StitchStyle>(i%3); final.material_style=static_cast<MaterialStyle>(i%2);
        session.Change(displayed,final);
    }
    gate.Release();
    auto saved=observed.Wait([](auto const& state) {
        return state.status==Status::saved && !state.pending && state.configuration.revision==2;
    });
    final.revision=2;
    Check(mutations==2 && saved.configuration==final && server.Snapshot()==final,
          "70 slider/color/style/design input snapshots coalesce as complete appearance, not partial field mutations");
    session.Stop(); server.Stop();
}
void SemanticDisplayedStateIsNotInvented(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-semantic-root");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; Gate gate; std::atomic<bool> first=false;
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE){return Exchange(server.Endpoint(),Self(),request);};
    io.start_host=[]{return false;};
    Session session(options,[&](ConnectionState state) {
        observed.Add(state);
        if (state.status==Status::saved && !state.pending && state.configuration.revision==1 &&
            !first.exchange(true)) gate.Block();
    },io);
    auto displayed=observed.Connected();
    auto appearance=displayed.configuration;
    appearance.design_selection=DesignSelection::original; appearance.design_id="spotify";
    appearance.yarn_argb=0xff112233; session.Change(displayed,appearance);
    gate.Wait();
    auto invented=displayed; invented.configuration.yarn_argb=0xff112233;
    invented.configuration.stitch_rows=8;
    appearance.yarn_argb=0xff445566;
    session.Change(invented,appearance);
    gate.Release();
    auto conflict=observed.Wait([](auto const& state){return state.status==Status::conflict;});
    Check(conflict.configuration.revision==1 && conflict.configuration.yarn_argb==0xff112233 &&
          server.Snapshot()==conflict.configuration,
          "Same revision with different displayed semantic fields cannot borrow a proven own chain");
    session.Stop(); server.Stop();
}
void QueuedPublicationPreservesNewDraft(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-dispatcher-draft");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations queued; Gate gate;
    std::atomic<unsigned> snapshots=0,mutations=0;
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE) {
        if (request.operation==Operation::snapshot && ++snapshots==3) gate.Block();
        if (request.operation==Operation::mutate) ++mutations;
        return Exchange(server.Endpoint(),Self(),request);
    };
    io.start_host=[]{return false;};
    Session session(options,[&](auto state){queued.Add(std::move(state));},io);
    DraftState ui;
    auto initial=queued.Connected();
    Check(ui.Apply(initial).controls_changed && initial.configuration_authoritative,
          "Production dispatcher reducer accepts an authoritative initial plain snapshot");
    auto old_plain=queued.Wait([&](auto const& state) {
        return state.publication_sequence>initial.publication_sequence && !state.pending;
    });
    gate.Wait();
    auto spotify=ui.Controls();
    spotify.design_selection=DesignSelection::original; spotify.design_id="spotify";
    spotify.yarn_argb=0xff497641; spotify.stitch_style=StitchStyle::garter;
    spotify.material_style=MaterialStyle::plain; spotify.stitch_rows=4.5f;
    auto first=session.Change(ui.Displayed(),spotify); ui.Begin(first,spotify);
    auto deferred=ui.Apply(old_plain);
    Check(deferred.accepted && !deferred.controls_changed && ui.Controls()==spotify && ui.Pending() &&
          ui.Displayed().configuration==initial.configuration,
          "Queued older pending=false plain publication cannot overwrite newer Spotify draft or rebase its displayed origin");
    auto width_input=ui.Controls(); width_input.border_width_dip=24;
    auto latest=session.Change(ui.Displayed(),width_input); ui.Begin(latest,width_input);
    Check(latest>first && ui.LastConfirmed() && ui.LastConfirmed()->design_selection==DesignSelection::plain,
          "New draft generation retains separate confirmed snapshot while coalescing full input state");
    gate.Release();
    auto saved=queued.Wait([&](auto const& state) {
        return state.status==Status::saved && !state.pending && state.settled_edit_generation==latest;
    });
    auto expected=width_input; expected.revision=1;
    Check(ui.Apply(saved).controls_changed && ui.Controls()==expected && ui.LastConfirmed()==expected &&
          server.Snapshot()==expected && mutations==1,
          "Spotify plus width24 preserves color/style/gauge through actual coalesced Session IPC and production dispatcher reduction");
    Check(!ui.Apply(old_plain).accepted && ui.Controls()==expected && ui.LastConfirmed()==expected,
          "Out-of-order queued publication cannot regress connection, controls or confirmed configuration after acknowledgment");
    session.Stop(); server.Stop();
}
void EarlierAcknowledgmentPreservesLaterDraft(std::filesystem::path const& directory) {
    auto options=TestOptions(directory,L"session-dispatcher-ack");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations queued; Gate gate; std::atomic<bool> first=false;
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE){return Exchange(server.Endpoint(),Self(),request);};
    io.start_host=[]{return false;};
    Session session(options,[&](ConnectionState state) {
        queued.Add(state);
        if (state.status==Status::saved && !state.pending && state.configuration.revision==1 &&
            !first.exchange(true)) gate.Block();
    },io);
    DraftState ui; auto initial=queued.Connected(); ui.Apply(initial);
    auto spotify=ui.Controls(); spotify.design_selection=DesignSelection::original;
    spotify.design_id="spotify"; spotify.yarn_argb=0xff497641;
    auto generation=session.Change(ui.Displayed(),spotify); ui.Begin(generation,spotify);
    gate.Wait();
    auto old_ack=queued.Wait([&](auto const& state){return state.settled_edit_generation==generation;});
    auto width=ui.Controls(); width.border_width_dip=24; width.yarn_argb=0xff228833;
    auto latest=session.Change(ui.Displayed(),width); ui.Begin(latest,width);
    auto update=ui.Apply(old_ack);
    Check(update.accepted && !update.controls_changed && ui.Controls()==width &&
          ui.LastConfirmed()==old_ack.configuration && ui.Pending(),
          "Earlier successful queued acknowledgment confirms saved state but never rolls back a newer draft generation");
    gate.Release();
    auto final=queued.Wait([&](auto const& state) {
        return state.status==Status::saved && !state.pending && state.settled_edit_generation==latest;
    });
    width.revision=2;
    Check(ui.Apply(final).controls_changed && ui.Controls()==width && server.Snapshot()==width,
          "Latest draft acknowledgment ends pending state using exact proven own chain");
    session.Stop(); server.Stop();
}
void RenewedSnapshotSurvivesDisconnect(std::filesystem::path const& directory,bool restart) {
    auto options=TestOptions(directory,restart?L"session-confirmed-restart":L"session-confirmed-lease");
    auto server=std::make_unique<ConfigurationServer>(ServerOptions{options.storage_directory,options.endpoint_suffix,Self()});
    server->Start();
    auto spotify=Configuration{}; spotify.design_selection=DesignSelection::original;
    spotify.design_id="spotify"; spotify.yarn_argb=0xff497641;
    Check(server->SubmitLocal(spotify).get().status==Status::saved,"Initial Spotify host appearance is durable");
    auto endpoint=server->Endpoint();
    Observations queued; Gate gate; std::atomic<unsigned> snapshots=0,launches=0;
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE) {
        if (request.operation==Operation::snapshot && ++snapshots==2) gate.Block();
        return Exchange(endpoint,Self(),request);
    };
    io.start_host=[&]{++launches;return false;};
    Session session(options,[&](auto state){queued.Add(std::move(state));},io);
    DraftState ui; auto initial=queued.Connected(); ui.Apply(initial);
    gate.Wait();
    if (restart) {
        server->Stop(); server.reset();
        server=std::make_unique<ConfigurationServer>(ServerOptions{options.storage_directory,options.endpoint_suffix,Self()});
        server->Start();
    } else {
        for (std::size_t i=0;i<maximum_client_leases;++i) {
            Message request; request.client=NewIdentity(); request.request_id=1;
            Exchange(endpoint,Self(),request);
        }
    }
    auto finder=server->Snapshot(); finder.design_id="finder"; finder.yarn_argb=0xff258de0;
    finder.border_width_dip=32; finder.stitch_rows=8;
    auto changed=server->SubmitLocal(finder).get();
    Check(changed.status==Status::saved && changed.configuration.revision==2,
          "Replacement or renewed host has a different complete appearance and revision");
    gate.Release();
    auto expired=queued.Wait([&](auto const& state) {
        return state.status==(restart?Status::wrong_instance:Status::client_expired);
    });
    ui.Apply(expired);
    auto renewed=queued.Wait([&](auto const& state) {
        return state.connected && state.client_lease!=0 &&
            (restart?state.server_instance!=initial.server_instance:state.client_lease!=initial.client_lease);
    });
    Check(renewed.status==Status::saved && renewed.configuration_authoritative &&
          renewed.configuration==changed.configuration && ui.Apply(renewed).controls_changed &&
          ui.LastConfirmed()==changed.configuration,
          "Fresh admitted snapshot clears transient expiry status and independently confirms the latest appearance");
    auto diagnostic=renewed; ++diagnostic.publication_sequence; diagnostic.status=Status::storage_error;
    DraftState diagnostic_ui; diagnostic_ui.Apply(initial); diagnostic_ui.Apply(diagnostic);
    Check(diagnostic_ui.LastConfirmed()==changed.configuration,
          "Authoritative snapshot confirmation does not depend on an unrelated retained mutation-error status");
    server->Stop();
    auto disconnected=queued.Wait([&](auto const& state) {
        return !state.connected && state.status==Status::disconnected &&
            state.publication_sequence>renewed.publication_sequence;
    });
    Check(ui.Apply(disconnected).controls_changed && ui.Controls()==changed.configuration &&
          ui.LastConfirmed()==changed.configuration && launches==0,
          "Disconnect after renewed admission retains the latest confirmed Finder appearance, not obsolete Spotify, without auto-launching host");
    session.Stop();
}
void StalledExchangeStopsPromptly(std::filesystem::path const& directory,bool busy) {
    auto options=TestOptions(directory,busy?L"session-busy-stop":L"session-stall-stop");
    StallPipe peer(busy);
    std::atomic<unsigned> calls=0;
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE cancel) { ++calls; return Exchange(peer.name,Self(),request,cancel); };
    io.start_host=[]{return false;};
    Session session(options,[](ConnectionState){},io);
    for (int i=0;i<500 && !calls;++i) Sleep(10);
    Check(calls==1,"Session reaches the stalled exchange");
    Sleep(100);
    auto began=GetTickCount64();
    session.RequestStop(); session.RequestStop(); session.Stop();
    Check(GetTickCount64()-began<500,"Stop cancels a stalled or busy Session exchange promptly");
    session.Stop();
    Sleep(300);
    Check(calls==1,"No exchange starts after Session stop");
}
void StartupPreferencesPersistThroughSoleWriter(std::filesystem::path const& directory) {
    struct FakeOperation final : startup::PendingOperation {
        unsigned cancellations{};
        void Cancel() noexcept override { ++cancellations; }
    };
    struct FakeStartup final : startup::Backend {
        startup::Command command{};
        Callback completion;
        std::shared_ptr<FakeOperation> operation=std::make_shared<FakeOperation>();
        unsigned submissions{};
        std::shared_ptr<startup::PendingOperation> Begin(startup::Command value,Callback callback) override {
            command=value; completion=std::move(callback); ++submissions; return operation;
        }
        void Complete(startup::Result result) { completion(result); }
    };
    auto options=TestOptions(directory,L"startup-preference");
    imports::StoredCatalogue empty; empty.revision_high_water=41;
    auto restored=imports::RestoreCatalogue(empty);
    auto catalogue=std::make_shared<const imports::ImportCatalogue>(
        std::get<imports::ImportCatalogue>(std::move(restored)));
    Configuration initial; initial.border_width_dip=23.f; initial.yarn_argb=0xff234567;
    auto enabled=*applications::AppIdentity::FromExecutablePath(L"C:\\Fixture\\StartupAllowed.exe");
    applications::EligibilityMutation eligibility; eligibility.defaultEnabled=false;
    eligibility.edits.emplace_back(applications::SetApplicationPreference{enabled,true});
    initial.eligibility=std::get<applications::EligibilityPreferences>(initial.eligibility.Apply(eligibility));
    configuration::DescribeCatalogue(initial.imports,*catalogue);
    Check(Storage(options.storage_directory).Save(initial,catalogue)==0,"Startup fixture persists nondefault appearance and import revision high water");
    ConfigurationServer server({options.storage_directory,options.endpoint_suffix,Self()}); server.Start();
    Observations observed; std::atomic<unsigned> launches{};
    SessionTransport io;
    io.exchange=[&](Message const& request,HANDLE cancel) { return Exchange(server.Endpoint(),Self(),request,cancel); };
    io.start_host=[&] { ++launches; return false; };
    Session session(options,[&](ConnectionState state) { observed.Add(std::move(state)); },io);
    auto backend=std::make_shared<FakeStartup>();
    StartupPreferences preferences(session,backend);
    auto displayed=observed.Connected();
    preferences.Observe(displayed);
    Check(!backend->submissions && preferences.Snapshot().savedDesired==startup::DesiredPreference::Off,
        "Settings constructor and authoritative handshake do not request OS enable");
    preferences.Read(); backend->Complete({startup::Outcome::Observed,startup::OsState::Disabled});
    Check(preferences.Poll()->save_generation==0 && server.Snapshot()==initial,
        "Read-only Windows observation does not persist or enable");
    Check(preferences.Request(displayed,startup::DesiredPreference::On,startup::UserIntent::None).status==
        startup::SubmissionStatus::ExplicitIntentRequired,"Actual Settings startup consumer requires explicit gesture");
    preferences.Request(displayed,startup::DesiredPreference::On,startup::UserIntent::SettingsGesture);
    Check(backend->command==startup::Command::Enable,"Explicit Settings gesture requests fake OS enable only");
    backend->Complete({startup::Outcome::Applied,startup::OsState::Enabled});
    auto update=preferences.Poll();
    Check(update && update->save_generation,"Applied OS enable queues exact-revision typed host persistence");
    auto saved=observed.Wait([&](auto const& state) { return !state.pending && state.settled_edit_generation>=update->save_generation; });
    auto expected=initial; expected.revision=1; expected.startup_desired=startup::DesiredPreference::On;
    Check(saved.status==Status::saved && saved.configuration==expected && server.Snapshot()==expected &&
        Storage(options.storage_directory).Load().configuration==expected,
        "Real pipe startup acknowledgement and disk preserve appearance, eligibility, imports and high water");
    preferences.Observe(saved);
    auto submissions=backend->submissions;
    preferences.Observe(saved);
    Check(preferences.Snapshot().savedDesired==startup::DesiredPreference::On && backend->submissions==submissions,
        "Saved preference observation does not mutate OS state");
    for (auto result:{startup::Result{startup::Outcome::BlockedByUser,startup::OsState::DisabledByUser},
        startup::Result{startup::Outcome::BlockedByPolicy,startup::OsState::DisabledByPolicy},
        startup::Result{startup::Outcome::BlockedByPolicy,startup::OsState::EnabledByPolicy},
        startup::Result{startup::Outcome::UnavailableNoPackage,startup::OsState::UnavailableNoPackage}}) {
        preferences.Request(saved,startup::DesiredPreference::Off,startup::UserIntent::SettingsGesture);
        backend->Complete(result); auto blocked=preferences.Poll();
        Check(blocked && !blocked->save_generation && blocked->completion.result.state==result.state &&
            server.Snapshot()==expected,"Actual Settings preserves user/policy/unpackaged OS states without false-success persistence");
    }
    configuration::Handle locked(CreateFileW(Storage(options.storage_directory).File().c_str(),GENERIC_READ,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    Check(bool(locked),"Hold only isolated configuration file against replacement");
    preferences.Request(saved,startup::DesiredPreference::Off,startup::UserIntent::SettingsGesture);
    backend->Complete({startup::Outcome::Applied,startup::OsState::Disabled});
    update=preferences.Poll();
    auto failed=observed.Wait([&](auto const& state) { return !state.pending && state.settled_edit_generation>=update->save_generation; });
    preferences.Observe(failed);
    Check(failed.status==Status::storage_error && server.Snapshot()==expected &&
        preferences.Snapshot().savedDesired==startup::DesiredPreference::On &&
        preferences.Snapshot().lastObservation->result.state==startup::OsState::Disabled,
        "OS change followed by real Save failure remains truthful: desired on, OS off, no automatic rollback");
    auto count=backend->submissions;
    preferences.Request(failed,startup::DesiredPreference::Off,startup::UserIntent::SettingsGesture);
    preferences.Stop();
    backend->Complete({startup::Outcome::Applied,startup::OsState::Disabled});
    Check(!preferences.Poll() && preferences.Snapshot().stopped && backend->operation->cancellations &&
        backend->submissions==count+1 && server.Snapshot()==expected,
        "Closing actual startup consumer cancels one pending operation and drops late completion without persistence");
    session.Stop(); server.Stop();
    Check(!launches,"Startup persistence test never starts OS startup or companion UI");
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
    AllAppearanceInputsCoalesce(directory);
    SemanticDisplayedStateIsNotInvented(directory);
    QueuedPublicationPreservesNewDraft(directory);
    EarlierAcknowledgmentPreservesLaterDraft(directory);
    RenewedSnapshotSurvivesDisconnect(directory,false);
    RenewedSnapshotSurvivesDisconnect(directory,true);
    StalledExchangeStopsPromptly(directory,false);
    StalledExchangeStopsPromptly(directory,true);
    StartupPreferencesPersistThroughSoleWriter(directory);
    return checks;
}
