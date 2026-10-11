#pragma once
#include "Configuration.h"
#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <string>
#include <stdexcept>
#include <windows.h>
#include <objbase.h>

namespace window_sweaters::contracts {
inline constexpr std::uint16_t protocol_version = 7;
inline constexpr std::size_t frame_size = 176;
inline constexpr std::size_t frame_header_size = 12;
inline constexpr DWORD io_timeout_ms = 3000;
inline constexpr std::size_t maximum_pending_mutations = 32;
inline constexpr std::size_t maximum_pending_mutation_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t maximum_client_leases = 128;
inline constexpr std::size_t maximum_deduplicated_bytes = 4 * 1024 * 1024;
using Identity = std::array<std::uint8_t, 16>;
enum class Operation : std::uint16_t {
    snapshot = 1, mutate = 2, reset = 3, response = 4, eligibility = 5, applications_snapshot = 6, imports = 7, startup = 8
};
enum class Status : std::uint32_t {
    saved, conflict, invalid, wrong_instance, storage_error, recovery_required,
    busy, duplicate_expired, shutting_down, disconnected, client_expired
};
struct Message {
    Operation operation = Operation::snapshot;
    Status status = Status::saved;
    std::uint64_t request_id = 0;
    Identity client{};
    Identity server{};
    std::uint64_t client_lease = 0;
    std::uint32_t error = 0;
    Configuration configuration{};
    applications::EligibilityMutation eligibility;
    ApplicationsSnapshot applications;
    ImportMutation import_edit;
    ImportFailure import_failure;
    Operation response_to=Operation::snapshot;
    bool operator==(Message const& other) const {
        return operation==other.operation && status==other.status && request_id==other.request_id &&
            client==other.client && server==other.server && client_lease==other.client_lease &&
            error==other.error && configuration==other.configuration && response_to==other.response_to &&
            applications==other.applications && EqualMutation(eligibility,other.eligibility) &&
            import_edit==other.import_edit && import_failure==other.import_failure;
    }
};
inline Identity NewIdentity() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) throw std::runtime_error("Cannot create instance identity");
    return std::bit_cast<Identity>(value);
}
inline bool EmptyIdentity(Identity const& value) noexcept { return value == Identity{}; }
inline std::vector<std::byte> Encode(Message const& value) {
    if (value.configuration.design_id.size() > maximum_design_id_bytes)
        throw std::invalid_argument("Design ID exceeds protocol bound");
    std::vector<std::byte> bytes(frame_size);
    auto put = [&](std::size_t at, std::uint64_t v, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) bytes[at + i] = std::byte((v >> (8 * i)) & 255);
    };
    put(0, 0x31535757, 4); put(4, protocol_version, 2);
    put(6, static_cast<std::uint16_t>(value.operation), 2);
    put(12, static_cast<std::uint32_t>(value.status), 4); put(16, value.request_id, 8);
    for (std::size_t i = 0; i < 16; ++i) {
        bytes[24 + i] = std::byte(value.client[i]); bytes[40 + i] = std::byte(value.server[i]);
    }
    put(56, value.configuration.revision, 8); put(64, value.configuration.schema, 4);
    put(68, value.configuration.paused ? 1 : 0, 4);
    put(72, std::bit_cast<std::uint32_t>(value.configuration.border_width_dip), 4);
    put(76, value.error, 4);
    put(80, value.client_lease, 8);
    put(88, static_cast<std::uint32_t>(value.configuration.design_selection), 4);
    put(92, static_cast<std::uint32_t>(value.configuration.stitch_style), 4);
    put(96, static_cast<std::uint32_t>(value.configuration.material_style), 4);
    put(100, std::bit_cast<std::uint32_t>(value.configuration.stitch_rows), 4);
    put(104, value.configuration.yarn_argb, 4);
    put(108, value.configuration.design_id.size(), 4);
    for (std::size_t i=0; i<value.configuration.design_id.size(); ++i)
        bytes[112+i]=std::byte(static_cast<unsigned char>(value.configuration.design_id[i]));
    // Tagged sections are the only extension boundary; unknown tags are rejected.
    auto section=[&](std::uint32_t tag,payload::Writer const& writer) {
        if (writer.bytes.size()+8>maximum_payload_bytes-bytes.size())
            throw std::length_error("Frame exceeds aggregate bound");
        payload::Writer header; header.Number(tag); header.Number(writer.bytes.size());
        bytes.insert(bytes.end(),header.bytes.begin(),header.bytes.end());
        bytes.insert(bytes.end(),writer.bytes.begin(),writer.bytes.end());
    };
    payload::Writer preferences; payload::Preferences(preferences,value.configuration.eligibility);
    section(1,preferences);
    payload::Writer imports; payload::Imports(imports,value.configuration.imports); section(6,imports);
    payload::Writer startup; startup.Number(static_cast<unsigned>(value.configuration.startup_desired)); section(8,startup);
    if (value.operation==Operation::imports ||
        (value.operation==Operation::response && value.response_to==Operation::imports)) {
        payload::Writer edit; payload::ImportEdit(edit,value.import_edit); section(5,edit);
    }
    if (value.operation==Operation::eligibility) {
        payload::Writer mutation; payload::Mutation(mutation,value.eligibility); section(2,mutation);
    }
    if (value.operation==Operation::response) {
        payload::Writer acknowledgement;
        acknowledgement.Number(static_cast<unsigned>(value.response_to)); section(4,acknowledgement);
        if (value.response_to==Operation::imports) {
            payload::Writer failure; failure.Number(value.import_failure.code); failure.Number(value.import_failure.line);
            payload::Utf8(failure,value.import_failure.reason,512); section(7,failure);
        }
        if (value.response_to==Operation::applications_snapshot) {
            payload::Writer snapshot; payload::Snapshot(snapshot,value.applications); section(3,snapshot);
        }
    }
    put(8,bytes.size(),4); return bytes;
}
inline std::size_t FrameLength(std::span<std::byte const> header) noexcept {
    if (header.size()!=frame_header_size) return 0;
    std::uint32_t magic=0,length=0,version=0;
    for (std::size_t i=0;i<4;++i) {
        magic|=std::to_integer<std::uint32_t>(header[i])<<(8*i);
        length|=std::to_integer<std::uint32_t>(header[8+i])<<(8*i);
    }
    version=std::to_integer<unsigned>(header[4])|(std::to_integer<unsigned>(header[5])<<8);
    return magic==0x31535757 && version==protocol_version && length>=frame_size+16 &&
        length<=maximum_payload_bytes?length:0;
}
inline bool Decode(std::span<std::byte const> bytes, Message& value) {
    if (bytes.size()<frame_header_size || FrameLength(bytes.first(frame_header_size))!=bytes.size()) return false;
    value={};
    auto get = [&](std::size_t at, std::size_t count) {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < count; ++i) v |= std::uint64_t(std::to_integer<unsigned>(bytes[at+i])) << (8*i);
        return v;
    };
    if (get(0,4) != 0x31535757 || get(4,2) != protocol_version ||
        get(8,4) != bytes.size() || get(68,4) > 1) return false;
    auto operation = get(6,2), status = get(12,4);
    if (operation < 1 || operation > 8 || status > static_cast<unsigned>(Status::client_expired)) return false;
    value.operation = static_cast<Operation>(operation); value.status = static_cast<Status>(status);
    value.request_id = get(16,8);
    for (std::size_t i=0; i<16; ++i) {
        value.client[i] = std::to_integer<std::uint8_t>(bytes[24+i]);
        value.server[i] = std::to_integer<std::uint8_t>(bytes[40+i]);
    }
    value.configuration.revision = get(56,8);
    value.configuration.schema = static_cast<std::uint32_t>(get(64,4));
    value.configuration.paused = get(68,4) == 1;
    value.configuration.border_width_dip = std::bit_cast<float>(static_cast<std::uint32_t>(get(72,4)));
    value.client_lease = get(80,8);
    value.error=static_cast<std::uint32_t>(get(76,4));
    if (value.operation!=Operation::response && value.error!=0) return false;
    value.configuration.design_selection=static_cast<DesignSelection>(get(88,4));
    value.configuration.stitch_style=static_cast<StitchStyle>(get(92,4));
    value.configuration.material_style=static_cast<MaterialStyle>(get(96,4));
    value.configuration.stitch_rows=std::bit_cast<float>(static_cast<std::uint32_t>(get(100,4)));
    value.configuration.yarn_argb=static_cast<std::uint32_t>(get(104,4));
    auto length=get(108,4);
    if (length>maximum_design_id_bytes) return false;
    value.configuration.design_id.clear();
    for (std::size_t i=0;i<64;++i) {
        auto character=std::to_integer<unsigned char>(bytes[112+i]);
        if (i<length) {
            if (!(character>='a' && character<='z') && !(character>='0' && character<='9') &&
                character!='-') return false;
            value.configuration.design_id+=static_cast<char>(character);
        } else if (character!=0) return false;
    }
    payload::Reader extension(bytes.subspan(frame_size));
    unsigned seen=0;
    while (!extension.Done()) {
        std::uint64_t tag,size;
        if (!extension.Number(tag) || tag<1 || tag>8 || !extension.Number(size) ||
            size>extension.bytes.size()-extension.offset || (seen&(1u<<tag))) return false;
        seen|=1u<<tag;
        payload::Reader section(extension.bytes.subspan(extension.offset,static_cast<std::size_t>(size)));
        extension.offset+=static_cast<std::size_t>(size);
        if (tag==1) {
            if (size>maximum_preference_bytes || !payload::Preferences(section,value.configuration.eligibility)) return false;
        } else if (tag==2) {
            if (value.operation!=Operation::eligibility || !payload::Mutation(section,value.eligibility)) return false;
        } else if (tag==3) {
            if (value.operation!=Operation::response || !payload::Snapshot(section,value.applications)) return false;
        } else if (tag==4) {
            std::uint64_t acknowledgement;
            if (value.operation!=Operation::response || !section.Number(acknowledgement) ||
                acknowledgement<1 || acknowledgement>8 || acknowledgement==4) return false;
            value.response_to=static_cast<Operation>(acknowledgement);
        } else if (tag==5) {
            if ((value.operation!=Operation::imports && value.operation!=Operation::response) ||
                !payload::ImportEdit(section,value.import_edit)) return false;
        } else if (tag==6) {
            if (size>maximum_import_metadata_bytes || !payload::Imports(section,value.configuration.imports)) return false;
        } else if (tag==7) {
            std::uint64_t code{},line{};
            if (value.operation!=Operation::response || !section.Number(code) ||
                code>static_cast<unsigned>(imports::ErrorCode::CatalogueLimit)+1 || !section.Number(line) ||
                !payload::Utf8(section,value.import_failure.reason,512)) return false;
            value.import_failure.code=static_cast<std::uint32_t>(code);
            value.import_failure.line=static_cast<std::uint32_t>(line);
        } else {
            std::uint64_t desired;
            if (!section.Number(desired) || desired>1) return false;
            value.configuration.startup_desired=static_cast<startup::DesiredPreference>(desired);
        }
        if (!section.Done()) return false;
    }
    unsigned required=2u|64u|256u;
    if (value.operation==Operation::imports) required|=32u;
    if (value.operation==Operation::eligibility) required|=4u;
    if (value.operation==Operation::response) {
        required|=16u;
        if (value.response_to==Operation::applications_snapshot) required|=8u;
        if (value.response_to==Operation::imports) required|=32u|128u;
    }
    return seen==required && Valid(value.configuration) &&
        (value.response_to!=Operation::applications_snapshot || value.operation!=Operation::response ||
            value.applications.configuration_revision==value.configuration.revision);
}
}
