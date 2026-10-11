#include "..\..\src\Settings\Preview.h"
#include "..\..\src\Configuration\ConfigurationServer.h"
#include "..\..\src\Configuration\Storage.h"
#include "..\..\src\Configuration\Pipe.h"
#include <bit>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <limits>
#include <set>
#include <thread>

using namespace window_sweaters;
using namespace window_sweaters::contracts;
using namespace window_sweaters::configuration;
namespace {
int checks=0;
void Check(bool condition,char const* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
std::filesystem::path Self() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768); return path;
}
std::vector<char> Bytes(std::filesystem::path const& path) {
    std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}};
}
void Write(std::filesystem::path const& path,std::vector<char> const& bytes) {
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
}
void Put(std::vector<char>& bytes,std::size_t offset,std::uint64_t value,std::size_t count) {
    for (std::size_t i=0;i<count;++i) bytes[offset+i]=static_cast<char>((value>>(8*i))&255);
}
void Checksum(std::vector<char>& bytes) {
    std::uint32_t crc=0xffffffff;
    for (std::size_t i=0;i<bytes.size()-4;++i) {
        crc^=static_cast<unsigned char>(bytes[i]);
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));
    }
    Put(bytes,bytes.size()-4,static_cast<std::uint32_t>(~crc),4);
}
Configuration Original(char const* id) {
    Configuration value; value.design_selection=DesignSelection::original; value.design_id=id;
    value.border_width_dip=24.5f; value.stitch_rows=4.5f; value.yarn_argb=0xffe65b7au;
    value.stitch_style=StitchStyle::garter; value.material_style=MaterialStyle::plain;
    return value;
}
void DomainAndFrames() {
    Check(configuration_schema==7 && protocol_version==8 && frame_size==176,"Appearance, eligibility, imports, startup and base override use bounded schema7/protocol8");
    auto defaults=settings::MakeAppearance(Configuration{});
    Check(Configuration{}.material_style==MaterialStyle::raised &&
          defaults.material==knitting::Material::Raised && defaults.stitch==knitting::Stitch::Stockinette,
          "Fresh semantic defaults map directly to production raised stockinette");
    Check(designs::ws_design_theme_count()>0,"Catalogue applications are available");
    std::set<std::string> ids;
    for (std::size_t i=0;i<designs::ws_design_theme_count();++i) {
        auto theme=designs::ws_design_theme_at(i);
        Check(theme && ids.insert(theme->id).second,"Every original theme has a unique stable ID");
        auto value=Original(theme->id);
        Check(Valid(value),"All original IDs validate without relying on catalogue indexes");
        Message frame; frame.operation=Operation::mutate; frame.client=NewIdentity(); frame.server=NewIdentity();
        frame.client_lease=137; frame.request_id=900; frame.configuration=value;
        frame.configuration.revision=81;
        Message decoded;
        Check(Decode(Encode(frame),decoded) && decoded==frame,"All appearance fields, lease and IDs survive bounded wire encoding");
        auto spec=settings::MakeAppearance(value);
        auto source=designs::ws_design_chart_by_name(theme->chart_name);
        Check(source && spec.chart && spec.chart->semanticId==theme->id &&
              spec.chart->width==source->width && spec.chart->height==source->height &&
              spec.chart->appearance.flags==source->flags,"Original appearance owns the actual authored chart and all flags");
        Check(spec.gauge.rows==4.5f && spec.baseArgb==0xffe65b7au &&
              spec.stitch==knitting::Stitch::Stockinette,"Original chart preserves requested gauge and color with authored stitches");
        bool cells=true;
        for (std::uint32_t y=0;y<source->height;++y)
            for (std::uint32_t x=0;x<source->width;++x) {
                auto expected=designs::ws_design_cell_argb(source,x,y);
                cells=cells && spec.chart->argbCells[std::size_t(y)*source->width+x]==expected;
            }
        Check(cells,"Preview extracts authored cells rather than substituting plain knitting");
    }
    Check(ids.contains("finder") && ids.contains("microsoft-teams") && ids.contains("vs-code"),
          "Persisted IDs include literal original application identities");
    auto invalid=Original("unknown-application");
    Check(!Valid(invalid),"Unrecognized original design is rejected");
    invalid=Original(""); Check(!Valid(invalid),"Original mode requires a stable ID");
    invalid=Original("finder"); invalid.design_id=std::string(64,'a');
    Check(!Valid(invalid),"Overlong design ID is rejected before fixed-buffer writes");
    bool throws=false;
    try { Message message; message.configuration=invalid; Encode(message); } catch (std::invalid_argument const&) { throws=true; }
    Check(throws,"Encoding cannot truncate an overlong ID");
    invalid=Original("finder"); invalid.design_id.append("\0suffix",7);
    Check(!Valid(invalid),"Embedded NUL cannot alias a known theme");
    for (auto style:{StitchStyle::stockinette,StitchStyle::rib,StitchStyle::garter}) {
        Configuration value; value.stitch_style=style;
        Check(Valid(value) && settings::MakeAppearance(value).stitch==static_cast<knitting::Stitch>(style),
              "All plain stitch styles are supported semantic values");
    }
    Configuration bad;
    bad.design_selection=static_cast<DesignSelection>(4); Check(!Valid(bad),"Unsupported selection cannot auto-upgrade");
    bad={}; bad.design_id="finder"; Check(!Valid(bad),"Plain selection cannot silently carry an original ID");
    bad={}; bad.stitch_style=static_cast<StitchStyle>(3); Check(!Valid(bad),"Unsupported stitch is rejected");
    bad={}; bad.material_style=static_cast<MaterialStyle>(2); Check(!Valid(bad),"Unsupported material is rejected");
    bad={}; bad.schema=1; Check(!Valid(bad),"Legacy schema is migrated only at storage boundary");
    bad={}; bad.yarn_argb=0x7f112233; Check(!Valid(bad),"Translucent yarn is rejected rather than silently rendered opaque");
    for (float rows:{1.49f,12.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        bad={}; bad.stitch_rows=rows; Check(!Valid(bad),"Invalid gauge is rejected rather than clamped");
    }
    for (float rows:{1.5f,12.f}) { bad={}; bad.stitch_rows=rows; Check(Valid(bad),"Gauge endpoints are valid"); }
    auto request=Message{}; request.configuration=Original("finder"); auto bytes=Encode(request);
    Message output;
    bytes[108]=std::byte{64}; Check(!Decode(bytes,output),"Malformed declared design length is rejected");
    bytes=Encode(request); bytes[118]=std::byte{'x'}; Check(!Decode(bytes,output),"Nonzero fixed-ID padding is rejected");
    bytes=Encode(request); bytes[112]=std::byte{0}; Check(!Decode(bytes,output),"Embedded-NUL wire ID is rejected");
    bytes=Encode(request); bytes[112]=std::byte{0xff}; Check(!Decode(bytes,output),"Unsupported UTF-8 ID cannot alias authored ASCII IDs");
    bytes=Encode(request); bytes[88]=std::byte{4}; Check(!Decode(bytes,output),"Unsupported wire design mode is rejected");
    bytes=Encode(request); bytes[92]=std::byte{7}; Check(!Decode(bytes,output),"Unsupported wire stitch is rejected");
    bytes=Encode(request); bytes[96]=std::byte{9}; Check(!Decode(bytes,output),"Unsupported wire material is rejected");
}
void Migration(std::filesystem::path const& root) {
    Storage storage(root/L"migration"); std::filesystem::create_directories(storage.Directory());
    std::vector<char> legacy(32);
    Put(legacy,0,0x31435357,4); Put(legacy,4,1,4); Put(legacy,8,41,8);
    Put(legacy,16,1,4); Put(legacy,20,std::bit_cast<std::uint32_t>(21.5f),4); Checksum(legacy);
    Write(storage.File(),legacy);
    auto expected=Configuration{}; expected.revision=41; expected.paused=true; expected.border_width_dip=21.5f;
    Check(storage.Load().state==StorageState::ready && storage.Load().configuration==expected,
          "Schema1 migration retains revision, pause and width with renderer-compatible appearance defaults");
    Check(storage.Load().configuration.material_style==MaterialStyle::raised,
          "Legacy migration adopts raised material only because schema1 had no material choice");
    Check(Bytes(storage.File())==legacy,"Read-only migration does not rewrite legacy settings");
    auto suffix=L"migration-"+std::to_wstring(GetCurrentProcessId());
    ConfigurationServer server({storage.Directory(),suffix,Self()}); server.Start();
    Check(server.Snapshot()==expected && Bytes(storage.File())==legacy,"Host startup publishes in-memory migration without an implicit write");
    {
        Handle lock(CreateFileW(storage.File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Check(bool(lock),"Legacy disk replacement is locked to test failed migration commit");
        auto failure=server.SubmitLocal(false,31,41).get();
        Check(failure.status==Status::storage_error && failure.error!=0 && failure.configuration==expected &&
              Bytes(storage.File())==legacy,"Failed atomic migration preserves legacy bytes and reports actual storage error");
    }
    auto saved=server.SubmitLocal(false,31,41).get();
    Check(saved.status==Status::saved && saved.configuration.revision==42 && !saved.configuration.paused &&
          Bytes(storage.File()).size()>120,"First acknowledged mutation atomically upgrades to length-framed schema3");
    server.Stop();
    ConfigurationServer restarted({storage.Directory(),suffix,Self()});
    Check(restarted.Snapshot()==saved.configuration,"Restart sees the acknowledged schema3 migration");
    auto invalid=legacy; Put(invalid,20,std::bit_cast<std::uint32_t>(65.f),4); Checksum(invalid);
    Write(storage.File(),invalid);
    Check(storage.Load().state==StorageState::corrupt && Bytes(storage.File())==invalid &&
          storage.Save({})!=0,"Invalid old values are preserved, not blanket-upgraded");
    auto future=legacy; Put(future,4,configuration_schema+1,4); Write(storage.File(),future);
    Check(storage.Load().state==StorageState::newer_schema && storage.Save({})!=0 &&
          Bytes(storage.File())==future,"Newer settings remain preserved and require explicit reset");
}
void Transactions(std::filesystem::path const& root) {
    auto directory=root/L"appearance-ipc";
    auto suffix=L"appearance-"+std::to_wstring(GetCurrentProcessId());
    ConfigurationServer server({directory,suffix,Self()}); server.Start();
    Message request; request.client=NewIdentity(); request.request_id=1;
    auto snapshot=Exchange(server.Endpoint(),Self(),request);
    auto command=snapshot; command.operation=Operation::mutate; command.request_id=2;
    command.configuration=Original("spotify");
    auto saved=Exchange(server.Endpoint(),Self(),command);
    auto expected=command.configuration; expected.revision=1;
    Check(saved.status==Status::saved && saved.configuration==expected,"Full appearance IPC acknowledgment follows durable semantic save");
    Storage storage(directory);
    Check(storage.Load().configuration==expected && Bytes(storage.File()).size()>120,"All appearance values persist in bounded schema3 disk format");
    Check(Exchange(server.Endpoint(),Self(),command)==saved,"Full appearance retry remains idempotent");
    auto reused=command; reused.configuration.yarn_argb=0xff001122;
    Check(Exchange(server.Endpoint(),Self(),reused).status==Status::invalid,"Request dedup includes color and entire appearance");
    auto local=server.SubmitLocal(true,40,1).get();
    expected.paused=true; expected.border_width_dip=40; expected.revision=2;
    Check(local.status==Status::saved && local.configuration==expected,"Legacy pause/width host wrapper preserves every current appearance field");
    auto stale=server.SubmitLocal(false,20,1).get();
    Check(stale.status==Status::conflict && stale.configuration==expected,"Legacy wrapper cannot overwrite newer appearance under stale revision");
    auto next=Original("notes"); next.revision=2; next.stitch_rows=8; next.yarn_argb=0xff228833;
    {
        Handle lock(CreateFileW(storage.File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        auto original=Bytes(storage.File());
        auto failure=server.SubmitLocal(next).get();
        Check(failure.status==Status::storage_error && failure.error!=0 && failure.configuration==expected &&
              server.Snapshot()==expected && Bytes(storage.File())==original,
              "Appearance failed-write acknowledgment preserves committed and disk values");
    }
    auto changed=server.SubmitLocal(next).get(); next.revision=3;
    Check(changed.status==Status::saved && changed.configuration==next,"Full local overload commits appearance with expected revision");
    auto invalid=next; invalid.design_id="not-authored";
    Check(server.SubmitLocal(invalid).get().status==Status::invalid && server.Snapshot()==next,
          "Host validation rejects unknown design without changing the current appearance");
    server.Stop();
    ConfigurationServer replacement({directory,suffix,Self()}); replacement.Start();
    Check(replacement.Snapshot()==next,"Restart retains complete original design, style, gauge and yarn color");
    Check(Exchange(replacement.Endpoint(),Self(),command).status==Status::wrong_instance,
          "Appearance retry cannot cross server-instance restart");
    auto malformed=Bytes(storage.File()); Put(malformed,12+108,64,4); Checksum(malformed);
    Write(storage.File(),malformed);
    Check(storage.Load().state==StorageState::corrupt && storage.Load().configuration==Configuration{},
          "Malformed schema3 appearance ID is rejected to valid recovery defaults, not leaked into IPC");
    malformed=Bytes(storage.File()); Put(malformed,12+108,5,4); malformed[12+112]='x'; Checksum(malformed);
    Write(storage.File(),malformed);
    Check(storage.Load().state==StorageState::corrupt,"Unknown or noncanonical disk ID is rejected even with a correct CRC");
    replacement.Stop();
}
struct PreviewViews {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<settings::PreviewResult const>> values;
    void Add(std::shared_ptr<settings::PreviewResult const> value) {
        std::lock_guard lock(mutex); values.push_back(std::move(value)); wake.notify_all();
    }
    std::shared_ptr<settings::PreviewResult const> Wait(std::uint64_t generation) {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock,std::chrono::seconds(20),[&] {
            return !values.empty() && values.back()->generation==generation;
        }),"Bounded preview worker publishes latest generation");
        return values.back();
    }
};
void ProductionPreview() {
    PreviewViews views;
    settings::Preview worker([&](auto value){views.Add(std::move(value));});
    knitting::TileCache cache;
    // The 16 DIP band's 8 DIP drop-shadow margin comes out of the 400x140 content.
    rendering::KnitFrameSpec frame; frame.widthDip=384; frame.heightDip=124;
    for (std::size_t i=0;i<designs::ws_design_theme_count();++i) {
        auto theme=designs::ws_design_theme_at(i);
        auto value=Original(theme->id); value.border_width_dip=16; value.stitch_rows=6; value.yarn_argb=theme->base_argb;
        auto generation=worker.Submit(value); auto result=views.Wait(generation);
        Check(result->error.empty() && result->configuration==value && result->image.width==432 &&
              result->image.height==172 && result->image.pixels.size()==432u*172u,
              "All original designs render into bounded native BGRA preview pixels on the MTA worker");
        frame.bandDip=16;
        auto tile=settings::MakeAppearance(value); tile.rasterScale=2;
        auto actual=rendering::KnitRaster::RenderFrame(frame,tile,cache);
        Check(actual.pixels==result->image.pixels,"Preview displays the exact production KnitRaster pieces, not an approximate texture");
        auto plain=value; plain.design_selection=DesignSelection::plain; plain.design_id.clear();
        auto plain_tile=settings::MakeAppearance(plain); plain_tile.rasterScale=2;
        auto baseline=rendering::KnitRaster::RenderFrame(frame,plain_tile,cache);
        Check(baseline.pixels!=result->image.pixels,"Original-design preview is not a fake plain tile");
    }
    worker.Stop();
}
void PreviewCoalescing() {
    PreviewViews views;
    std::mutex mutex; std::condition_variable wake;
    bool entered=false,released=false;
    unsigned renders=0; bool mta=true;
    settings::Preview worker([&](auto value){views.Add(std::move(value));},[&](Configuration const&) {
        APTTYPE type; APTTYPEQUALIFIER qualifier;
        mta=mta && SUCCEEDED(CoGetApartmentType(&type,&qualifier)) && type==APTTYPE_MTA;
        std::unique_lock lock(mutex);
        if (++renders==1) {
            entered=true; wake.notify_all();
            if (!wake.wait_for(lock,std::chrono::seconds(10),[&]{return released;}))
                throw std::runtime_error("Preview test gate timed out");
        }
                return knitting::RasterImage{1,1,4,{0xff123456}};
    });
    worker.Submit(Configuration{});
    {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock,std::chrono::seconds(10),[&]{return entered;}),"Preview starts bounded first worker job");
    }
    std::uint64_t latest=0; Configuration final;
    for (unsigned i=0;i<70;++i) {
        final=Original(i%2?"finder":"spotify"); final.border_width_dip=2.f+.5f*static_cast<float>(i);
        final.stitch_rows=1.5f+.5f*static_cast<float>(i%22); final.yarn_argb=0xff000000u+i*0x10101u;
        latest=worker.Submit(final);
    }
    { std::lock_guard lock(mutex); released=true; wake.notify_all(); }
    auto result=views.Wait(latest); worker.Stop();
    Check(renders==2 && views.values.size()==1 && result->configuration==final && mta,
          "70 color/gauge/width/design changes coalesce to one latest immutable MTA preview without publishing stale work");
}
void ProductionSliderBounds() {
    PreviewViews views;
    settings::Preview worker([&](auto value){views.Add(std::move(value));});
    for (float width:{2.f,64.f}) {
        for (float rows:{1.5f,12.f}) {
            for (char const* id:{"","finder","spotify","notes"}) {
                Configuration configuration;
                if (*id) configuration=Original(id);
                configuration.border_width_dip=width; configuration.stitch_rows=rows;
                configuration.yarn_argb=0xff090df0;
                auto result=views.Wait(worker.Submit(configuration));
                Check(result->error.empty() && result->configuration==configuration &&
                      result->image.width==400+static_cast<unsigned>(2*width) &&
                      result->image.height==140+static_cast<unsigned>(2*width) &&
                      result->image.ByteSize()<=528u*268u*4u,
                      "Small and large slider limits produce bounded real plain/pattern/raised/cuff preview output");
            }
        }
    }
    worker.Stop();
}
}
int RunAppearanceTests(std::filesystem::path const& directory) {
    checks=0;
    DomainAndFrames(); Migration(directory); Transactions(directory);
    auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if (FAILED(initialized)) throw std::runtime_error("Cannot initialize production preview test apartment");
    try { ProductionPreview(); ProductionSliderBounds(); PreviewCoalescing(); }
    catch (...) { CoUninitialize(); throw; }
    CoUninitialize(); return checks;
}
