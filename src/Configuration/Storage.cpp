#include "Storage.h"
#include "Pipe.h"
#include <array>
#include <bit>
#include <shlobj.h>

namespace window_sweaters::configuration {
namespace {
constexpr std::size_t disk_size = 32;
std::uint32_t Crc(std::span<std::byte const> bytes) noexcept {
    std::uint32_t crc=0xffffffff;
    for (auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int i=0;i<8;++i) crc = (crc>>1) ^ (0xedb88320 & (0u-(crc&1)));
    }
    return ~crc;
}
void Put(std::array<std::byte,disk_size>& bytes, std::size_t offset, std::uint64_t value, std::size_t count) {
    for (std::size_t i=0;i<count;++i) bytes[offset+i]=std::byte((value>>(8*i))&255);
}
std::uint64_t Get(std::array<std::byte,disk_size> const& bytes, std::size_t offset, std::size_t count) {
    std::uint64_t value=0;
    for (std::size_t i=0;i<count;++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(8*i);
    return value;
}
std::wstring UniqueSuffix() {
    auto identity=contracts::NewIdentity();
    constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring text;
    for (auto byte:identity) { text+=hex[byte>>4]; text+=hex[byte&15]; }
    return text;
}
}
std::filesystem::path DefaultStorageDirectory() {
    PWSTR path=nullptr;
    auto hr=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&path);
    if (FAILED(hr)) throw std::runtime_error("Cannot locate local application data");
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result/L"WindowSweaters";
}
Storage::Storage(std::filesystem::path directory)
    : directory_(directory.empty()?DefaultStorageDirectory():std::filesystem::absolute(directory).lexically_normal()) {}
LoadedConfiguration Storage::Load() const noexcept {
    LoadedConfiguration result;
    try {
        Handle file(CreateFileW(File().c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if (!file) {
            result.error=GetLastError();
            if (result.error==ERROR_FILE_NOT_FOUND || result.error==ERROR_PATH_NOT_FOUND) result.error=0;
            else result.state=StorageState::io_error;
            return result;
        }
        LARGE_INTEGER size{};
        std::array<std::byte,disk_size> bytes{};
        DWORD read=0;
        if (!GetFileSizeEx(file.get(),&size) || !ReadFile(file.get(),bytes.data(),disk_size,&read,nullptr)) {
            result.state=StorageState::io_error; result.error=GetLastError(); return result;
        }
        if (read>=8 && Get(bytes,0,4)==0x31435357 && Get(bytes,4,4)>contracts::configuration_schema) {
            result.state=StorageState::newer_schema; return result;
        }
        if (size.QuadPart!=disk_size || read!=disk_size || Get(bytes,0,4)!=0x31435357 ||
            Get(bytes,4,4)!=contracts::configuration_schema || Get(bytes,16,4)>1 ||
            Get(bytes,24,4)!=0 || Get(bytes,28,4)!=Crc(std::span(bytes).first(28))) {
            result.state=StorageState::corrupt; return result;
        }
        result.configuration.schema=static_cast<std::uint32_t>(Get(bytes,4,4));
        result.configuration.revision=Get(bytes,8,8);
        result.configuration.paused=Get(bytes,16,4)==1;
        result.configuration.border_width_dip=std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes,20,4)));
        if (!contracts::Valid(result.configuration)) {
            result.configuration={}; result.state=StorageState::corrupt;
        }
    } catch (...) { result.state=StorageState::io_error; result.error=ERROR_NOT_ENOUGH_MEMORY; }
    return result;
}
DWORD Storage::Save(contracts::Configuration const& value, bool explicit_reset) const noexcept {
    std::filesystem::path staged;
    try {
        if (!contracts::Valid(value)) return ERROR_INVALID_DATA;
        auto existing=Load();
        if (existing.state==StorageState::io_error) return existing.error;
        if (existing.state!=StorageState::ready && !explicit_reset) return ERROR_INVALID_DATA;
        std::error_code error;
        std::filesystem::create_directories(directory_,error);
        if (error) return static_cast<DWORD>(error.value());
        auto file=File();
        if (explicit_reset && GetFileAttributesW(file.c_str())!=INVALID_FILE_ATTRIBUTES) {
            auto preserved=file; preserved+=L".preserved."+UniqueSuffix();
            if (!CopyFileW(file.c_str(),preserved.c_str(),TRUE)) return GetLastError();
        }
        staged=file; staged+=L".pending."+UniqueSuffix();
        DWORD written=0;
        {
            Handle handle(CreateFileW(staged.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
            if (!handle) return GetLastError();
            std::array<std::byte,disk_size> bytes{};
            Put(bytes,0,0x31435357,4); Put(bytes,4,value.schema,4); Put(bytes,8,value.revision,8);
            Put(bytes,16,value.paused?1:0,4); Put(bytes,20,std::bit_cast<std::uint32_t>(value.border_width_dip),4);
            Put(bytes,28,Crc(std::span(bytes).first(28)),4);
            if (!WriteFile(handle.get(),bytes.data(),disk_size,&written,nullptr) ||
                written!=disk_size || !FlushFileBuffers(handle.get())) {
                auto failure=GetLastError();
                handle=Handle{}; DeleteFileW(staged.c_str());
                return failure?failure:ERROR_WRITE_FAULT;
            }
        }
        DWORD failure=ERROR_SUCCESS;
        for (unsigned attempt=0;attempt<5;++attempt) {
            BOOL success;
            if (GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)
                success=MoveFileExW(staged.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH);
            else
                success=ReplaceFileW(file.c_str(),staged.c_str(),nullptr,0,nullptr,nullptr);
            if (success) return ERROR_SUCCESS;
            failure=GetLastError();
            if (failure!=ERROR_SHARING_VIOLATION && failure!=ERROR_ACCESS_DENIED) break;
            // Antivirus/indexing readers can briefly deny deletion; retry only safe failures.
            if (attempt<4) Sleep(20);
        }
        DeleteFileW(staged.c_str()); return failure;
    } catch (...) {
        if (!staged.empty()) DeleteFileW(staged.c_str());
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}
}
