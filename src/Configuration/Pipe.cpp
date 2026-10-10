#include "Pipe.h"
#include <sddl.h>
#include <vector>
#include <cwctype>

namespace window_sweaters::configuration {
namespace {
std::vector<std::byte> TokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS type) {
    DWORD size=0;
    GetTokenInformation(token,type,nullptr,0,&size);
    if (!size || size>65536) throw std::runtime_error("Invalid peer token");
    std::vector<std::byte> bytes(size);
    if (!GetTokenInformation(token,type,bytes.data(),size,&size)) throw std::runtime_error("Cannot read peer token");
    return bytes;
}
std::wstring SidString(PSID sid) {
    LPWSTR text=nullptr;
    if (!ConvertSidToStringSidW(sid,&text)) throw std::runtime_error("Cannot read identity SID");
    std::wstring value(text); LocalFree(text); return value;
}
EndpointIdentity IdentityFromToken(HANDLE token) {
    EndpointIdentity result;
    auto user=TokenInfo(token,TokenUser);
    result.user_sid=SidString(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    auto groups=TokenInfo(token,TokenGroups);
    auto native=reinterpret_cast<TOKEN_GROUPS*>(groups.data());
    for (DWORD i=0;i<native->GroupCount;++i)
        if ((native->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID)
            result.logon_sid=SidString(native->Groups[i].Sid);
    if (result.logon_sid.empty()) throw std::runtime_error("Interactive logon SID required");
    auto session=TokenInfo(token,TokenSessionId);
    result.session=*reinterpret_cast<DWORD*>(session.data());
    auto integrity=TokenInfo(token,TokenIntegrityLevel);
    auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    result.integrity=*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
    auto statistics=TokenInfo(token,TokenStatistics);
    result.authentication=reinterpret_cast<TOKEN_STATISTICS*>(statistics.data())->AuthenticationId;
    return result;
}
bool SameIdentity(EndpointIdentity const& a, EndpointIdentity const& b) {
    return a.user_sid==b.user_sid && a.logon_sid==b.logon_sid && a.session==b.session &&
        a.integrity==b.integrity && a.authentication.HighPart==b.authentication.HighPart &&
        a.authentication.LowPart==b.authentication.LowPart;
}
bool WaitIo(HANDLE pipe, OVERLAPPED& io, HANDLE stop, DWORD& transferred) noexcept {
    HANDLE events[]={io.hEvent,stop};
    auto result=WaitForMultipleObjects(stop?2:1,events,FALSE,contracts::io_timeout_ms);
    if (result!=WAIT_OBJECT_0) {
        CancelIoEx(pipe,&io);
        GetOverlappedResult(pipe,&io,&transferred,TRUE);
        return false;
    }
    return GetOverlappedResult(pipe,&io,&transferred,FALSE)!=FALSE;
}
}
EndpointIdentity CurrentIdentity() {
    Handle token;
    HANDLE raw=nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)) throw std::runtime_error("Cannot read process token");
    token=Handle(raw); return IdentityFromToken(token.get());
}
std::wstring EndpointName(std::wstring const& suffix) {
    if (suffix.size()>48) throw std::invalid_argument("Test endpoint suffix too long");
    for (auto c:suffix)
        if (!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_'))
            throw std::invalid_argument("Invalid test endpoint suffix");
    auto identity=CurrentIdentity();
    return L"\\\\.\\pipe\\WindowSweaters."+identity.user_sid+L"."+identity.logon_sid+
        (suffix.empty()?L"":L"."+suffix);
}
std::wstring PipeSecuritySddl() {
    // The logon SID denotes one user's one authenticated logon, unlike a user-only ACE.
    return L"D:P(A;;GRGW;;;"+CurrentIdentity().logon_sid+L")";
}
std::wstring TestEndpointSuffix(std::filesystem::path const& storage_directory) {
    auto path=std::filesystem::weakly_canonical(std::filesystem::absolute(storage_directory)).native();
    std::uint64_t hash=14695981039346656037ull;
    for (wchar_t c:path) {
        auto lower=static_cast<std::uint16_t>(towlower(c));
        hash^=lower&255; hash*=1099511628211ull; hash^=lower>>8; hash*=1099511628211ull;
    }
    return L"test-"+std::to_wstring(hash);
}
bool VerifyPeer(HANDLE pipe, bool server_peer, std::filesystem::path const& expected_image) noexcept {
    try {
        auto own=CurrentIdentity();
        ULONG pid=0;
        if (!(server_peer?GetNamedPipeServerProcessId(pipe,&pid):GetNamedPipeClientProcessId(pipe,&pid))) return false;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        if (!process) return false;
        DWORD session=0;
        if (!ProcessIdToSessionId(pid,&session) || session!=own.session) return false;
        HANDLE token_raw=nullptr;
        if (!OpenProcessToken(process.get(),TOKEN_QUERY,&token_raw)) return false;
        Handle token(token_raw);
        if (!SameIdentity(own,IdentityFromToken(token.get()))) return false;
        if (!server_peer) {
            if (!ImpersonateNamedPipeClient(pipe)) return false;
            HANDLE impersonation_raw=nullptr;
            bool opened=OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&impersonation_raw)!=FALSE;
            Handle impersonation(impersonation_raw);
            bool matches=false;
            try { matches=opened && SameIdentity(own,IdentityFromToken(impersonation.get())); }
            catch (...) { matches=false; }
            if (!RevertToSelf()) std::terminate();
            if (!matches) return false;
        }
        wchar_t image[32768]{};
        DWORD size=static_cast<DWORD>(std::size(image));
        if (!QueryFullProcessImageNameW(process.get(),0,image,&size) || expected_image.empty()) return false;
        auto expected=std::filesystem::weakly_canonical(expected_image).native();
        auto actual=std::filesystem::weakly_canonical(std::filesystem::path(image)).native();
        return CompareStringOrdinal(actual.c_str(),-1,expected.c_str(),-1,TRUE)==CSTR_EQUAL;
    } catch (...) { return false; }
}
bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write, HANDLE stop) noexcept {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if (!event) return false;
    OVERLAPPED io{}; io.hEvent=event.get();
    DWORD transferred=0;
    BOOL started=write?WriteFile(pipe,bytes,size,&transferred,&io):ReadFile(pipe,bytes,size,&transferred,&io);
    if (!started && (GetLastError()!=ERROR_IO_PENDING || !WaitIo(pipe,io,stop,transferred))) return false;
    return transferred==size;
}
bool Connect(HANDLE pipe, HANDLE stop) noexcept {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if (!event) return false;
    OVERLAPPED io{}; io.hEvent=event.get();
    if (ConnectNamedPipe(pipe,&io)) return true;
    auto error=GetLastError();
    if (error==ERROR_PIPE_CONNECTED) return true;
    DWORD transferred=0;
    return error==ERROR_IO_PENDING && WaitIo(pipe,io,stop,transferred);
}
contracts::Message Exchange(std::wstring const& endpoint, std::filesystem::path const& host_path,
                            contracts::Message const& request) {
    if (!WaitNamedPipeW(endpoint.c_str(),contracts::io_timeout_ms)) throw std::runtime_error("Host is disconnected");
    Handle pipe(CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
    if (!pipe || !VerifyPeer(pipe.get(),true,host_path)) throw std::runtime_error("Host identity verification failed");
    DWORD mode=PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(),&mode,nullptr,nullptr)) throw std::runtime_error("Cannot configure settings pipe");
    auto bytes=contracts::Encode(request);
    if (!Transfer(pipe.get(),bytes.data(),static_cast<DWORD>(bytes.size()),true))
        throw std::runtime_error("Settings request write failed");
    if (!Transfer(pipe.get(),bytes.data(),static_cast<DWORD>(bytes.size()),false))
        throw std::runtime_error("Settings response timed out or was truncated");
    contracts::Message response;
    if (!contracts::Decode(bytes,response) || response.operation!=contracts::Operation::response ||
        response.request_id!=request.request_id || response.client!=request.client ||
        contracts::EmptyIdentity(response.server) ||
        (request.operation!=contracts::Operation::snapshot && response.server!=request.server &&
         response.status!=contracts::Status::wrong_instance) ||
        (request.operation!=contracts::Operation::snapshot && response.client_lease!=request.client_lease) ||
        (request.operation==contracts::Operation::snapshot && response.status==contracts::Status::saved &&
         response.client_lease==0))
        throw std::runtime_error("Invalid host response");
    // Receipt lets the server disconnect without discarding its buffered response.
    std::byte receipt{0xac};
    Transfer(pipe.get(),&receipt,1,true);
    return response;
}
std::filesystem::path SiblingExecutable(wchar_t const* name) {
    wchar_t path[32768]{};
    DWORD size=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    if (!size || size>=std::size(path)) throw std::runtime_error("Cannot locate companion executable");
    return std::filesystem::path(path).parent_path()/name;
}
}
