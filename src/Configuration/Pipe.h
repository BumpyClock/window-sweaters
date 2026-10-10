#pragma once
#include "..\Contracts\Protocol.h"
#include <filesystem>
#include <string>

namespace window_sweaters::configuration {
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { Handle old(release()); value_ = other.release(); } return *this;
    }
    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept { HANDLE value=value_; value_=nullptr; return value; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
struct EndpointIdentity {
    std::wstring user_sid;
    std::wstring logon_sid;
    DWORD session = 0;
    DWORD integrity = 0;
    LUID authentication{};
};
EndpointIdentity CurrentIdentity();
std::wstring EndpointName(std::wstring const& suffix = {});
std::wstring TestEndpointSuffix(std::filesystem::path const& storage_directory);
std::wstring PipeSecuritySddl();
bool VerifyPeer(HANDLE pipe, bool server_peer, std::filesystem::path const& expected_image) noexcept;
bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write, HANDLE stop = nullptr) noexcept;
bool Connect(HANDLE pipe, HANDLE stop) noexcept;
contracts::Message Exchange(std::wstring const& endpoint, std::filesystem::path const& host_path,
                            contracts::Message const& request);
std::filesystem::path SiblingExecutable(wchar_t const* name);
}
