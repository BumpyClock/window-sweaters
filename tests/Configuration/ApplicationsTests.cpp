#include "..\..\src\Configuration\ConfigurationServer.h"
#include "..\..\src\Configuration\Storage.h"
#include "..\..\src\Configuration\Pipe.h"
#include "..\..\src\Settings\Session.h"
#include "..\..\src\Settings\AppsPresentation.h"
#include <atomic>
#include <algorithm>
#include <bit>
#include <condition_variable>
#include <fstream>
#include <set>
#include <mutex>

using namespace window_sweaters;
using namespace window_sweaters::contracts;
using namespace window_sweaters::configuration;
namespace {
int checks{};
void Check(bool value,char const* label) { ++checks; if (!value) throw std::runtime_error(label); }
std::filesystem::path Self() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768); return path;
}
std::vector<char> Bytes(std::filesystem::path const& path) {
    std::ifstream input(path,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};
}
void Put(std::vector<char>& bytes,std::size_t at,std::uint64_t value,std::size_t count=4) {
    for (std::size_t i=0;i<count;++i) bytes[at+i]=static_cast<char>((value>>(8*i))&255);
}
void Write(std::filesystem::path const& path,std::vector<char> bytes) {
    std::uint32_t crc=0xffffffff;
    for (std::size_t i=0;i<bytes.size()-4;++i) {
        crc^=static_cast<unsigned char>(bytes[i]);
        for (int bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    Put(bytes,bytes.size()-4,static_cast<std::uint32_t>(~crc));
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
}
applications::AppIdentity Path(wchar_t const* text) {
    auto identity=applications::AppIdentity::FromExecutablePath(text);
    if (!identity) throw std::runtime_error("Invalid test identity");
    return *identity;
}
applications::RunningApplicationWindow Window(std::uintptr_t hwnd,std::uint32_t pid,std::uint64_t birth,
    std::uint64_t generation,wchar_t const* path,std::optional<std::wstring> aumid={}) {
    auto token=applications::TargetToken::FromTarget({hwnd,pid,birth,generation},0);
    if (!token) throw std::runtime_error("Invalid test lifetime");
    auto process=applications::ResolveMetadata(applications::NoPackagedIdentity{},applications::ExecutablePath{path});
    applications::PackagedIdentityQuery window_id=aumid?applications::PackagedIdentityQuery{applications::PackagedAumid{*aumid}}:
        applications::PackagedIdentityQuery{applications::NoPackagedIdentity{}};
    auto metadata=applications::ResolveWindowOwnerMetadata(window_id,process);
    return {*token,std::make_shared<applications::MetadataResult const>(std::move(metadata))};
}
Message Read(ConfigurationServer& server,Message request) {
    request.operation=Operation::applications_snapshot; request.status=Status::saved;
    request.error=0; ++request.request_id;
    return Exchange(server.Endpoint(),Self(),request);
}
Message Edit(ConfigurationServer& server,Message base,applications::EligibilityMutation mutation) {
    base.operation=Operation::eligibility; base.status=Status::saved; base.error=0;
    ++base.request_id; base.eligibility=std::move(mutation);
    return Exchange(server.Endpoint(),Self(),base);
}
void MigrationAndBounds(std::filesystem::path const& root) {
    Storage storage(root/L"apps-migration"); std::filesystem::create_directories(storage.Directory());
    std::vector<char> old(120);
    Put(old,0,0x31435357); Put(old,4,2); Put(old,8,72,8); Put(old,16,1);
    Put(old,20,std::bit_cast<std::uint32_t>(25.5f)); Put(old,24,1); Put(old,28,2); Put(old,32,0);
    Put(old,36,std::bit_cast<std::uint32_t>(8.5f)); Put(old,40,0xff228833); Put(old,44,7);
    std::copy_n("spotify",7,old.begin()+48); Write(storage.File(),old);
    auto original=Bytes(storage.File()); auto loaded=storage.Load();
    Check(loaded.state==StorageState::ready && loaded.configuration.schema==configuration_schema &&
        loaded.configuration.revision==72 && loaded.configuration.paused && loaded.configuration.border_width_dip==25.5f &&
        loaded.configuration.design_id=="spotify" && loaded.configuration.stitch_style==StitchStyle::garter &&
        loaded.configuration.material_style==MaterialStyle::plain && loaded.configuration.stitch_rows==8.5f &&
        loaded.configuration.yarn_argb==0xff228833,"Schema2 migration preserves every appearance value");
    Check(loaded.configuration.eligibility.DefaultEnabled() && loaded.configuration.eligibility.Exceptions().empty() &&
        Bytes(storage.File())==original,"Schema2 migration is read-only default-on with empty exceptions");
    ConfigurationServer server({storage.Directory(),L"apps-migration-"+std::to_wstring(GetCurrentProcessId()),Self()});
    server.Start();
    {
        Handle locked(CreateFileW(storage.File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        auto failed=server.SubmitEligibility({false,true,{}},72).get();
        Check(failed.status==Status::storage_error && server.Snapshot()==loaded.configuration &&
            Bytes(storage.File())==original,"Failed schema2 eligibility migration preserves appearance and preferences");
    }
    auto saved=server.SubmitEligibility({false,true,{}},72).get();
    Check(saved.status==Status::saved && saved.configuration.revision==73 &&
        !storage.Load().configuration.eligibility.DefaultEnabled(),"Acknowledged eligibility edit atomically writes schema3");
    server.Stop();
    Message message; message.configuration=saved.configuration;
    Message decoded; auto encoded=Encode(message);
    Check(Decode(encoded,decoded) && decoded.configuration==saved.configuration,"Bounded preference payload round-trip");
    for (std::size_t count=0;count<encoded.size();++count)
        Check(!Decode(std::span(encoded).first(count),decoded),"Every truncated frame is rejected");
    auto malformed=encoded; malformed.push_back(std::byte{});
    Check(!Decode(malformed,decoded),"Trailing frame bytes are rejected");
    malformed=encoded; malformed[176]=std::byte{99};
    Check(!Decode(malformed,decoded),"Unknown tagged extension cannot invent unsupported feature");
    malformed=encoded; malformed[180]=std::byte{255}; malformed[181]=std::byte{255};
    Check(!Decode(malformed,decoded),"Oversized section rejected before allocation");
    malformed=encoded; malformed[184]=std::byte{2};
    Check(!Decode(malformed,decoded),"Non-boolean persisted preference rejected");
    message.operation=static_cast<Operation>(8);
    Check(!Decode(Encode(message),decoded),"Unsupported operation is rejected");
    applications::EligibilityMutation duplicate;
    auto identity=Path(L"C:\\Apps\\player.exe");
    duplicate.edits={applications::SetApplicationPreference{identity,true},
        applications::RemoveApplicationPreference{identity}};
    message.operation=Operation::eligibility; message.eligibility=duplicate;
    Check(!Decode(Encode(message),decoded),"Duplicate identity edits rejected at typed boundary");
    applications::EligibilityMutation many;
    for (unsigned i=0;i<40;++i) {
        auto key=L"C:\\"+std::wstring(8000,L'x')+std::to_wstring(i)+L".exe";
        many.edits.emplace_back(applications::SetApplicationPreference{
            *applications::AppIdentity::FromExecutablePath(key),true});
    }
    auto excessive=std::get<applications::EligibilityPreferences>(applications::EligibilityPreferences{}.Apply(many));
    Check(!BoundedPreferences(excessive),"Aggregate identity bytes bounded independently from count");
    auto invalid=saved.configuration; invalid.eligibility=excessive;
    Check(!Valid(invalid) && storage.Save(invalid)!=0,"Aggregate identity budget enforced at durable boundary");
}
void LiveIpc(std::filesystem::path const& root) {
    ConfigurationServer server({root/L"apps-ipc",L"apps-ipc-"+std::to_wstring(GetCurrentProcessId()),Self()});
    server.Start();
    Message request; request.client=NewIdentity();
    auto snapshot=Read(server,request);
    auto all_off=Edit(server,snapshot,{false,true,{}});
    Check(all_off.status==Status::saved && all_off.response_to==Operation::eligibility &&
        all_off.configuration.revision==1 && !all_off.configuration.eligibility.DefaultEnabled(),"All-off uses typed durable acknowledgement");
    auto a=Window(11,10,100,1,L"C:\\One\\player.exe");
    auto a2=Window(12,10,100,2,L"C:\\One\\player.exe");
    auto b=Window(13,11,101,3,L"C:\\Two\\player.exe");
    server.PublishApplications({a,a2,b});
    auto running=Read(server,all_off);
    Check(running.configuration.revision==1 && running.applications.applications.size()==2 &&
        !running.applications.applications[0].enabled && !running.applications.applications[1].enabled &&
        running.applications.applications[0].window_count==2,"Disabled running owners and multiple windows stay in snapshots");
    auto generation=running.applications.observation_generation;
    server.PublishApplications({a,b});
    auto changed=Read(server,running);
    Check(changed.configuration.revision==1 && changed.applications.observation_generation>generation &&
        changed.applications.applications[0].window_count==1,"Observation generation advances when counts change without configuration mutation");
    auto replacement=Window(11,20,300,4,L"C:\\One\\player.exe");
    server.PublishApplications({replacement,b});
    auto same_counts=Read(server,changed);
    Check(same_counts.configuration.revision==1 &&
        same_counts.applications.applications[0].window_count==changed.applications.applications[0].window_count &&
        same_counts.applications.observation_generation>changed.applications.observation_generation,
        "Owner lifetime changes advance observation generation even when identity and aggregate counts are unchanged");
    auto builds=server.ApplicationBuilds();
    auto repeated=Read(server,same_counts); auto again=Read(server,repeated);
    Check(server.ApplicationBuilds()==builds && repeated.applications==same_counts.applications &&
        again.applications==same_counts.applications && again.request_id==same_counts.request_id+2 &&
        again.client_lease==same_counts.client_lease && again.server==same_counts.server && again.status==Status::saved,
        "Repeated reads reuse the built snapshot while per-response request, lease and server fields stay fresh");
    server.PublishApplications({Window(11,21,301,5,L"C:\\One\\player.exe"),b});
    auto relifed=Read(server,again);
    Check(server.ApplicationBuilds()==builds+1 && relifed.applications.applications==same_counts.applications.applications &&
        relifed.applications.observation_generation>same_counts.applications.observation_generation,
        "Same-count lifetime replacement invalidates the reused snapshot and advances observation generation");
    changed=relifed;
    builds=server.ApplicationBuilds();
    applications::EligibilityMutation toggle;
    toggle.edits.emplace_back(applications::SetApplicationPreference{Path(L"C:\\One\\player.exe"),true});
    auto command=changed; command.operation=Operation::eligibility; command.status=Status::saved;
    ++command.request_id; command.eligibility=toggle;
    auto enabled=Exchange(server.Endpoint(),Self(),command);
    Check(enabled.status==Status::saved && Exchange(server.Endpoint(),Self(),command)==enabled,
        "Eligibility retry retains exact durable result");
    auto different_spelling=command;
    different_spelling.eligibility.edits={applications::SetApplicationPreference{Path(L"C:\\one\\player.exe"),true}};
    Check(Exchange(server.Endpoint(),Self(),different_spelling).status==Status::invalid,
        "Duplicate request ID rejects changed wire spelling even for ordinal-equal identity");
    command.eligibility.clearExceptions=true;
    Check(Exchange(server.Endpoint(),Self(),command).status==Status::invalid,"Reused eligibility request ID cannot change semantics");
    auto listed=Read(server,enabled);
    Check(server.ApplicationBuilds()==builds+1 && listed.applications.configuration_revision==enabled.configuration.revision &&
        Read(server,listed).applications==listed.applications && server.ApplicationBuilds()==builds+1,
        "Configuration mutation invalidates the reused snapshot exactly once");
    Check(listed.applications.applications[0].enabled && listed.applications.applications[0].explicit_preference &&
        !listed.applications.applications[1].enabled,"Same basename different paths never share a saved toggle");
    auto stale_base=changed; stale_base.request_id=listed.request_id;
    auto stale=Edit(server,stale_base,{true,true,{}});
    Check(stale.status==Status::conflict && server.Snapshot()==enabled.configuration,"Old displayed apps revision is never blindly rebased");
    Storage storage(root/L"apps-ipc"); auto before=Bytes(storage.File());
    {
        Handle locked(CreateFileW(storage.File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        auto failed=Edit(server,listed,{true,true,{}});
        Check(failed.status==Status::storage_error && failed.configuration==enabled.configuration &&
            Bytes(storage.File())==before,"Real IPC failed eligibility write retains complete preferences and appearance");
    }
    auto appearance=enabled.configuration; appearance.revision=enabled.configuration.revision;
    appearance.border_width_dip=27; appearance.eligibility={};
    auto appearance_saved=server.SubmitLocal(appearance).get();
    Check(appearance_saved.status==Status::saved && appearance_saved.configuration.border_width_dip==27 &&
        appearance_saved.configuration.eligibility==enabled.configuration.eligibility,"Appearance-only mutation cannot overwrite eligibility");
    server.PublishApplications({b});
    auto nonrunning=Read(server,listed);
    Check(nonrunning.applications.applications.size()==2 &&
        nonrunning.applications.applications[1].presence==applications::ApplicationPresence::SavedNotRunning &&
        nonrunning.applications.applications[1].enabled &&
        nonrunning.applications.applications[1].display_name.empty() &&
        !nonrunning.applications.applications[1].provenance,"Saved non-running exception remains visible without fabricated metadata");
    auto relaunched=Window(11,15,200,4,L"C:\\One\\player.exe");
    auto future=Window(14,16,201,5,L"C:\\New\\future.exe");
    auto hosted_a=Window(15,17,202,6,L"C:\\Windows\\System32\\ApplicationFrameHost.exe",L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    auto hosted_b=Window(16,17,202,7,L"C:\\Windows\\System32\\ApplicationFrameHost.exe",L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App");
    auto unknown=Window(17,18,203,8,L"C:\\Windows\\System32\\ApplicationFrameHost.exe");
    server.PublishApplications({relaunched,future,hosted_a,hosted_b,unknown});
    auto owners=Read(server,nonrunning);
    Check(owners.applications.applications.size()==4 && owners.applications.unavailable.size()==1,
        "Hosted AUMIDs remain separate and unverified frame host is non-mutable diagnostic");
    auto path=Path(L"C:\\One\\player.exe");
    for (auto const& row:owners.applications.applications)
        Check(row.enabled==(row.identity==path),"Relaunch and HWND/PID reuse retain only stable identity choice; future apps remain off");
    auto default_changed=Edit(server,owners,{true,false,{}});
    Check(default_changed.status==Status::saved && default_changed.configuration.eligibility.Exceptions().size()==1,
        "Default-only edit preserves explicit choices atomically");
    auto cleared=Edit(server,Read(server,default_changed),{false,true,{}});
    Check(cleared.status==Status::saved && cleared.configuration.eligibility.Exceptions().empty(),
        "All-off clears explicit exceptions in same transaction");
    auto local=server.SubmitEligibility(toggle,cleared.configuration.revision).get();
    auto base=Read(server,cleared);
    applications::EligibilityMutation remove; remove.edits.emplace_back(applications::RemoveApplicationPreference{path});
    auto removed=Edit(server,base,remove);
    Check(local.status==Status::saved && removed.status==Status::saved && removed.configuration.eligibility.Exceptions().empty(),
        "Remove saved choice restores current default");
    auto prior_server=removed.server; server.Stop();
    ConfigurationServer restarted({root/L"apps-ipc",L"apps-ipc-"+std::to_wstring(GetCurrentProcessId()),Self()});
    restarted.Start();
    Message fresh; fresh.client=NewIdentity(); auto after=Read(restarted,fresh);
    Check(!after.configuration.eligibility.DefaultEnabled() && after.configuration.eligibility.Exceptions().empty() &&
        after.configuration.border_width_dip==27,"Cold restart loads all-off and preserves appearance");
    auto wrong=after; wrong.server=prior_server;
    Check(Edit(restarted,wrong,toggle).status==Status::wrong_instance,"Old-instance eligibility command cannot cross reconnect");
    restarted.Stop();
}
void ProductionDraft() {
    auto identity=Path(L"C:\\Apps\\player.exe");
    settings::AppsConnectionState initial;
    initial.connection.connected=true; initial.connection.configuration_authoritative=true;
    initial.connection.publication_sequence=1; initial.connection.client_lease=1;
    initial.connection.server_instance=NewIdentity();
    initial.snapshot.applications.push_back({identity,L"player.exe",applications::ApplicationPresence::Running,true,false,1,1,
        applications::MetadataProvenance::ProcessImageName});
    settings::AppsDraftState draft;
    Check(draft.Apply(initial),"Production apps reducer accepts first authoritative host list");
    applications::EligibilityMutation mutation; mutation.edits.emplace_back(applications::SetApplicationPreference{identity,false});
    draft.Begin(1,mutation);
    Check(draft.CanInteract() && !draft.CanEdit(),
        "Pending native app edit retains focusable controls while refusing another mutation");
    auto old=initial; old.connection.publication_sequence=2; old.snapshot.observation_generation=2;
    Check(!draft.Apply(old) && !draft.Controls().applications[0].enabled && draft.Pending(),
        "New app toggle draft survives later-published old snapshot without settlement");
    auto pending=old; pending.connection.publication_sequence=3; pending.connection.pending=true;
    Check(!draft.Apply(pending) && draft.Displayed().connection.configuration.revision==0,
        "In-flight apps state does not move displayed CAS base");
    auto terminal=initial; terminal.connection.publication_sequence=4; terminal.connection.settled_edit_generation=1;
    terminal.connection.configuration.revision=1; terminal.snapshot.configuration_revision=1;
    terminal.snapshot.applications[0].enabled=false;
    Check(draft.Apply(terminal) && !draft.Pending() && !draft.Controls().applications[0].enabled &&
        draft.Displayed().connection.configuration.revision==1,"Typed terminal apps state settles draft and advances displayed CAS base");
    Check(!draft.Apply(old) && draft.Current().connection.publication_sequence==4,"Out-of-order apps publication cannot overwrite current draft");
}
void PresentationContracts() {
    Check(!settings::ApplicationPresentationReady(false,false,0,0) &&
        !settings::ApplicationPresentationReady(true,false,660,920) &&
        !settings::ApplicationPresentationReady(false,true,660,920) &&
        !settings::ApplicationPresentationReady(true,true,0,920),
        "First native Apps snapshot cannot touch focus or viewport before Loaded, attached XamlRoot and measured layout");
    Check(settings::ApplicationPresentationReady(true,true,660,920),
        "Attached measured native layout admits retained Apps presentation");
    settings::AppsConnectionState beforeLoaded;
    beforeLoaded.connection.publication_sequence=1; beforeLoaded.connection.configuration.revision=1;
    std::optional<settings::AppsConnectionState> pendingPublication;
    settings::RetainApplicationPublication(pendingPublication,beforeLoaded);
    auto newer=beforeLoaded; newer.connection.publication_sequence=3; newer.connection.configuration.revision=2;
    settings::RetainApplicationPublication(pendingPublication,newer);
    auto late=beforeLoaded; late.connection.publication_sequence=2;
    settings::RetainApplicationPublication(pendingPublication,late);
    Check(pendingPublication && pendingPublication->connection.publication_sequence==3 && pendingPublication->connection.configuration.revision==2,
        "Pre-Loaded native Apps snapshots retain latest authoritative publication rather than dropping or overwriting with older state");
    auto a=Path(L"C:\\One\\app.exe"),b=Path(L"C:\\Two\\app.exe");
    ApplicationRow rowA{a,L"app.exe",applications::ApplicationPresence::Running,false,false,1,1,
        applications::MetadataProvenance::ProcessImageName};
    ApplicationRow rowB{b,L"app.exe",applications::ApplicationPresence::Running,true,false,2,1,
        applications::MetadataProvenance::ProcessImageName};
    std::vector<applications::AppIdentity> keys{a,b};
    std::vector<ApplicationRow> rows{rowA,rowB};
    Check(settings::ReconcileApplicationItems(keys,rows).empty(),"Unchanged host observation retains native item/control instances");
    rows[0].enabled=true; rows[0].explicit_preference=true;
    Check(settings::ReconcileApplicationItems(keys,rows).empty(),
        "All-off app toggle draft and added Remove saved choice do not replace focused row or touch items collection");
    rows[0].window_count=4; rows[0].process_count=3; rows[0].display_name=L"refreshed app";
    Check(settings::ReconcileApplicationItems(keys,rows).empty(),"Snapshot metadata/count refresh retains keyed app controls");
    rows[0].identity=Path(L"C:\\one\\app.exe");
    Check(settings::ReconcileApplicationItems(keys,rows).empty(),"Ordinal-equal stable identity retains app-toggle instance");
    auto saved=rows[0]; saved.presence=applications::ApplicationPresence::SavedNotRunning;
    saved.display_name.clear(); saved.provenance.reset(); saved.window_count=0; saved.process_count=0;
    rows={rowB,saved};
    auto edits=settings::ReconcileApplicationItems(keys,rows);
    Check(edits.size()==1 && edits[0].change==settings::AppItemChange::move &&
        edits[0].index==1 && edits[0].destination==0,"Running-to-saved ordering moves retained item without recreating identity-keyed control");
    std::vector<unsigned> controls{17,29};
    auto focused=controls[0];
    for (auto edit:edits) {
        auto retained=controls[edit.index]; controls.erase(controls.begin()+edit.index);
        controls.insert(controls.begin()+edit.destination,retained);
    }
    Check(controls[1]==focused,"Production keyed move plan retains focused control token");
    Check(settings::FocusAfterApplicationUpdate(settings::AppFocusControl::remove_choice,true,true)==
        settings::AppFocusControl::remove_choice,
        "Retained row move preserves exact focused Remove saved choice subcontrol, not its toggle");
    Check(settings::FocusAfterApplicationUpdate(settings::AppFocusControl::toggle,true,true)==
        settings::AppFocusControl::toggle,
        "Adding a saved-choice button preserves focused application toggle");
    Check(settings::FocusAfterApplicationUpdate(settings::AppFocusControl::remove_choice,true,false)==
        settings::AppFocusControl::toggle,
        "Only collapsed focused Remove saved choice falls back to the same owner's toggle");
    Check(settings::FocusAfterApplicationUpdate(settings::AppFocusControl::remove_choice,false,false)==
        settings::AppFocusControl::defaults &&
        settings::FocusAfterApplicationUpdate(settings::AppFocusControl::toggle,false,false)==
        settings::AppFocusControl::defaults,
        "Only disappearing focused owner falls back to Apps default control");
    Check(settings::PreservedViewportOffset(0,244)==0 && settings::PreservedViewportOffset(244,500)==244,
        "Native viewport restore policy retains exact user offset when a saved-choice button changes layout");
    Check(settings::PreservedViewportOffset(244,120)==120 && settings::PreservedViewportOffset(244,0)==0,
        "Native viewport restore clamps only when removed content reduces available scroll extent");
    {
        using Kind=settings::RulesDraft::Kind;
        Check(settings::RulesDraft::KindOf(contracts::SetRulesText{})==Kind::save_text &&
            settings::RulesDraft::KindOf(contracts::ReadRulesFile{})==Kind::read_file &&
            settings::RulesDraft::KindOf(contracts::SelectPng{})==Kind::other &&
            settings::RulesDraft::KindOf(contracts::RemovePng{})==Kind::other,
            "Only rules mutations are classified as rules-document mutations");
        settings::RulesDraft draft;
        draft.Edit(); draft.Begin(Kind::other); draft.Settle(true);
        Check(draft.Dirty(),"Successful chart, selection or app-color mutation preserves unsaved rules text");
        draft.Begin(Kind::save_text); draft.Settle(false);
        Check(draft.Dirty(),"Rejected rules save retains unsaved rules text");
        draft.Begin(Kind::save_text); draft.Settle(true);
        Check(!draft.Dirty(),"Saved SetRulesText settles the rules draft");
        draft.Edit(); draft.Begin(Kind::save_text); draft.Edit(); draft.Settle(true);
        Check(draft.Dirty(),"Newer rules edit survives an older rules-save acknowledgment");
        draft.Begin(Kind::read_file); draft.Settle(true);
        Check(!draft.Dirty(),"Saved explicit rules-file import replaces the rules draft");
        draft.Edit(); draft.Settle(true);
        Check(draft.Dirty(),"Settlement without a queued rules mutation cannot clear the draft");
    }
    {
        using Kind=settings::ChartMutation;
        auto actions=settings::ChartActions(true,false,false,false);
        Check(!actions.use_chart && !actions.manage_chart && !actions.clear_chart,
            "Placeholder choice cannot queue Use selected chart, replace or remove");
        actions=settings::ChartActions(true,true,false,false);
        Check(actions.use_chart && actions.manage_chart && !actions.clear_chart,
            "A known chosen chart enables Use selected chart without exposing Clear imported chart");
        actions=settings::ChartActions(true,true,true,true);
        Check(!actions.use_chart && actions.clear_chart,
            "An already active chart needs no Use selected chart and keeps explicit Clear imported chart");
        actions=settings::ChartActions(false,true,false,true);
        Check(!actions.use_chart && !actions.manage_chart && !actions.clear_chart,
            "Pending or unauthoritative state disables every chart action");
        auto chart=[](char const* id,char const* name) { return contracts::ChartMetadata{id,name,"h",1,2,2}; };
        std::vector<contracts::ChartMetadata> before{chart("old","old")},after{chart("old","old"),chart("new","personal-cu")};
        Check(settings::ClassifyChartMutation(contracts::SelectPng{})==Kind::clear &&
            settings::ClassifyChartMutation(contracts::SelectPng{"new"})==Kind::select &&
            settings::ClassifyChartMutation(contracts::AddPng{})==Kind::add &&
            settings::ClassifyChartMutation(contracts::RemovePng{})==Kind::remove &&
            settings::ClassifyChartMutation(contracts::SetRulesText{})==Kind::none,
            "Empty SelectPng is the explicit clear action and no other mutation is a chart selection");
        Check(settings::ChartToHighlight(Kind::add,"personal-cu","",before,after,false,true)=="new",
            "Acknowledged AddPng highlights the added asset by its exact ack identity");
        Check(settings::ChartToHighlight(Kind::add,"renamed","",before,after,false,true)=="new",
            "A single newly acknowledged asset is highlighted even if the host normalized its name");
        Check(settings::ChartToHighlight(Kind::add,"personal-cu","",before,after,true,true).empty(),
            "A newer user combo choice is never replaced by the add highlight");
        Check(settings::ChartToHighlight(Kind::add,"personal-cu","",before,after,false,false).empty() &&
            settings::ChartToHighlight(Kind::select,"","new",before,after,false,true).empty() &&
            settings::ChartToHighlight(Kind::clear,"","",before,after,false,true).empty(),
            "Highlight never follows a rejected add or an explicit select or clear");
        Check(settings::ChartToHighlight(Kind::replace,"","old",before,after,false,true)=="old" &&
            settings::ChartToHighlight(Kind::replace,"","gone",before,after,false,true).empty(),
            "Acknowledged ReplacePng highlights only an asset still present");
    }
    rows={rowA}; edits=settings::ReconcileApplicationItems(keys,rows);
    Check(edits.size()==1 && edits[0].change==settings::AppItemChange::remove && edits[0].index==1,
        "Removing another saved choice cannot reconstruct the surviving focused app row");
    rows={rowB,rowA}; edits=settings::ReconcileApplicationItems({a},rows);
    Check(edits.size()==1 && edits[0].change==settings::AppItemChange::insert && edits[0].index==0,
        "New owner inserts only its native row while retaining existing control identity");
    {
        std::vector<ApplicationRow> many; std::vector<applications::AppIdentity> order;
        for (unsigned i=0;i<64;++i) {
            many.push_back({Path((L"C:\\Many\\app"+std::to_wstring(i)+L".exe").c_str()),L"app",applications::ApplicationPresence::Running,
                true,false,1,1,applications::MetadataProvenance::ProcessImageName});
            order.push_back(many.back().identity);
        }
        Check(settings::ReconcileApplicationItems(order,many).empty(),"Matching identity order skips reconciliation");
        std::reverse(many.begin(),many.end()); many.erase(many.begin()+10); many.push_back(rowA);
        auto plan=settings::ReconcileApplicationItems(order,many);
        std::set<applications::AppIdentity> rowControls;
        for (auto const& row:many) rowControls.insert(row.identity);
        for (auto const& identity:order) rowControls.insert(identity);
        for (auto edit:plan) {
            if (edit.change==settings::AppItemChange::remove) {
                rowControls.erase(order[edit.index]); order.erase(order.begin()+edit.index);
            } else if (edit.change==settings::AppItemChange::insert) order.insert(order.begin()+edit.index,many[edit.index].identity);
            else { auto moved=order[edit.index]; order.erase(order.begin()+edit.index); order.insert(order.begin()+edit.destination,moved); }
        }
        bool same=order.size()==many.size();
        for (std::size_t i=0;same && i<order.size();++i) same=order[i]==many[i].identity;
        Check(same,"Reorder, removal and insertion plan converges to the desired identity order");
        same=rowControls.size()==many.size();
        for (std::size_t i=0;same && i<many.size();++i) same=rowControls.contains(many[i].identity);
        Check(same,"Remove edits alone retire exactly the keyed controls absent from the snapshot");
    }
    settings::ConnectionState initial;
    initial.connected=true; initial.configuration_authoritative=true;
    initial.publication_sequence=1; initial.server_instance=NewIdentity(); initial.client_lease=4;
    settings::DraftState appearance; appearance.Apply(initial);
    auto revisionOnly=initial; revisionOnly.publication_sequence=2; revisionOnly.configuration.revision=1;
    revisionOnly.configuration.eligibility=std::get<applications::EligibilityPreferences>(
        revisionOnly.configuration.eligibility.Apply({false,true,{}}));
    auto previous=appearance.Controls(); auto update=appearance.Apply(revisionOnly);
    Check(update.accepted && appearance.Displayed().configuration.revision==1 &&
        !appearance.Controls().eligibility.DefaultEnabled() && settings::SameAppearance(previous,appearance.Controls()),
        "Actual appearance draft advances displayed CAS on eligibility-only revision without requiring native control/preview reset");
    auto changed=revisionOnly.configuration; changed.border_width_dip=24;
    Check(!settings::SameAppearance(changed,appearance.Controls()),"Genuine width change still updates production appearance controls");
    changed=revisionOnly.configuration; changed.design_selection=DesignSelection::original; changed.design_id="spotify";
    Check(!settings::SameAppearance(changed,appearance.Controls()),"Genuine design change still updates native selection and preview");
}
void DedupByteBudget(std::filesystem::path const& root) {
    ConfigurationServer server({root/L"apps-dedup-budget",L"apps-dedup-"+std::to_wstring(GetCurrentProcessId()),Self()});
    server.Start();
    Message request; request.client=NewIdentity(); request.request_id=1;
    auto snapshot=Exchange(server.Endpoint(),Self(),request);
    applications::EligibilityMutation mutation;
    for (unsigned i=0;i<3;++i) {
        auto path=L"C:\\"+std::wstring(30000,L'x')+std::to_wstring(i)+L".exe";
        mutation.edits.emplace_back(applications::SetApplicationPreference{
            *applications::AppIdentity::FromExecutablePath(path),false});
    }
    auto original=snapshot; original.operation=Operation::eligibility; original.request_id=2;
    original.eligibility=mutation;
    auto current=Exchange(server.Endpoint(),Self(),original);
    Check(current.status==Status::saved && current.configuration.eligibility.Exceptions().size()==3,
        "Large but bounded identity preferences commit through actual IPC");
    for (unsigned i=0;i<10;++i) {
        current=Edit(server,current,mutation);
        Check(current.status==Status::saved,"Byte-budget dedup fixture commits without count exhaustion");
    }
    auto revision=current.configuration.revision;
    Check(Exchange(server.Endpoint(),Self(),original).status==Status::duplicate_expired &&
        server.Snapshot().revision==revision,"Aggregate-byte dedup eviction cannot revive old request before record-count limit");
    server.Stop();
}
void EligibilityEpochs(std::filesystem::path const& root) {
    ConfigurationServer server({root/L"apps-epochs",L"apps-epochs-"+std::to_wstring(GetCurrentProcessId()),Self()});
    server.Start();
    auto a=Window(51,51,5100,1,L"C:\\One\\app.exe");
    auto b=Window(52,52,5200,2,L"C:\\Two\\app.exe");
    server.PublishApplications({a,b});
    auto a_id=std::get<applications::AppMetadata>(*a.metadata).Identity();
    auto b_id=std::get<applications::AppMetadata>(*b.metadata).Identity();
    auto a_epoch=server.EligibilityRevision(a_id),b_epoch=server.EligibilityRevision(b_id);
    applications::EligibilityMutation mutation;
    mutation.edits.emplace_back(applications::SetApplicationPreference{a_id,false});
    auto disabled=server.SubmitEligibility(mutation,0).get();
    Check(disabled.status==Status::saved && server.EligibilityRevision(a_id)>a_epoch &&
        server.EligibilityRevision(b_id)==b_epoch,"Durable app-only commit advances only affected live identity epoch");
    mutation.edits={applications::SetApplicationPreference{a_id,true}};
    auto enabled=server.SubmitEligibility(mutation,1).get();
    Check(enabled.status==Status::saved && enabled.configuration.eligibility.Allows(a_id) &&
        server.EligibilityRevision(a_id)==2 && server.EligibilityRevision(b_id)==b_epoch,
        "Disable/re-enable between host observations remains fenced despite equal final enabledness");
    auto default_off=server.SubmitEligibility({false,false,{}},2).get();
    Check(default_off.status==Status::saved && server.EligibilityRevision(a_id)==2 &&
        server.EligibilityRevision(b_id)==3,"Default-only preserves explicit owner epoch and advances default-controlled owner");
    auto off=server.SubmitEligibility({false,true,{}},3).get();
    Check(off.status==Status::saved && server.EligibilityRevision(a_id)==4 &&
        server.EligibilityRevision(b_id)==3,"All-off clearing advances only identities whose effective decision changed");
    server.PublishApplications({b});
    mutation.edits={applications::SetApplicationPreference{a_id,true}};
    auto future=server.SubmitEligibility(mutation,4).get();
    server.PublishApplications({Window(53,53,5300,3,L"C:\\One\\app.exe"),b});
    Check(future.status==Status::saved && server.EligibilityRevision(a_id)==5 &&
        server.EligibilityRevision(b_id)==3,"New owner pins a current epoch without resurrecting retired window work");
    server.Stop();
}
}
int RunApplicationConfigurationTests(std::filesystem::path const& root) {
    checks=0; MigrationAndBounds(root); LiveIpc(root); ProductionDraft(); PresentationContracts(); DedupByteBudget(root);
    EligibilityEpochs(root); return checks;
}
