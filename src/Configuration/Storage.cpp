#include "Storage.h"
#include "Pipe.h"
#include <array>
#include <algorithm>
#include <bit>
#include <shlobj.h>

namespace window_sweaters::configuration {
namespace {
constexpr std::size_t legacy_disk_size = 32;
constexpr std::size_t disk_size = 120;
constexpr std::size_t maximum_disk_bytes = imports::maximum_catalogue_bytes + contracts::maximum_payload_bytes + 65536;
std::uint32_t Crc(std::span<std::byte const> bytes) noexcept {
    std::uint32_t crc=0xffffffff;
    for (auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int i=0;i<8;++i) crc = (crc>>1) ^ (0xedb88320 & (0u-(crc&1)));
    }
    return ~crc;
}
void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, std::size_t count) {
    for (std::size_t i=0;i<count;++i) bytes[offset+i]=std::byte((value>>(8*i))&255);
}
std::uint64_t Get(std::span<std::byte const> bytes, std::size_t offset, std::size_t count) {
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
        std::array<std::byte,12> header{};
        DWORD read=0;
        if (!GetFileSizeEx(file.get(),&size) || !ReadFile(file.get(),header.data(),12,&read,nullptr)) {
            result.state=StorageState::io_error; result.error=GetLastError(); return result;
        }
        if (read>=8 && Get(header,0,4)==0x31435357 && Get(header,4,4)>contracts::configuration_schema) {
            result.state=StorageState::newer_schema; return result;
        }
        if (read<8 || size.QuadPart<legacy_disk_size ||
            size.QuadPart>static_cast<LONGLONG>(maximum_disk_bytes)) {
            result.state=StorageState::corrupt; return result;
        }
        auto schema=Get(header,4,4);
        if ((schema>=3 && schema<=6) &&
            (read!=12 || Get(header,8,4)!=static_cast<std::uint64_t>(size.QuadPart))) {
            result.state=StorageState::corrupt; return result;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(file.get(),beginning,nullptr,FILE_BEGIN) ||
            !ReadFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)) {
            result.state=StorageState::io_error; result.error=GetLastError(); return result;
        }
        if (read!=bytes.size()) { result.state=StorageState::corrupt; return result; }
        if (schema>=3 && schema<=6) {
            contracts::Message decoded;
            if (Get(bytes,0,4)!=0x31435357 ||
                bytes.size()<12+contracts::frame_size+16+4 ||
                Get(bytes,bytes.size()-4,4)!=Crc(std::span(bytes).first(bytes.size()-4))) {
                result.state=StorageState::corrupt; return result;
            }
            const auto frame_length=Get(bytes,12+8,4);
            if (frame_length>contracts::maximum_payload_bytes || frame_length>bytes.size()-16 ||
                frame_length<contracts::frame_size+16) {
                result.state=StorageState::corrupt; return result;
            }
            std::vector<std::byte> frame(bytes.begin()+12,bytes.begin()+12+static_cast<std::size_t>(frame_length));
            if (schema<6) {
                if (Get(frame,4,2)!=(schema==3?4u:schema==4?5u:6u) || Get(frame,64,4)!=schema ||
                    (schema<5 && frame_length!=bytes.size()-16) ||
                    (schema==3 && Get(frame,88,4)>static_cast<unsigned>(contracts::DesignSelection::by_app))) {
                    result.state=StorageState::corrupt; return result;
                }
                Put(frame,4,contracts::protocol_version,2);
                Put(frame,64,contracts::configuration_schema,4);
                if (schema<5) {
                    contracts::payload::Writer empty;
                    contracts::payload::Imports(empty,{});
                    const auto offset=frame.size(); frame.resize(offset+8);
                    Put(frame,offset,6,4); Put(frame,offset+4,empty.bytes.size(),4);
                    frame.insert(frame.end(),empty.bytes.begin(),empty.bytes.end());
                }
                contracts::payload::Writer startup; startup.Number(0);
                const auto offset=frame.size(); frame.resize(offset+8);
                Put(frame,offset,8,4); Put(frame,offset+4,startup.bytes.size(),4);
                frame.insert(frame.end(),startup.bytes.begin(),startup.bytes.end());
                Put(frame,8,frame.size(),4);
            } else if (Get(frame,64,4)!=schema) {
                result.state=StorageState::corrupt; return result;
            }
            if (!contracts::Decode(frame,decoded) ||
                decoded.operation!=contracts::Operation::snapshot || decoded.request_id ||
                !contracts::EmptyIdentity(decoded.client) || !contracts::EmptyIdentity(decoded.server) ||
                decoded.client_lease || decoded.error || decoded.status!=contracts::Status::saved) {
                result.state=StorageState::corrupt; return result;
            }
            if (schema>=5) {
                contracts::payload::Reader reader(std::span<const std::byte>(bytes).subspan(
                    12+static_cast<std::size_t>(frame_length),bytes.size()-16-static_cast<std::size_t>(frame_length)));
                imports::StoredCatalogue stored;
                auto rules=imports::ParseRules(decoded.configuration.imports.document);
                auto parsed=std::get_if<imports::ParsedRules>(&rules);
                std::uint64_t high{},count{};
                if (!parsed || !reader.Number(high,8) || !reader.Number(count) || count>imports::maximum_effective_charts) {
                    result.state=StorageState::corrupt; return result;
                }
                stored.rules=*parsed; stored.revision_high_water=high;
                for (std::size_t i=0;i<count;++i) {
                    imports::StoredChart c; std::uint64_t flags{},corner{},cuff{},length{};
                    if (!contracts::payload::Utf8(reader,c.id,80) || !contracts::payload::Utf8(reader,c.name,63) ||
                        !contracts::payload::Utf8(reader,c.revision,71) || !reader.Number(c.content_revision,8) ||
                        !reader.Number(flags) || !reader.Number(corner) || !reader.Number(cuff) ||
                        !reader.Number(length) || !length || length>imports::maximum_png_bytes ||
                        length>reader.bytes.size()-reader.offset) {
                        result.state=StorageState::corrupt; return result;
                    }
                    c.material={static_cast<std::uint32_t>(flags),static_cast<std::uint32_t>(corner),static_cast<std::uint32_t>(cuff)};
                    c.encoded_png.reserve(static_cast<std::size_t>(length));
                    for (std::size_t j=0;j<length;++j)
                        c.encoded_png.push_back(std::to_integer<std::uint8_t>(reader.bytes[reader.offset++]));
                    stored.charts.push_back(std::move(c));
                }
                auto restored=imports::RestoreCatalogue(stored);
                auto catalogue=std::get_if<imports::ImportCatalogue>(&restored);
                if (!reader.Done() || !catalogue || !ValidImportState(decoded.configuration.imports,*catalogue)) {
                    result.state=StorageState::corrupt; return result;
                }
                result.catalogue=std::make_shared<const imports::ImportCatalogue>(std::move(*catalogue));
            }
            result.configuration=std::move(decoded.configuration); return result;
        }
        bool legacy=schema==1;
        auto expected_size=legacy?legacy_disk_size:disk_size;
        if (size.QuadPart!=static_cast<LONGLONG>(expected_size) || read!=expected_size ||
            Get(bytes,0,4)!=0x31435357 ||
            (!legacy && schema!=2) || Get(bytes,16,4)>1 ||
            (legacy && Get(bytes,24,4)!=0) ||
            (!legacy && Get(bytes,112,4)!=0) ||
            Get(bytes,expected_size-4,4)!=Crc(std::span(bytes).first(expected_size-4))) {
            result.state=StorageState::corrupt; return result;
        }
        // Migration is in memory only; the original file survives until a validated save.
        result.configuration.schema=contracts::configuration_schema;
        result.configuration.revision=Get(bytes,8,8);
        result.configuration.paused=Get(bytes,16,4)==1;
        result.configuration.border_width_dip=std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes,20,4)));
        if (!legacy) {
            if (Get(bytes,24,4)>static_cast<unsigned>(contracts::DesignSelection::by_app)) {
                result.configuration={}; result.state=StorageState::corrupt; return result;
            }
            result.configuration.design_selection=static_cast<contracts::DesignSelection>(Get(bytes,24,4));
            result.configuration.stitch_style=static_cast<contracts::StitchStyle>(Get(bytes,28,4));
            result.configuration.material_style=static_cast<contracts::MaterialStyle>(Get(bytes,32,4));
            result.configuration.stitch_rows=std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes,36,4)));
            result.configuration.yarn_argb=static_cast<std::uint32_t>(Get(bytes,40,4));
            auto length=Get(bytes,44,4);
            if (length>contracts::maximum_design_id_bytes) {
                result.configuration={}; result.state=StorageState::corrupt; return result;
            }
            for (std::size_t i=0;i<64;++i) {
                auto character=std::to_integer<unsigned char>(bytes[48+i]);
                if (i<length) {
                    if (!(character>='a' && character<='z') && !(character>='0' && character<='9') &&
                        character!='-') {
                        result.configuration={}; result.state=StorageState::corrupt; return result;
                    }
                    result.configuration.design_id+=static_cast<char>(character);
                } else if (character!=0) {
                    result.configuration={}; result.state=StorageState::corrupt; return result;
                }
            }
        }
        if (!contracts::Valid(result.configuration)) {
            result.configuration={}; result.state=StorageState::corrupt;
        }
    } catch (...) { result.state=StorageState::io_error; result.error=ERROR_NOT_ENOUGH_MEMORY; }
    return result;
}
DWORD Storage::Save(contracts::Configuration const& value, bool explicit_reset) const noexcept {
    try {
        auto loaded=Load();
        if (!explicit_reset) return Save(value,loaded.catalogue);
        imports::StoredCatalogue empty;
        empty.revision_high_water=loaded.catalogue->RevisionHighWater();
        auto restored=imports::RestoreCatalogue(empty);
        auto catalogue=std::make_shared<const imports::ImportCatalogue>(
            std::get<imports::ImportCatalogue>(std::move(restored)));
        auto reset=value; reset.imports={}; DescribeCatalogue(reset.imports,*catalogue);
        return Save(reset,catalogue,true);
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD Storage::Save(contracts::Configuration const& value, CatalogueSnapshot const& catalogue, bool explicit_reset) const noexcept {
    std::filesystem::path staged;
    try {
        if (!contracts::Valid(value) || !catalogue || !ValidImportState(value.imports,*catalogue)) return ERROR_INVALID_DATA;
        auto stored=imports::StoreCatalogue(*catalogue);
        if (std::holds_alternative<imports::Error>(stored)) return ERROR_INVALID_DATA;
        contracts::Message record; record.configuration=value;
        auto payload=contracts::Encode(record);
        std::vector<std::byte> bytes(12);
        bytes.insert(bytes.end(),payload.begin(),payload.end());
        contracts::payload::Writer charts(maximum_disk_bytes);
        auto const& data=std::get<imports::StoredCatalogue>(stored);
        charts.Number(*data.revision_high_water,8); charts.Number(data.charts.size());
        for (auto const& c:data.charts) {
            contracts::payload::Utf8(charts,c.id,80); contracts::payload::Utf8(charts,c.name,63);
            contracts::payload::Utf8(charts,c.revision,71); charts.Number(c.content_revision,8);
            charts.Number(c.material.flags); charts.Number(c.material.corner_argb); charts.Number(c.material.cuff_argb);
            charts.Number(c.encoded_png.size());
            if (c.encoded_png.size()>charts.limit-charts.bytes.size()) return ERROR_INVALID_DATA;
            auto encoded=std::as_bytes(std::span(c.encoded_png));
            charts.bytes.insert(charts.bytes.end(),encoded.begin(),encoded.end());
        }
        bytes.insert(bytes.end(),charts.bytes.begin(),charts.bytes.end()); bytes.resize(bytes.size()+4);
        if (bytes.size()>maximum_disk_bytes) return ERROR_INVALID_DATA;
        Put(bytes,0,0x31435357,4); Put(bytes,4,value.schema,4); Put(bytes,8,bytes.size(),4);
        Put(bytes,bytes.size()-4,Crc(std::span(bytes).first(bytes.size()-4)),4);
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
            if (!WriteFile(handle.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) ||
                written!=bytes.size() || !FlushFileBuffers(handle.get())) {
                auto failure=GetLastError();
                handle=Handle{}; DeleteFileW(staged.c_str());
                return failure?failure:ERROR_WRITE_FAULT;
            }
        }
        DWORD failure=ERROR_SUCCESS;
        for (unsigned attempt=0;attempt<8;++attempt) {
            BOOL success;
            if (GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)
                success=MoveFileExW(staged.c_str(),file.c_str(),MOVEFILE_WRITE_THROUGH);
            else
                success=ReplaceFileW(file.c_str(),staged.c_str(),nullptr,0,nullptr,nullptr);
            if (success) return ERROR_SUCCESS;
            failure=GetLastError();
            if (failure!=ERROR_SHARING_VIOLATION && failure!=ERROR_ACCESS_DENIED &&
                failure!=ERROR_UNABLE_TO_REMOVE_REPLACED) break;
            // Antivirus/indexing readers can briefly deny deletion; retry only safe failures.
            const bool unchangedReplacement=failure==ERROR_UNABLE_TO_REMOVE_REPLACED;
            const unsigned limit=unchangedReplacement?8u:5u;
            if (attempt+1>=limit) break;
            Sleep(unchangedReplacement?20u<<std::min(attempt,3u):20u);
        }
        DeleteFileW(staged.c_str()); return failure;
    } catch (...) {
        if (!staged.empty()) DeleteFileW(staged.c_str());
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}
}
