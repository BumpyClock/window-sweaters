#pragma once
#include "..\Contracts\Configuration.h"
#include "..\Rendering\KnitRaster.h"
#include "..\Integration\ImportedAppearance.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace window_sweaters::settings {
knitting::TileSpec MakeAppearance(contracts::Configuration const& configuration,
    integration::ImportedAppearance const* imported=nullptr);
struct PreviewResult {
    std::uint64_t generation=0;
    contracts::Configuration configuration;
    knitting::RasterImage image;
    std::wstring error;
};
class Preview {
public:
    using Render=std::function<knitting::RasterImage(contracts::Configuration const&)>;
    explicit Preview(std::function<void(std::shared_ptr<PreviewResult const>)> publish,Render render={},
        std::filesystem::path storage_directory={});
    ~Preview();
    Preview(Preview const&)=delete;
    Preview& operator=(Preview const&)=delete;
    std::uint64_t Submit(contracts::Configuration const& configuration);
    void Stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
