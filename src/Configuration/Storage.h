#pragma once
#include "..\Contracts\Protocol.h"
#include <filesystem>

namespace window_sweaters::configuration {
enum class StorageState { ready, corrupt, newer_schema, io_error };
struct LoadedConfiguration {
    StorageState state = StorageState::ready;
    contracts::Configuration configuration{};
    DWORD error = ERROR_SUCCESS;
};
class Storage {
public:
    explicit Storage(std::filesystem::path directory);
    LoadedConfiguration Load() const noexcept;
    DWORD Save(contracts::Configuration const& value, bool explicit_reset = false) const noexcept;
    std::filesystem::path File() const { return directory_ / L"configuration.bin"; }
    std::filesystem::path Directory() const { return directory_; }
private:
    std::filesystem::path directory_;
};
std::filesystem::path DefaultStorageDirectory();
}
