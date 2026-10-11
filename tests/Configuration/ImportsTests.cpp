#include "..\..\src\Configuration\Storage.h"
#include "..\..\src\Configuration\Pipe.h"
#include "..\..\src\Settings\Session.h"
#include "..\..\src\Host\Appearance.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
using namespace window_sweaters;
using namespace contracts;
using namespace configuration;
int checks{};
void Check(bool value,char const* label) {
    ++checks;
    if (!value) throw std::runtime_error(std::string(label)+" (import assertion "+std::to_string(checks)+")");
}
std::filesystem::path Self() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768); return path;
}
std::vector<char> Bytes(std::filesystem::path const& file) {
    std::ifstream input(file,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};
}
void Write(std::filesystem::path const& file,std::string const& text) {
    std::ofstream output(file,std::ios::binary); output.write(text.data(),static_cast<std::streamsize>(text.size()));
}
void Put(std::vector<char>& bytes,std::size_t offset,std::uint64_t value,unsigned count=4) {
    for (unsigned i=0;i<count;++i) bytes[offset+i]=static_cast<char>((value>>(8*i))&255);
}
void WriteRecord(std::filesystem::path const& path,std::vector<char> bytes) {
    std::uint32_t crc=0xffffffff;
    for (std::size_t i=0;i<bytes.size()-4;++i) {
        crc^=static_cast<unsigned char>(bytes[i]);
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    Put(bytes,bytes.size()-4,static_cast<std::uint32_t>(~crc));
    std::ofstream out(path,std::ios::binary); out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
}
applications::RunningApplicationWindow Window(std::uintptr_t hwnd,wchar_t const* path) {
    auto token=applications::TargetToken::FromTarget({hwnd,static_cast<std::uint32_t>(hwnd)+1000,hwnd+100,1},0);
    auto resolved=applications::ResolveMetadata(applications::NoPackagedIdentity{},applications::ExecutablePath{path});
    return {*token,std::make_shared<const applications::MetadataResult>(std::move(resolved))};
}
Message Observe(ConfigurationServer& server,Message request={}) {
    if (EmptyIdentity(request.client)) request.client=NewIdentity();
    request.operation=Operation::snapshot; request.error=0; request.status=Status::saved; ++request.request_id;
    return Exchange(server.Endpoint(),Self(),request);
}
Message Edit(ConfigurationServer& server,Message base,ImportMutation mutation) {
    base.operation=Operation::imports; base.status=Status::saved; base.error=0; ++base.request_id;
    base.import_edit=std::move(mutation); return Exchange(server.Endpoint(),Self(),base);
}
void Png(std::filesystem::path const& file,std::uint32_t first=0xffe02152u) {
    knitting::RasterImage image;
    image.width=2; image.height=2; image.stride=8;
    image.pixels={first,0xff37d0a6u,0,first};
    knitting::SavePng(image,file.c_str());
}
void Persistence(std::filesystem::path const& root) {
    auto directory=root/L"imports"; std::filesystem::create_directories(directory);
    auto png=directory/L"literal.png",replacement=directory/L"replacement.png",bad=directory/L"bad.png";
    Png(png); Png(replacement,0xff1357a9u); Write(bad,"not a PNG");
    auto first=Window(100,L"C:\\Fixture\\first\\Player.exe");
    auto second=Window(101,L"C:\\Fixture\\second\\Player.exe");
    auto unique=Window(102,L"C:\\Fixture\\Unique.exe");
    auto uniqueIdentity=std::get<applications::AppMetadata>(*unique.metadata).Identity();
    auto playerIdentity=std::get<applications::AppMetadata>(*first.metadata).Identity();
    std::string stable;
    std::uint64_t high=0;
    {
        ConfigurationServer server({directory,L"imports-"+std::to_wstring(GetCurrentProcessId()),Self()}); server.Start();
        server.PublishApplications({first,second,unique});
        auto initial=Observe(server);
        auto added=Edit(server,initial,AddPng{"literal",png.native()});
        Check(added.status==Status::saved && added.response_to==Operation::imports &&
            std::get<AddPng>(added.import_edit).path==png.native(),"Actual typed pipe acknowledges the exact local PNG operation");
        auto owned=server.OwnedSnapshot();
        auto const& asset=owned.catalogue->Charts()[0]; stable=asset.Id(); high=asset.ContentRevision();
        Check(high==1 && asset.Data().Cells()[0]==0xffe02152u && asset.Data().Cells()[1]==0xff37d0a6u &&
            asset.Data().Cells()[2]==0,"Host validated PNG preserves literal cells and base-alpha sentinel");
        Check(Encode(Observe(server,added)).size()<1024 && Bytes(Storage(directory).File()).size()>100,
            "Observation carries headers, not encoded PNG blobs");
        auto before=Bytes(Storage(directory).File());
        auto invalid=Edit(server,added,ReplacePng{stable,bad.native(),{}});
        Check(invalid.status==Status::invalid && !invalid.import_failure.reason.empty() &&
            invalid.configuration==added.configuration && Bytes(Storage(directory).File())==before &&
            server.OwnedSnapshot().catalogue==owned.catalogue,"Invalid replacement preserves disk bytes, immutable catalogue and saved revision");
        auto duplicate=Edit(server,Observe(server,invalid),AddPng{"literal",replacement.native()});
        Check(duplicate.status==Status::invalid && duplicate.import_failure.code==static_cast<unsigned>(imports::ErrorCode::DuplicateName)+1,
            "Duplicate semantic name requires explicit replacement");
        auto selected=Edit(server,Observe(server,duplicate),SelectPng{stable});
        auto manual=integration::ImportedAppearance(selected.configuration.imports,server.OwnedSnapshot().catalogue);
        auto tile=host::MakeAppearance(selected.configuration,&manual);
        Check(tile.chart && tile.chart->argbCells==std::vector<std::uint32_t>{0xffe02152u,0xff37d0a6u,0,0xffe02152u},
            "Actual Host appearance adapter consumes manually selected imported PNG without recoloring");
        auto request=Observe(server,selected); request.operation=Operation::imports; ++request.request_id;
        request.import_edit=ReplacePng{stable,replacement.native(),{}};
        auto changed=Exchange(server.Endpoint(),Self(),request);
        auto retried=Exchange(server.Endpoint(),Self(),request);
        Check(changed.status==Status::saved && retried.configuration==changed.configuration &&
            changed.configuration.imports.charts[0].id==stable && changed.configuration.imports.charts[0].revision==2,
            "Changed content retains asset identity and exact request retry deduplicates one durable revision");
        request.import_edit=RemovePng{stable};
        Check(Exchange(server.Endpoint(),Self(),request).status==Status::invalid,
            "Request ID cannot be reused for a different typed import edit");
        auto stale=selected; stale.request_id=changed.request_id+10;
        Check(Edit(server,stale,RemovePng{stable}).status==Status::conflict,"Displayed-revision CAS rejects stale import removal");
        auto rules=Edit(server,Observe(server,changed),SetRulesText{"# keep spelling\nPlayer = #224466\nUnique = #113355 literal\nMissing = #abcdef\n"});
        Check(rules.status==Status::saved && rules.configuration.imports.diagnostics.size()==3 &&
            rules.configuration.imports.diagnostics[0].match==RuleMatch::ambiguous &&
            rules.configuration.imports.diagnostics[0].candidates.size()==2 &&
            rules.configuration.imports.diagnostics[2].match==RuleMatch::unmatched &&
            rules.configuration.imports.bindings.size()==1 &&
            rules.configuration.imports.bindings[0].application==uniqueIdentity,
            "Authoritative executable metadata exposes ambiguous and unmatched rules; only exact unique identity binds");
        auto byApp=rules.configuration; byApp.design_selection=DesignSelection::by_app;
        auto configured=server.SubmitLocal(byApp).get();
        auto personal=integration::ImportedAppearance(configured.configuration.imports,server.OwnedSnapshot().catalogue);
        auto app=host::MakeAppearance(configured.configuration,uniqueIdentity,{},colorways::AutomaticAppearance{
            0xff445566u,0xffeeeeeeu,"zigzag"},{},&personal);
        Check(app.baseArgb==0xff113355u && app.chart && app.chart->semanticId==stable &&
            app.chart->argbCells[0]==0xff1357a9u,"Personal bound chart and color override icon proposal with literal PNG RGB");
        auto player=Edit(server,Observe(server,rules),SetAppColor{playerIdentity,0xff214365u,{}});
        auto plain=integration::ImportedAppearance(player.configuration.imports,server.OwnedSnapshot().catalogue);
        auto plainApp=host::MakeAppearance(player.configuration,playerIdentity,{},colorways::AutomaticAppearance{
            0xff777777u,0xffeeeeeeu,"zigzag"},{},&plain);
        Check(plainApp.baseArgb==0xff214365u && !plainApp.chart,"Typed per-app color omission binds explicit PlainPattern, not icon fallback");
        auto shared=player.configuration; shared.design_selection=DesignSelection::shared_zigzag;
        auto sharedTile=host::MakeAppearance(shared,uniqueIdentity,{},colorways::AutomaticAppearance{
            0xff445566u,0xffeeeeeeu,"zigzag"},{},&plain);
        Check(sharedTile.baseArgb==0xff113355u && sharedTile.chart && sharedTile.chart->semanticId=="automatic-zigzag" &&
            sharedTile.chart->argbCells[0]!=0xff1357a9u,"Shared Zigzag global priority retains personal base color but uses generated contrast");
        auto manualPlain=player.configuration; manualPlain.design_selection=DesignSelection::plain;
        auto explicitPlain=host::MakeAppearance(manualPlain,uniqueIdentity,{},std::nullopt,{},&plain);
        Check(explicitPlain.baseArgb==0xff113355u && !explicitPlain.chart,
            "Global plain pattern overrides personal chart while preserving public-resolver personal color priority");
        auto manualOriginal=manualPlain; manualOriginal.design_selection=DesignSelection::original; manualOriginal.design_id="messages";
        auto explicitOriginal=host::MakeAppearance(manualOriginal,uniqueIdentity,{},std::nullopt,{},&plain);
        Check(explicitOriginal.baseArgb==0xff113355u && explicitOriginal.chart &&
            explicitOriginal.chart->semanticId=="messages","Manual original theme wins pattern selection without erasing personal application color");
        auto manualImport=manualPlain; manualImport.imports.selected_id=stable;
        auto explicitImport=host::MakeAppearance(manualImport,uniqueIdentity,{},std::nullopt,{},&plain);
        Check(explicitImport.baseArgb==0xff113355u && explicitImport.chart && explicitImport.chart->semanticId==stable &&
            explicitImport.chart->argbCells[0]==0xff1357a9u,"Selected manual PNG wins global pattern with personal base color and literal user yarn");
        auto fresh=Observe(server,player);
        auto appearance=fresh.configuration; appearance.imports={}; appearance.stitch_rows=7.f;
        auto preserved=server.SubmitLocal(appearance).get();
        Check(preserved.status==Status::saved && preserved.configuration.imports==fresh.configuration.imports,
            "Stale full-appearance values cannot erase committed imports");
        auto blocked=Edit(server,Observe(server,fresh),RemovePng{stable});
        Check(blocked.status==Status::invalid && server.OwnedSnapshot().catalogue->FindById(stable),
            "Removing a non-built-in chart referenced by saved rules fails before mutation");
        auto clear=Edit(server,Observe(server,blocked),SetRulesText{"Player = #224466\nUnique = #113355\nMissing = #abcdef\n"});
        auto removed=Edit(server,clear,RemovePng{stable});
        Check(removed.status==Status::saved && removed.configuration.imports.charts.empty() &&
            removed.configuration.imports.high_water==2 && removed.configuration.imports.selected_id.empty(),
            "Final asset removal retains catalogue high-water and safely clears manual selection");
        before=Bytes(Storage(directory).File());
        auto restored=Storage(directory).Load();
        Check(restored.state==StorageState::ready && restored.configuration==removed.configuration &&
            restored.catalogue->Charts().empty() && restored.catalogue->RevisionHighWater()==2 &&
            Bytes(Storage(directory).File())==before,"Restart restores exact bindings, diagnostics and empty high-water without startup rewrite");
        server.Stop();
    }
    ConfigurationServer restarted({directory,L"imports-restart-"+std::to_wstring(GetCurrentProcessId()),Self()}); restarted.Start();
    restarted.PublishApplications({unique});
    auto loaded=Observe(restarted);
    Check(loaded.configuration.imports.diagnostics[0].match==RuleMatch::ambiguous &&
        loaded.configuration.imports.diagnostics[0].candidates.size()==2,"Restart does not rebind an originally ambiguous rule to currently unique metadata");
    auto readded=Edit(restarted,loaded,AddPng{"literal",png.native()});
    Check(readded.status==Status::saved && readded.configuration.imports.charts[0].id==stable &&
        readded.configuration.imports.charts[0].revision==3 && readded.configuration.imports.high_water>high,
        "Remove all, restart, reimport same pixels allocates a strictly increasing numeric revision");
    auto rulefile=directory/L"rules.txt"; Write(rulefile,"Unique = #010203 literal\n");
    auto fromFile=Edit(restarted,readded,ReadRulesFile{rulefile.native()});
    Check(fromFile.status==Status::saved && fromFile.configuration.imports.document=="Unique = #010203 literal\n",
        "Native local rules-file operation reads, validates and persists exact declarative text");
    auto missing=Edit(restarted,fromFile,AddPng{"absent",(directory/L"absent.png").native()});
    Check(missing.status==Status::invalid && !missing.import_failure.reason.empty(),"Missing local PNG produces explicit error and preserves previous acknowledgement");
    auto invalidRules=Edit(restarted,Observe(restarted,missing),SetRulesText{"Unique = #bad\n"});
    Check(invalidRules.status==Status::invalid && invalidRules.import_failure.line==1 &&
        invalidRules.configuration.imports.document==fromFile.configuration.imports.document,
        "Invalid declarative edit preserves prior document and per-rule diagnostics");
    auto wire=Encode(Observe(restarted,invalidRules));
    Message decoded; Check(Decode(wire,decoded),"Bounded metadata snapshot validates");
    auto malformed=wire; malformed[8]=std::byte{0xff};
    Check(!Decode(malformed,decoded),"Malformed import-aware aggregate frame rejected");
    wire=Encode(missing); wire.pop_back(); Check(!Decode(wire,decoded),"Truncated typed import acknowledgement rejected");
    auto pointer=restarted.OwnedSnapshot().catalogue;
    for (unsigned i=0;i<32;++i) {
        (void)Observe(restarted,fromFile);
        Check(restarted.OwnedSnapshot().catalogue==pointer,"Cheap IPC observation reuses immutable catalogue without redecoding or copying blobs");
    }
    restarted.Stop();
}
void Schema4(std::filesystem::path const& root) {
    auto directory=root/L"imports-schema4"; Storage storage(directory);
    Configuration old; old.revision=47; old.paused=true; old.border_width_dip=23.5f;
    old.design_selection=DesignSelection::shared_zigzag; old.stitch_style=StitchStyle::garter;
    old.material_style=MaterialStyle::plain; old.stitch_rows=9.5f; old.yarn_argb=0xff172839u;
    auto identity=*applications::AppIdentity::FromExecutablePath(L"C:\\Fixture\\Saved.exe");
    applications::EligibilityMutation edit; edit.defaultEnabled=false;
    edit.edits.emplace_back(applications::SetApplicationPreference{identity,true});
    old.eligibility=std::get<applications::EligibilityPreferences>(old.eligibility.Apply(edit));
    Check(storage.Save(old)==0,"Create isolated schema migration fixture");
    auto bytes=Bytes(storage.File());
    std::uint32_t frameLength=0;
    for (unsigned i=0;i<4;++i) frameLength|=static_cast<unsigned char>(bytes[12+8+i])<<(8*i);
    auto offset=12+frame_size;
    while (offset<12+frameLength) {
        std::uint32_t tag=0,size=0;
        for (unsigned i=0;i<4;++i) {
            tag|=static_cast<unsigned char>(bytes[offset+i])<<(8*i);
            size|=static_cast<unsigned char>(bytes[offset+4+i])<<(8*i);
        }
        if (tag==6) { bytes.erase(bytes.begin()+offset,bytes.begin()+offset+8+size); frameLength-=8+size; break; }
        offset+=8+size;
    }
    bytes.resize(12+frameLength+4); Put(bytes,4,4); Put(bytes,8,bytes.size());
    Put(bytes,12+4,5,2); Put(bytes,12+64,4); Put(bytes,12+8,frameLength);
    WriteRecord(storage.File(),bytes); auto exact=Bytes(storage.File());
    auto loaded=storage.Load();
    Check(loaded.state==StorageState::ready && loaded.configuration==old && loaded.catalogue->RevisionHighWater()==0 &&
        Bytes(storage.File())==exact,"Schema4 protocol5 migrates all exact appearance and exclusion fields in memory without rewrite");
}
struct Observations {
    std::mutex mutex; std::condition_variable wake; std::vector<settings::ConnectionState> states;
    void Add(settings::ConnectionState state) { std::lock_guard lock(mutex); states.push_back(std::move(state)); wake.notify_all(); }
    template<class P> settings::ConnectionState Wait(P predicate) {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock,std::chrono::seconds(15),[&] {
            return std::any_of(states.begin(),states.end(),predicate);
        }),"Production settings Session publishes expected import state");
        for (auto i=states.rbegin();i!=states.rend();++i) if (predicate(*i)) return *i;
        throw std::runtime_error("Missing import observation");
    }
};
void SessionCases(std::filesystem::path const& root) {
    auto directory=root/L"imports-session"; std::filesystem::create_directories(directory);
    auto png=directory/L"session.png"; Png(png);
    ConfigurationServer server({directory,L"imports-session-"+std::to_wstring(GetCurrentProcessId()),Self()}); server.Start();
    Observations observed; std::atomic<unsigned> launches{},attempts{};
    settings::SessionTransport transport;
    transport.exchange=[&](Message const& request,HANDLE cancel) {
        auto response=Exchange(server.Endpoint(),Self(),request,cancel);
        if (request.operation==Operation::imports) {
            auto attempt=++attempts;
            if (attempt==1) throw std::runtime_error("Lost committed import acknowledgement");
            if (attempt==2) response.response_to=Operation::mutate;
        }
        return response;
    };
    transport.start_host=[&] { ++launches; return false; };
    settings::Session session({true,directory,L"imports-session-"+std::to_wstring(GetCurrentProcessId())},
        [&](auto state) { observed.Add(std::move(state)); },transport);
    auto displayed=observed.Wait([](auto const& s) {return s.connected && s.configuration_authoritative && !s.pending;});
    auto generation=session.Import(displayed,AddPng{"session",png.native()});
    Check(generation!=0,"Production Session queues a typed import with displayed CAS/lease");
    auto saved=observed.Wait([&](auto const& s) {
        return s.connected && !s.pending && s.status==Status::saved && s.settled_edit_generation>=generation;
    });
    Check(saved.configuration.revision==1 && saved.configuration.imports.charts.size()==1 && attempts==3 && launches==0,
        "Lost or wrong-operation import reply retries exact request ID; native Session confirms one durable commit and no host launch");
    settings::DraftState draft;
    Check(draft.Apply(saved).accepted && !draft.Pending(),"Existing draft reducer accepts confirmed import publication");
    auto duplicate=session.Import(saved,AddPng{"session",png.native()});
    auto rejected=observed.Wait([&](auto const& s) {return s.settled_edit_generation>=duplicate && s.status==Status::invalid;});
    Check(!rejected.import_failure.reason.empty() && rejected.configuration==saved.configuration,
        "Production Session exposes rejected import without optimistic committed label or replacing saved metadata");
    session.Stop();
    Check(Observe(server).configuration==saved.configuration,"Closing native Session leaves sole-writer Host and committed imports available");
    server.Stop();
}
void StorageFailureAndShadow(std::filesystem::path const& root) {
    auto directory=root/L"imports-storage-failure"; std::filesystem::create_directories(directory);
    auto png=directory/L"shadow.png",changed=directory/L"changed.png";
    Png(png); Png(changed,0xff102938u);
    ConfigurationServer server({directory,L"imports-failure-"+std::to_wstring(GetCurrentProcessId()),Self()}); server.Start();
    auto added=Edit(server,Observe(server),AddPng{"atelier-messages",png.native()});
    Check(added.status==Status::saved,"Host imports explicit authored semantic-name shadow");
    auto owned=server.OwnedSnapshot();
    auto id=owned.catalogue->Charts()[0].Id();
    auto cfg=added.configuration; cfg.design_selection=DesignSelection::original; cfg.design_id="messages";
    integration::ImportedAppearance imported(cfg.imports,owned.catalogue);
    auto shadow=host::MakeAppearance(cfg,&imported);
    auto source=designs::ws_design_chart_by_name("atelier-messages");
    Check(shadow.chart && shadow.chart->semanticId==id && shadow.chart->appearance.flags==source->flags &&
        shadow.chart->argbCells[0]==0xffe02152u,"Imported shadow resolves before built-in with exact retained source material and literal RGB");
    auto bytes=Bytes(Storage(directory).File());
    Message replace=Observe(server,added); replace.operation=Operation::imports; ++replace.request_id;
    replace.import_edit=ReplacePng{id,changed.native(),{}};
    {
        Handle locked(CreateFileW(Storage(directory).File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        Check(bool(locked),"Deny replacement using an isolated storage reader");
        auto failed=Exchange(server.Endpoint(),Self(),replace);
        Check(failed.status==Status::storage_error && failed.configuration==added.configuration &&
            server.OwnedSnapshot().catalogue==owned.catalogue && Bytes(Storage(directory).File())==bytes,
            "Failed durable import replacement retains exact old bytes, chart snapshot and acknowledgement");
    }
    auto retry=Exchange(server.Endpoint(),Self(),replace);
    Check(retry.status==Status::saved && retry.configuration.imports.charts[0].revision==2,
        "Failed save consumes no asset/configuration revision; exact request retry can durably succeed");
    auto removed=Edit(server,retry,RemovePng{id});
    integration::ImportedAppearance empty(removed.configuration.imports,server.OwnedSnapshot().catalogue);
    cfg.imports=removed.configuration.imports;
    auto restored=host::MakeAppearance(cfg,&empty);
    std::vector<std::uint32_t> authored;
    for (std::uint32_t y=0;y<source->height;++y)
        for (std::uint32_t x=0;x<source->width;++x) authored.push_back(designs::ws_design_cell_argb(source,x,y));
    Check(restored.chart && restored.chart->semanticId=="messages" &&
        restored.chart->argbCells==authored,
        "Removing an imported authored shadow restores exact immutable authored chart");
    auto held=server.OwnedSnapshot().catalogue;
    auto memory=Edit(server,removed,AddPng{"large-local",png.native()});
    Check(memory.status==Status::saved && held->Charts().empty() && held->RevisionHighWater()==2,
        "Published immutable catalogue remains valid when later mutation replaces the owned snapshot");
    server.Stop();
}
void OrdinaryEditsChainAfterImport(std::filesystem::path const& root,bool reset) {
    auto directory=root/(reset?L"imports-reset-chain":L"imports-design-chain");
    std::filesystem::create_directories(directory);
    auto png=directory/L"chain.png"; Png(png);
    auto suffix=(reset?L"imports-reset-chain-":L"imports-design-chain-")+std::to_wstring(GetCurrentProcessId());
    ConfigurationServer server({directory,suffix,Self()}); server.Start();
    auto added=Edit(server,Observe(server),AddPng{"chain",png.native()});
    auto selected=Edit(server,added,SelectPng{added.configuration.imports.charts[0].id});
    Check(selected.status==Status::saved && selected.configuration.imports.high_water==1,
        "Own-chain fixture starts with a durably selected imported chart and nonzero high-water");
    std::mutex gateMutex; std::condition_variable gateWake;
    bool entered=false,released=false;
    struct Release {
        std::mutex& mutex; std::condition_variable& wake; bool& released;
        ~Release() {std::lock_guard lock(mutex); released=true; wake.notify_all();}
    } release{gateMutex,gateWake,released};
    Observations observed; std::atomic<unsigned> writes{};
    settings::SessionTransport transport;
    transport.exchange=[&](Message const& request,HANDLE cancel) {
        auto result=Exchange(server.Endpoint(),Self(),request,cancel);
        if ((request.operation==Operation::mutate || request.operation==Operation::reset) && ++writes==1) {
            std::unique_lock lock(gateMutex); entered=true; gateWake.notify_all();
            if (!gateWake.wait_for(lock,std::chrono::seconds(15),[&] {return released;}))
                throw std::runtime_error("Own import predecessor gate timed out");
        }
        return result;
    };
    transport.start_host=[] {return false;};
    settings::Session session({true,directory,suffix},[&](auto state) {observed.Add(std::move(state));},transport);
    auto displayed=observed.Wait([](auto const& s) {return s.connected && s.configuration_authoritative && !s.pending;});
    auto next=reset?Configuration{}:displayed.configuration;
    next.design_selection=reset?DesignSelection::plain:DesignSelection::by_app;
    next.design_id.clear();
    auto first=reset?session.Reset(displayed):session.Change(displayed,next);
    Check(first!=0,"Native Session queues reset/design operation after imported selection");
    {
        std::unique_lock lock(gateMutex);
        Check(gateWake.wait_for(lock,std::chrono::seconds(15),[&] {return entered;}),
            "Real IPC predecessor durably commits before its acknowledgement reaches native Session");
    }
    next.border_width_dip=27.f;
    auto last=session.Change(displayed,next);
    Check(last>first,"Width edit queues against the same displayed snapshot while own acknowledgement is withheld");
    {std::lock_guard lock(gateMutex); released=true; gateWake.notify_all();}
    auto settled=observed.Wait([&](auto const& s) {return !s.pending && s.settled_edit_generation>=last;});
    Check(settled.status==Status::saved && settled.configuration.revision==selected.configuration.revision+2 &&
        settled.configuration.border_width_dip==27.f && settled.configuration.imports.selected_id.empty() &&
        settled.configuration.imports.high_water==1 && writes==2,
        reset?"Reset preserves imported high-water and chains queued width through exact own acknowledgement":
            "Design change clears selected PNG and chains queued width through exact own acknowledgement");
    Check(Storage(directory).Load().configuration==settled.configuration &&
        (reset?settled.configuration.imports.charts.empty():settled.configuration.imports.charts.size()==1),
        "Both ordinary own-chain operations persist exact final imports and appearance");
    session.Stop(); server.Stop();
}
void AboveWireLimit(std::filesystem::path const& root) {
    auto directory=root/L"imports-large"; std::filesystem::create_directories(directory);
    auto png=directory/L"large.png"; Png(png,0xff918273u);
    auto bytes=Bytes(png);
    std::vector<char> chunk(4+4+1024*1024+128+4);
    const auto length=chunk.size()-12;
    for (unsigned i=0;i<4;++i) chunk[i]=static_cast<char>((length>>(8*(3-i)))&255);
    chunk[4]='p'; chunk[5]='A'; chunk[6]='D'; chunk[7]='d';
    std::uint32_t crc=0xffffffff;
    for (std::size_t i=4;i<chunk.size()-4;++i) {
        crc^=static_cast<unsigned char>(chunk[i]);
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    crc=~crc;
    for (unsigned i=0;i<4;++i) chunk[chunk.size()-4+i]=static_cast<char>((crc>>(8*(3-i)))&255);
    bytes.insert(bytes.end()-12,chunk.begin(),chunk.end());
    { std::ofstream out(png,std::ios::binary); out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); }
    ConfigurationServer server({directory,L"imports-large-"+std::to_wstring(GetCurrentProcessId()),Self()}); server.Start();
    auto saved=Edit(server,Observe(server),AddPng{"large-local",png.native()});
    Check(saved.status==Status::saved && Bytes(Storage(directory).File()).size()>maximum_payload_bytes &&
        Encode(saved).size()<1024,"Validated PNG above 1 MiB is durably owned without increasing 1 MiB IPC frame bound");
    auto loaded=Storage(directory).Load();
    Check(loaded.state==StorageState::ready && loaded.catalogue->Charts()[0].Data().EncodedPng().size()==bytes.size(),
        "Bounded full disk envelope restores encoded blob above IPC limit");
    server.Stop();
}
}
int RunImportsTests(std::filesystem::path const& root) {
    auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Apartment { HRESULT result; ~Apartment() {if (SUCCEEDED(result)) CoUninitialize();} } cleanup{apartment};
    Check(SUCCEEDED(apartment),"Imports E2E COM apartment");
    checks=0; Persistence(root); Schema4(root); SessionCases(root); StorageFailureAndShadow(root); AboveWireLimit(root);
    OrdinaryEditsChainAfterImport(root,false); OrdinaryEditsChainAfterImport(root,true); return checks;
}
