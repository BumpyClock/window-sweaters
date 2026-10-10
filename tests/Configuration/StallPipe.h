#pragma once
#include "..\..\src\Configuration\Pipe.h"
#include <atomic>
#include <stdexcept>
// Single-instance pipe that never answers; with busy=true a held client makes every other open fail with ERROR_PIPE_BUSY.
struct StallPipe {
    std::wstring name;
    window_sweaters::configuration::Handle server,held;
    explicit StallPipe(bool busy) {
        static std::atomic<unsigned> serial;
        name=window_sweaters::configuration::EndpointName(L"stall-"+std::to_wstring(GetCurrentProcessId())+L"-"+
            std::to_wstring(++serial));
        server=window_sweaters::configuration::Handle(CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,
            1,4096,4096,0,nullptr));
        if (!server) throw std::runtime_error("Cannot create stall pipe");
        if (busy) {
            held=window_sweaters::configuration::Handle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
                OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
            if (!held) throw std::runtime_error("Cannot occupy stall pipe");
        }
    }
};
