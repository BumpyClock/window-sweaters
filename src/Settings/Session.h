#pragma once
#include "..\Configuration\ConfigurationServer.h"
#include <functional>
#include <memory>

namespace window_sweaters::settings {
struct Options {
    bool test_mode=false;
    std::filesystem::path storage_directory;
    std::wstring endpoint_suffix;
};
struct ConnectionState {
    bool connected=false;
    bool pending=false;
    contracts::Status status=contracts::Status::disconnected;
    contracts::Configuration configuration{};
    contracts::Identity server_instance{};
    std::uint64_t client_lease=0;
    std::wstring detail=L"Connecting to the host…";
};
struct SessionTransport {
    std::function<contracts::Message(contracts::Message const&)> exchange;
    std::function<bool()> start_host;
};
class Session {
public:
    Session(Options options,std::function<void(ConnectionState)> publish,SessionTransport transport={});
    ~Session();
    Session(Session const&)=delete;
    Session& operator=(Session const&)=delete;
    void Change(ConnectionState const& displayed,bool paused,float width);
    void Reset(ConnectionState const& displayed);
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Options ParseOptions();
bool StartHost(Options const& options);
}
