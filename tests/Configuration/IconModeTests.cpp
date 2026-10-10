#include "..\..\src\Configuration\Storage.h"
#include "..\..\src\Configuration\Pipe.h"
#include "..\..\src\Settings\Session.h"
#include "..\..\src\Settings\Preview.h"
#include "..\..\src\Integration\IconAppearance.h"
#include <condition_variable>
#include <fstream>
#include <mutex>

namespace {
using namespace window_sweaters;
using namespace window_sweaters::contracts;
unsigned checks{};
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::vector<char> Bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void Put(std::vector<char>& bytes, std::size_t offset, std::uint64_t value, unsigned count = 4) {
    for (unsigned i=0; i<count; ++i) bytes[offset+i]=static_cast<char>((value>>(8*i))&255);
}
void WriteRecord(const std::filesystem::path& path, std::vector<char> bytes) {
    std::uint32_t crc=0xffffffffu;
    for (std::size_t i=0; i<bytes.size()-4; ++i) {
        crc^=static_cast<unsigned char>(bytes[i]);
        for (unsigned j=0; j<8; ++j) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    Put(bytes,bytes.size()-4,static_cast<std::uint32_t>(~crc));
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Cannot write isolated schema fixture");
}
applications::AppIdentity Exe(std::wstring_view value) {
    auto identity=applications::AppIdentity::FromExecutablePath(value);
    if (!identity) throw std::runtime_error("Invalid icon identity fixture");
    return *identity;
}
void SaltAndPreview() {
    const auto lower=Exe(L"C:\\Private\\élan.exe");
    const auto upper=Exe(L"c:\\PRIVATE\\ÉLAN.EXE");
    auto a=integration::MakeIconInput(lower,lower.Key());
    auto b=integration::MakeIconInput(upper,upper.Key());
    const auto& first=std::get<icon_yarn::IconInput>(a);
    const auto& second=std::get<icon_yarn::IconInput>(b);
    Check(lower==upper && first.canonicalAppIdentity==second.canonicalAppIdentity,
        "Ordinal-equal executable identities have identical kind-prefixed UTF8 motif salts");
    Check(first.canonicalAppIdentity.starts_with("exe:") &&
        icon_yarn::StableMotif(first.canonicalAppIdentity)==icon_yarn::StableMotif(second.canonicalAppIdentity),
        "Canonical motif is independent of image case and process lifetime");
    const auto packageA=*applications::AppIdentity::FromPackagedAumid(L"PrivateApp_8wekyb3d8bbwe!App");
    const auto packageB=*applications::AppIdentity::FromPackagedAumid(L"PrivateApp_8wekyb3d8bbwe!app");
    Check(std::get<icon_yarn::IconInput>(integration::MakeIconInput(packageA,{})).canonicalAppIdentity !=
        std::get<icon_yarn::IconInput>(integration::MakeIconInput(packageB,{})).canonicalAppIdentity,
        "Package AUMIDs retain exact ordinal case");
    Check(std::holds_alternative<icon_yarn::Fallback>(
        integration::MakeIconInput(lower,std::wstring(4097,L'x'))),
        "Overbound executable source is reported, never truncated");
    const auto longIdentity=Exe(L"C:\\"+std::wstring(4100,L'a')+L".exe");
    Check(std::holds_alternative<icon_yarn::Fallback>(integration::MakeIconInput(longIdentity,{})),
        "Overbound UTF8 identity is reported, never salted with a truncated key");
    Check(!integration::AutomaticProposal(first,icon_yarn::Fallback{icon_yarn::FallbackReason::IconUnavailable}),
        "Typed unavailable icon leaves resolver stable fallback, never a false color");
    const auto proposal=integration::AutomaticProposal(first,icon_yarn::SampledPalette{0xff769cc0u,0xfff6f0deu});
    Check(proposal && proposal->base_argb==0xff769cc0u &&
        *proposal->chart_name==icon_yarn::MotifName(icon_yarn::StableMotif(first.canonicalAppIdentity)),
        "Sampled proposal carries exact sample and stable generated motif");
    auto process=applications::ResolveMetadata(applications::PackagedAumid{packageA.Key()},
        applications::ExecutablePath{L"C:\\Private\\App.exe"});
    const auto& metadata=std::get<applications::AppMetadata>(process);
    Check(metadata.ExecutableSource()==L"C:\\Private\\App.exe" &&
        metadata.WithVerifiedProcessSource(metadata).ExecutableSource()==metadata.ExecutableSource(),
        "Verified standalone package process retains bounded transient full image source");
    auto other=applications::ResolveMetadata(applications::PackagedAumid{packageB.Key()},
        applications::ExecutablePath{L"C:\\Windows\\System32\\ApplicationFrameHost.exe"});
    Check(std::get<applications::AppMetadata>(other).ExecutableSource().empty(),
        "ApplicationFrameHost is never a packaged application's icon source");
    Check(metadata.WithVerifiedProcessSource(std::get<applications::AppMetadata>(other)).ExecutableSource()==
        metadata.ExecutableSource(),"A different hosted identity cannot substitute its executable source");
    auto zigzag=Configuration{};
    zigzag.design_selection=DesignSelection::shared_zigzag;
    Check(Valid(zigzag) && PerApplication(zigzag.design_selection) &&
        !PerApplication(DesignSelection::plain),"Shared Zigzag is an explicit fourth semantic selection");
    Message message; message.configuration=zigzag;
    Message decoded;
    Check(Decode(Encode(message),decoded) && decoded.configuration==zigzag,
        "Shared Zigzag uses the existing bounded 176-byte appearance prefix");
    zigzag.design_id="finder";
    Check(!Valid(zigzag),"Shared Zigzag cannot carry a parallel manual catalogue selection");
    zigzag.design_id.clear();
    auto tile=settings::MakeAppearance(zigzag);
    Check(tile.chart && tile.chart->semanticId=="automatic-zigzag" && tile.baseArgb==0xff769cc0u &&
        tile.chart->argbCells[0]==icon_yarn::ZigzagContrast(0xff769cc0u) &&
        tile.chart->argbCells[1]==0,"Representative preview uses real two-color generated yarn and base sentinels");
    Check(tile.chart->appearance.flags==48,"Generated motifs retain original defined/sculpted renderer flags");
}
void Schema3Migration(const std::filesystem::path& root) {
    configuration::Storage storage(root/L"schema3");
    Configuration original;
    original.revision=92; original.design_selection=DesignSelection::by_app;
    original.yarn_argb=0xff112233u; original.stitch_rows=4.5f;
    const auto excluded=Exe(L"C:\\Private\\Disabled.exe");
    applications::EligibilityMutation mutation;
    mutation.edits.emplace_back(applications::SetApplicationPreference{excluded,false});
    original.eligibility=std::get<applications::EligibilityPreferences>(
        original.eligibility.Apply(mutation));
    Check(storage.Save(original)==0,"Prepare isolated framed disk fixture through production writer");
    auto schema3=Bytes(storage.File());
    Put(schema3,4,3); Put(schema3,12+4,4,2); Put(schema3,12+64,3);
    WriteRecord(storage.File(),schema3);
    const auto unchanged=Bytes(storage.File());
    auto loaded=storage.Load();
    Check(loaded.state==configuration::StorageState::ready && loaded.configuration==original &&
        Bytes(storage.File())==unchanged,"Schema3 protocol4 appearance and exclusions migrate in memory without startup rewrite");
    auto oldWire=Encode(Message{});
    oldWire[4]=std::byte{4};
    Message rejectedWire;
    Check(FrameLength(std::span(oldWire).first(frame_header_size))==0 && !Decode(oldWire,rejectedWire),
        "Protocol4 is not admitted by the live protocol5 endpoint");
    auto bad=unchanged; Put(bad,12+88,3); WriteRecord(storage.File(),bad);
    const auto invalid=Bytes(storage.File());
    Check(storage.Load().state==configuration::StorageState::corrupt && storage.Save({})!=0 &&
        Bytes(storage.File())==invalid,"Old schema cannot acquire new Zigzag semantics by numeric reinterpretation");
    bad=unchanged; Put(bad,4,4); WriteRecord(storage.File(),bad);
    Check(storage.Load().state==configuration::StorageState::corrupt,
        "Outer schema4 cannot disguise a protocol4/schema3 frame");
    bad=unchanged; Put(bad,4,5); WriteRecord(storage.File(),bad);
    const auto future=Bytes(storage.File());
    Check(storage.Load().state==configuration::StorageState::newer_schema && storage.Save({})!=0 &&
        Bytes(storage.File())==future,"Newer disk schema is preserved until explicit recovery");
    WriteRecord(storage.File(),unchanged);
    auto next=original; ++next.revision; next.design_selection=DesignSelection::shared_zigzag;
    Check(storage.Save(next)==0 && storage.Load().configuration==next && Bytes(storage.File())[4]==4,
        "First successful semantic mutation durably upgrades schema3 to schema4");
}
struct Observed {
    std::mutex mutex;
    std::condition_variable wake;
    settings::ConnectionState latest;
    void Add(settings::ConnectionState state) {
        std::lock_guard lock(mutex); latest=std::move(state); wake.notify_all();
    }
    template<class Predicate> settings::ConnectionState Wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock,std::chrono::seconds(15),[&]{return predicate(latest);}),
            "Actual Settings Session publishes expected real-pipe state");
        return latest;
    }
};
void NativeSessionPersistence(const std::filesystem::path& root) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr,executable,32768)) throw std::runtime_error("Cannot identify native peer");
    const auto directory=root/L"zigzag-session";
    const auto suffix=L"zigzag-"+std::to_wstring(GetCurrentProcessId());
    configuration::ConfigurationServer server({directory,suffix,executable}); server.Start();
    Observed observed;
    settings::SessionTransport transport;
    transport.exchange=[&](const Message& request,HANDLE) { return configuration::Exchange(server.Endpoint(),executable,request); };
    transport.start_host=[] { return false; };
    settings::Session session({true,directory,suffix},[&](auto state){observed.Add(std::move(state));},transport);
    auto initial=observed.Wait([](const auto& state){return state.connected && !state.pending && state.configuration_authoritative;});
    auto intent=initial.configuration;
    intent.design_selection=DesignSelection::shared_zigzag;
    const auto generation=session.Change(initial,intent);
    auto saved=observed.Wait([&](const auto& state){return !state.pending && state.settled_edit_generation>=generation;});
    Check(saved.status==Status::saved && saved.configuration.revision==1 &&
        saved.configuration.design_selection==DesignSelection::shared_zigzag &&
        configuration::Storage(directory).Load().configuration==saved.configuration,
        "Production Settings Session shared-mode acknowledgment follows the sole writer durable save");
    {
        configuration::Handle lock(CreateFileW(configuration::Storage(directory).File().c_str(),
            GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Check(bool(lock),"Inject real replacement failure after committed shared Zigzag");
        auto rejected=saved.configuration; rejected.design_selection=DesignSelection::by_app;
        const auto failedGeneration=session.Change(saved,rejected);
        auto failed=observed.Wait([&](const auto& state){return !state.pending && state.settled_edit_generation>=failedGeneration;});
        Check(failed.status==Status::storage_error && failed.configuration==saved.configuration &&
            server.Snapshot()==saved.configuration,"Failed mode save keeps the acknowledged Zigzag configuration and reports storage failure");
    }
    session.Stop(); server.Stop();
    configuration::ConfigurationServer restart({directory,suffix,executable});
    Check(restart.Snapshot()==saved.configuration,"Native server restart retains distinct shared mode and all manual controls");
    restart.Start();
    Message snapshot; snapshot.client=NewIdentity(); snapshot.request_id=1;
    auto current=configuration::Exchange(restart.Endpoint(),executable,snapshot);
    auto command=current; command.operation=Operation::mutate; command.request_id=2;
    command.configuration.design_selection=DesignSelection::by_app;
    auto changed=configuration::Exchange(restart.Endpoint(),executable,command);
    Check(changed.status==Status::saved && changed.configuration.revision==2 &&
        changed.configuration.design_selection==DesignSelection::by_app &&
        configuration::Exchange(restart.Endpoint(),executable,command)==changed,
        "By App remains distinct after restart; mode mutation retries are durable and idempotent");
    auto stale=command; stale.request_id=3; stale.configuration.design_selection=DesignSelection::shared_zigzag;
    Check(configuration::Exchange(restart.Endpoint(),executable,stale).status==Status::conflict,
        "Stale shared-mode intent cannot overwrite a newer By App commit");
    restart.Stop();
}
}
int RunIconModeTests(const std::filesystem::path& root) {
    checks=0;
    SaltAndPreview();
    Schema3Migration(root);
    NativeSessionPersistence(root);
    return static_cast<int>(checks);
}
