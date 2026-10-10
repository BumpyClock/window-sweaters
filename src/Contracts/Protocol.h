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
inline constexpr std::uint16_t protocol_version = 2;
inline constexpr std::size_t frame_size = 88;
inline constexpr DWORD io_timeout_ms = 3000;
inline constexpr std::size_t maximum_pending_mutations = 32;
inline constexpr std::size_t maximum_client_leases = 128;
using Identity = std::array<std::uint8_t, 16>;
enum class Operation : std::uint16_t { snapshot = 1, mutate = 2, reset = 3, response = 4 };
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
    Configuration configuration{};
    bool operator==(Message const&) const = default;
};
inline Identity NewIdentity() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) throw std::runtime_error("Cannot create instance identity");
    return std::bit_cast<Identity>(value);
}
inline bool EmptyIdentity(Identity const& value) noexcept { return value == Identity{}; }
inline std::array<std::byte, frame_size> Encode(Message const& value) {
    std::array<std::byte, frame_size> bytes{};
    auto put = [&](std::size_t at, std::uint64_t v, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) bytes[at + i] = std::byte((v >> (8 * i)) & 255);
    };
    put(0, 0x31535757, 4); put(4, protocol_version, 2);
    put(6, static_cast<std::uint16_t>(value.operation), 2); put(8, frame_size, 4);
    put(12, static_cast<std::uint32_t>(value.status), 4); put(16, value.request_id, 8);
    for (std::size_t i = 0; i < 16; ++i) {
        bytes[24 + i] = std::byte(value.client[i]); bytes[40 + i] = std::byte(value.server[i]);
    }
    put(56, value.configuration.revision, 8); put(64, value.configuration.schema, 4);
    put(68, value.configuration.paused ? 1 : 0, 4);
    put(72, std::bit_cast<std::uint32_t>(value.configuration.border_width_dip), 4);
    put(80, value.client_lease, 8);
    return bytes;
}
inline bool Decode(std::span<std::byte const> bytes, Message& value) noexcept {
    if (bytes.size() != frame_size) return false;
    auto get = [&](std::size_t at, std::size_t count) {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < count; ++i) v |= std::uint64_t(std::to_integer<unsigned>(bytes[at+i])) << (8*i);
        return v;
    };
    if (get(0,4) != 0x31535757 || get(4,2) != protocol_version ||
        get(8,4) != frame_size || get(76,4) != 0 || get(68,4) > 1) return false;
    auto operation = get(6,2), status = get(12,4);
    if (operation < 1 || operation > 4 || status > static_cast<unsigned>(Status::client_expired)) return false;
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
    return Valid(value.configuration);
}
}
