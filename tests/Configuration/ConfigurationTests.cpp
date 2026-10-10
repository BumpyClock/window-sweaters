#include "..\..\src\Configuration\ConfigurationServer.h"
#include "..\..\src\Configuration\Storage.h"
#include "..\..\src\Configuration\Pipe.h"
#include <aclapi.h>
#include <sddl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

using namespace window_sweaters;
using namespace window_sweaters::contracts;
using namespace window_sweaters::configuration;
int RunSessionTests(std::filesystem::path const& directory);
int RunAppearanceTests(std::filesystem::path const& directory);
int RunApplicationConfigurationTests(std::filesystem::path const& directory);
int RunAppsSessionTests(std::filesystem::path const& directory);
namespace {
int checks=0;
void Require(bool value,char const* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
std::filesystem::path Self() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768); return path;
}
Message SnapshotRequest(std::uint64_t id=1) {
    Message value; value.client=NewIdentity(); value.request_id=id; return value;
}
Message Mutation(Message const& snapshot,std::uint64_t id,float width,bool paused=false) {
    Message value=snapshot;
    value.operation=Operation::mutate; value.status=Status::saved; value.request_id=id;
    value.configuration.border_width_dip=width; value.configuration.paused=paused; return value;
}
std::vector<char> Bytes(std::filesystem::path const& path) {
    std::ifstream stream(path,std::ios::binary); return {std::istreambuf_iterator<char>(stream),{}};
}
Handle OpenPipe(std::wstring const& endpoint) {
    WaitNamedPipeW(endpoint.c_str(),io_timeout_ms);
    Handle pipe(CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    Require(bool(pipe),"Open real local pipe");
    DWORD mode=PIPE_READMODE_BYTE;
    Require(SetNamedPipeHandleState(pipe.get(),&mode,nullptr,nullptr)!=FALSE,"Length-framed byte read mode");
    return pipe;
}
void Malformed(std::wstring const& endpoint,std::vector<std::byte> bytes) {
    auto pipe=OpenPipe(endpoint);
    Require(Transfer(pipe.get(),bytes.data(),static_cast<DWORD>(bytes.size()),true),"Malformed payload reaches real pipe");
    std::array<std::byte,frame_size> response{};
    Require(!Transfer(pipe.get(),response.data(),frame_size,false),"Malformed payload rejected, no success response");
}
void SeparateProcessMutation(std::wstring const& endpoint) {
    auto image=Self();
    std::wstring command=L"\""+image.native()+L"\" --ipc-client \""+endpoint+L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,image.parent_path().c_str(),&startup,&process)!=FALSE,"Launch isolated native IPC client process");
    Handle thread(process.hThread),handle(process.hProcess);
    auto wait=WaitForSingleObject(handle.get(),15000);
    if (wait!=WAIT_OBJECT_0) TerminateProcess(handle.get(),99);
    Require(wait==WAIT_OBJECT_0,"Separate-process settings mutation finishes within deadline");
    DWORD result=99; GetExitCodeProcess(handle.get(),&result);
    Require(result==0,"Separate process receives persisted success acknowledgement");
}
Message CommitWithTransientRetry(std::wstring const& endpoint,Message const& request) {
    auto result=Exchange(endpoint,Self(),request);
    for (unsigned attempt=0;result.status==Status::storage_error && attempt<5;++attempt) {
        Require(result.configuration.revision==request.configuration.revision,
                "Transient filesystem failure does not advance committed revision");
        Sleep(25);
        result=Exchange(endpoint,Self(),request);
    }
    return result;
}
void StorageCases(std::filesystem::path const& directory) {
    Storage storage(directory/L"storage");
    auto missing=storage.Load();
    Require(missing.state==StorageState::ready && missing.configuration.border_width_dip==16 &&
            missing.configuration.revision==0 && !missing.configuration.paused &&
            missing.configuration.material_style==MaterialStyle::raised,"Missing storage uses shared raised-stockinette defaults");
    Configuration value; value.revision=42; value.border_width_dip=2; value.paused=true;
    value.material_style=MaterialStyle::plain;
    Require(storage.Save(value)==0,"Atomic first save");
    Require(storage.Load().configuration==value && storage.Load().configuration.material_style==MaterialStyle::plain,
            "Binary semantic settings survive reload without replacing explicit plain material with the raised default");
    value.revision=43; value.border_width_dip=64;
    Require(storage.Save(value)==0 && storage.Load().configuration==value,"Atomic replacement and boundary width");
    auto original=Bytes(storage.File());
    {
        std::ofstream file(storage.File(),std::ios::binary|std::ios::trunc); file<<"corrupt";
    }
    auto corrupt=Bytes(storage.File());
    Require(storage.Load().state==StorageState::corrupt,"Corrupt data is diagnosed");
    Require(storage.Save({})!=0 && Bytes(storage.File())==corrupt,"Corrupt data is never silently overwritten");
    Require(storage.Save({},true)==0,"Explicit reset succeeds");
    bool preserved=false;
    for (auto const& entry:std::filesystem::directory_iterator(storage.Directory()))
        if (entry.path().filename().wstring().find(L".preserved.")!=std::wstring::npos &&
            Bytes(entry.path())==corrupt) preserved=true;
    Require(preserved,"Explicit reset retains exact corrupt bytes");
    original[4]=static_cast<char>(configuration_schema+1);
    {
        std::ofstream file(storage.File(),std::ios::binary|std::ios::trunc);
        file.write(original.data(),static_cast<std::streamsize>(original.size()));
    }
    Require(storage.Load().state==StorageState::newer_schema,"Future schema distinguished before parsing");
    Require(storage.Save({})!=0 && Bytes(storage.File())==original,"Future schema preserved");
    Require(storage.Save({},true)==0,"Future schema reset is explicit");
    value.border_width_dip=std::numeric_limits<float>::quiet_NaN();
    Require(storage.Save(value)!=0 && storage.Load().configuration.border_width_dip==16,"NaN rejected at disk boundary");
}
void IpcCases(std::filesystem::path const& directory) {
    auto suffix=L"test-"+std::to_wstring(GetCurrentProcessId());
    ServerOptions options{directory/L"ipc",suffix,Self()};
    ConfigurationServer server(options); server.Start();
    auto endpoint=server.Endpoint();
    auto request=SnapshotRequest();
    auto snapshot=Exchange(endpoint,Self(),request);
    Require(snapshot.status==Status::saved && snapshot.configuration.revision==0,"Real pipe snapshot reports committed defaults");
    auto mutation=Mutation(snapshot,2,24.5f,true);
    auto saved=Exchange(endpoint,Self(),mutation);
    Require(saved.status==Status::saved && saved.configuration.revision==1 &&
            saved.configuration.border_width_dip==24.5f && saved.configuration.paused,"Real IPC saves width and pause");
    Require(Exchange(endpoint,Self(),mutation)==saved,"Committed retry is idempotent");
    auto reused=mutation; reused.configuration.border_width_dip=30;
    Require(Exchange(endpoint,Self(),reused).status==Status::invalid,"Request identity cannot mutate twice with different bytes");
    auto stale=Mutation(snapshot,3,33);
    Require(Exchange(endpoint,Self(),stale).status==Status::conflict,"Stale revision has explicit conflict");
    stale.configuration.revision=std::numeric_limits<std::uint64_t>::max();
    Require(Exchange(endpoint,Self(),stale).status==Status::conflict,"Oversized revision cannot overflow");
    auto wrong=Mutation(saved,4,34); wrong.server=NewIdentity();
    Require(Exchange(endpoint,Self(),wrong).status==Status::wrong_instance,"Old host instance request rejected");
    auto invalid=Mutation(saved,5,65); auto encoded=Encode(invalid);
    Malformed(endpoint,{encoded.begin(),encoded.end()});
    encoded=Encode(Mutation(saved,6,32)); encoded[4]=std::byte{0xff};
    Malformed(endpoint,{encoded.begin(),encoded.end()});
    encoded=Encode(Mutation(saved,7,32)); encoded[8]=std::byte{0xff};
    Malformed(endpoint,{encoded.begin(),encoded.end()});
    encoded=Encode(Mutation(saved,8,32)); encoded[68]=std::byte{2};
    Malformed(endpoint,{encoded.begin(),encoded.end()});
    Malformed(endpoint,std::vector<std::byte>(frame_size+1,std::byte{1}));
    Malformed(endpoint,std::vector<std::byte>(frame_size-1,std::byte{1}));
    encoded=Encode(Mutation(saved,9,std::numeric_limits<float>::infinity()));
    Malformed(endpoint,{encoded.begin(),encoded.end()});
    Require(server.Snapshot().revision==1,"Invalid frames leave committed state intact");
    {
        auto pipe=OpenPipe(endpoint);
        PACL dacl=nullptr; PSECURITY_DESCRIPTOR descriptor=nullptr;
        auto error=GetSecurityInfo(pipe.get(),SE_KERNEL_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&descriptor);
        Require(error==0 && dacl && dacl->AceCount==1,"Pipe uses explicit single-logon DACL");
        void* ace=nullptr; Require(GetAce(dacl,0,&ace)!=FALSE,"Logon ACE is readable");
        auto access=static_cast<ACCESS_ALLOWED_ACE*>(ace);
        LPWSTR sid=nullptr; ConvertSidToStringSidW(&access->SidStart,&sid);
        Require(sid && CurrentIdentity().logon_sid==sid,"Pipe DACL grants only intended logon");
        LocalFree(sid); LocalFree(descriptor);
        Require(VerifyPeer(pipe.get(),true,Self()),"Correct peer image/session/token accepted");
        Require(!VerifyPeer(pipe.get(),true,Self().parent_path()/L"OtherHost.exe"),"Wrong peer image rejected");
    }
    Storage storage(options.storage_directory);
    auto before=Bytes(storage.File());
    {
        Handle locked(CreateFileW(storage.File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Require(bool(locked),"Hold real file lock to inject replacement failure");
        auto failure=Mutation(saved,10,40);
        auto result=Exchange(endpoint,Self(),failure);
        Require(result.status==Status::storage_error && result.configuration==saved.configuration,"Failed replacement retains committed snapshot");
        Require(Bytes(storage.File())==before,"Failed replacement retains original disk bytes");
    }
    auto retry=Exchange(endpoint,Self(),Mutation(saved,10,40));
    Require(retry.status==Status::saved && retry.configuration.revision==2,"Failed request can retry after storage repair");
    auto a=server.SubmitLocal(false,12,2), b=server.SubmitLocal(true,13,2);
    auto first=a.get(),second=b.get();
    Require(first.status==Status::saved && first.configuration.revision==3 && second.status==Status::conflict,
            "Local and IPC mutations share ordered committed-revision validation");
    server.Stop();
    Require(storage.Load().configuration.revision==3 && storage.Load().configuration.border_width_dip==12,"Restart reads last successful ordered commit");
    server.Start();
    auto restarted=Exchange(endpoint,Self(),SnapshotRequest());
    Require(restarted.configuration.revision==3,"Restart publishes persisted snapshot");
    // Stop/Start on one service retains identity; constructing a new host service replaces it.
    server.Stop();
    ConfigurationServer replacement(options); replacement.Start();
    auto new_snapshot=Exchange(endpoint,Self(),SnapshotRequest());
    Require(new_snapshot.server!=snapshot.server,"New host process lifetime has new instance identity");
    Require(Exchange(endpoint,Self(),Mutation(retry,11,50)).status==Status::wrong_instance,"Reconnect cannot replay old-instance mutation");
    auto current=new_snapshot;
    auto oldest=Mutation(current,100,17);
    current=Exchange(endpoint,Self(),oldest);
    for (std::uint64_t id=101;id<357;++id) {
        auto command=Mutation(current,id,17);
        current=Exchange(endpoint,Self(),command);
        for (unsigned attempt=0;current.status==Status::storage_error && attempt<5;++attempt) {
            Require(current.configuration.revision==command.configuration.revision &&
                    storage.Load().configuration==current.configuration,"Transient write failure retains disk and committed revision");
            Sleep(25);
            current=Exchange(endpoint,Self(),command);
        }
        if (current.status!=Status::saved)
            throw std::runtime_error("Dedup stress mutation rejected: status "+std::to_string(static_cast<unsigned>(current.status))+
                                     ", request "+std::to_string(id)+", revision "+std::to_string(current.configuration.revision));
    }
    Require(current.configuration.revision==260,"Bounded dedup test commits expected revisions");
    auto expired=oldest; expired.configuration.revision=current.configuration.revision;
    Require(Exchange(endpoint,Self(),expired).status==Status::duplicate_expired,"Evicted duplicate cannot be applied with a new revision");
    SeparateProcessMutation(endpoint);
    Require(replacement.Snapshot().revision==261 && replacement.Snapshot().border_width_dip==18.5f &&
            !replacement.Snapshot().paused,"Authenticated different process updates host-owned snapshot");
    Require(storage.Load().configuration.revision==261 && storage.Load().configuration.border_width_dip==18.5f,
            "Separate-process acknowledgement follows actual durable replacement");
    replacement.Stop();
}
void RecoveryIpc(std::filesystem::path const& directory) {
    auto storage_dir=directory/L"recovery";
    std::filesystem::create_directories(storage_dir);
    { std::ofstream file(storage_dir/L"configuration.bin",std::ios::binary); file<<"keep me"; }
    ConfigurationServer server({storage_dir,L"recovery-"+std::to_wstring(GetCurrentProcessId()),Self()});
    server.Start();
    auto snapshot=Exchange(server.Endpoint(),Self(),SnapshotRequest());
    Require(snapshot.status==Status::recovery_required,"UI is told storage needs explicit recovery");
    auto request=Mutation(snapshot,2,20);
    Require(Exchange(server.Endpoint(),Self(),request).status==Status::recovery_required,"Mutation cannot overwrite corrupt config");
    request.operation=Operation::reset; request.request_id=3;
    auto reset=Exchange(server.Endpoint(),Self(),request);
    Require(reset.status==Status::saved && reset.configuration.revision==1 &&
            reset.configuration.border_width_dip==16,"Explicit IPC reset saves default revision");
    server.Stop();
    ConfigurationServer denied({directory/L"denied",L"denied-"+std::to_wstring(GetCurrentProcessId()),
                                Self().parent_path()/L"OtherSettings.exe"});
    denied.Start();
    bool peer_rejected=false;
    try { Exchange(denied.Endpoint(),Self(),SnapshotRequest()); } catch (...) { peer_rejected=true; }
    Require(peer_rejected,"Server rejects a client with unexpected executable identity");
    Require(denied.Snapshot().revision==0,"Unauthorized client cannot commit");
    denied.Stop();
}
void ClientLeaseCases(std::filesystem::path const& directory) {
    ConfigurationServer server({directory/L"leases",L"leases-"+std::to_wstring(GetCurrentProcessId()),Self()});
    server.Start();
    auto endpoint=server.Endpoint();
    auto original=Exchange(endpoint,Self(),SnapshotRequest());
    auto old_mutation=Mutation(original,2,19);
    auto saved=CommitWithTransientRetry(endpoint,old_mutation);
    Require(saved.status==Status::saved && saved.configuration.revision==1,"Original admitted client commits");
    for (unsigned i=0;i<maximum_client_leases+16;++i) {
        auto opened=Exchange(endpoint,Self(),SnapshotRequest());
        auto result=CommitWithTransientRetry(endpoint,Mutation(opened,2,21));
        Require(result.status==Status::saved && result.configuration.revision==i+2,
                "Repeated settings reopen admits and saves beyond lifetime client capacity");
    }
    auto revision=server.Snapshot().revision;
    Require(Exchange(endpoint,Self(),old_mutation).status==Status::client_expired,
            "Retired committed client retry is explicitly expired, not silently applied or readmitted");
    auto forged=old_mutation; forged.configuration.revision=revision;
    Require(Exchange(endpoint,Self(),forged).status==Status::client_expired && server.Snapshot().revision==revision,
            "Retired token with current revision cannot revive an old client mutation");
    Message refresh=original; refresh.operation=Operation::snapshot; refresh.request_id=3;
    Require(Exchange(endpoint,Self(),refresh).status==Status::client_expired,
            "Snapshot carrying retired lease cannot mint replacement admission");
    auto reopened_request=SnapshotRequest(); reopened_request.client=original.client;
    auto reopened=Exchange(endpoint,Self(),reopened_request);
    Require(reopened.client_lease!=original.client_lease && reopened.client_lease!=0,
            "Explicit new handshake issues a distinct server lease for same client GUID");
    auto mismatched=Mutation(reopened,2,22); mismatched.client=NewIdentity();
    Require(Exchange(endpoint,Self(),mismatched).status==Status::client_expired,
            "Lease admission binds to exact client identity");
    Require(CommitWithTransientRetry(endpoint,Mutation(reopened,2,22)).status==Status::saved,
            "Explicit new admission remains usable after retirement");
    Storage storage(directory/L"leases");
    {
        Handle locked(CreateFileW(storage.File().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        Require(bool(locked),"Lock persistent file for repeated failed-client writes");
        for (unsigned i=0;i<maximum_client_leases+8;++i) {
            auto client=Exchange(endpoint,Self(),SnapshotRequest());
            auto result=Exchange(endpoint,Self(),Mutation(client,2,30));
            Require(result.status==Status::storage_error && result.configuration.revision==revision+1,
                    "Failed writes across new clients do not alter committed revision");
        }
    }
    auto healthy=Exchange(endpoint,Self(),SnapshotRequest());
    Require(CommitWithTransientRetry(endpoint,Mutation(healthy,2,31)).status==Status::saved,
            "Failed clients cannot permanently exhaust mutation admission");
    auto local=server.SubmitLocal(true,32,revision+2).get();
    Require(local.status==Status::saved,"Bounded client retirement never exhausts host-local mutation");
    auto prior_instance=healthy.server;
    server.Stop();
    ConfigurationServer restarted({directory/L"leases",L"leases-"+std::to_wstring(GetCurrentProcessId()),Self()});
    restarted.Start();
    auto fresh=Exchange(endpoint,Self(),SnapshotRequest());
    auto replay=Mutation(fresh,2,33); replay.server=prior_instance;
    Require(Exchange(endpoint,Self(),replay).status==Status::wrong_instance,
            "Numeric lease token is bound to host lifetime even if sequence repeats after restart");
    restarted.Stop();
}
}
int wmain(int argc,wchar_t** argv) {
    if (argc==3 && std::wstring_view(argv[1])==L"--ipc-client") {
        try {
            auto snapshot=Exchange(argv[2],Self(),SnapshotRequest());
            auto result=Exchange(argv[2],Self(),Mutation(snapshot,2,18.5f));
            return result.status==Status::saved && result.configuration.revision==snapshot.configuration.revision+1 &&
                result.configuration.border_width_dip==18.5f && !result.configuration.paused?0:1;
        } catch (...) { return 2; }
    }
    auto directory=std::filesystem::current_path()/L"tests"/L"Configuration"/
        (L".run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        if (argc==2 && std::wstring_view(argv[1])==L"--session-only")
            checks+=RunSessionTests(directory);
        else if (argc==2 && std::wstring_view(argv[1])==L"--appearance-only")
            checks+=RunAppearanceTests(directory);
        else if (argc==2 && std::wstring_view(argv[1])==L"--apps-only")
            checks+=RunApplicationConfigurationTests(directory)+RunAppsSessionTests(directory);
        else {
            StorageCases(directory); IpcCases(directory); RecoveryIpc(directory); ClientLeaseCases(directory);
            checks+=RunSessionTests(directory); checks+=RunAppearanceTests(directory);
            checks+=RunApplicationConfigurationTests(directory);
            checks+=RunAppsSessionTests(directory);
        }
        std::filesystem::remove_all(directory);
        std::cout<<"PASS "<<checks<<" configuration/storage/real-pipe assertions\n"; return 0;
    } catch (std::exception const& error) {
        std::cerr<<"FAIL "<<error.what()<<" after "<<checks<<" assertions\n";
        std::filesystem::remove_all(directory); return 1;
    }
}
