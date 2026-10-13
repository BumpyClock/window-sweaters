#include "Preview.h"
#include "..\Rendering\GpuSnapshot.h"
#include "..\Geometry\Geometry.h"
#include "..\Integration\IconAppearance.h"
#include "..\Integration\BaseOverride.h"
#include "..\Configuration\Storage.h"
#include <cmath>
#include <windows.h>
#include <objbase.h>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <system_error>

namespace window_sweaters::settings {
knitting::TileSpec MakeAppearance(contracts::Configuration const& configuration,
    integration::ImportedAppearance const* imported) {
    if (!contracts::Valid(configuration)) throw std::invalid_argument("Invalid preview configuration");
    knitting::TileSpec tile;
    tile.bandDip=configuration.border_width_dip;
    tile.baseArgb=configuration.yarn_argb;
    tile.gauge.rows=configuration.stitch_rows;
    tile.stitch=static_cast<knitting::Stitch>(configuration.stitch_style);
    tile.material=static_cast<knitting::Material>(configuration.material_style);
    auto recolor=integration::ChartRecolor::Preserve;
    if (!configuration.imports.selected_id.empty()) {
        tile.chart=imported?imported->Find(configuration.imports.selected_id):nullptr;
        if (!tile.chart) throw std::invalid_argument("Selected imported preview is not available in a matching saved catalogue");
    } else if (configuration.design_selection==contracts::DesignSelection::shared_zigzag) {
        tile.baseArgb=0xff769cc0u;
        tile.chart=integration::MakeGeneratedChart("zigzag",icon_yarn::ZigzagContrast(tile.baseArgb));
        recolor=integration::ChartRecolor::Contrast;
    } else if (configuration.design_selection!=contracts::DesignSelection::plain) {
        auto id=configuration.design_selection==contracts::DesignSelection::original
            ? configuration.design_id.c_str() : "finder";
        auto theme=designs::ws_design_theme_by_id(id);
        auto chart=theme?designs::ws_design_chart_by_name(theme->chart_name):nullptr;
        if (!chart) throw std::invalid_argument("Original design chart is unavailable");
        tile.chart=imported?imported->Named(chart->name):nullptr;
        recolor=tile.chart?integration::ChartRecolor::Preserve:integration::ChartRecolor::Adapt;
        if (!tile.chart) tile.chart=knitting::MakeDesignChart(*chart,theme->id);
        if (configuration.design_selection==contracts::DesignSelection::by_app && configuration.base_override)
            tile.baseArgb=theme->base_argb;
    }
    if (configuration.base_override)
        tile=integration::OverrideBase(std::move(tile),configuration.base_override_argb,recolor,
            icon_yarn::ZigzagContrast(configuration.base_override_argb));
    return knitting::NormalizeTileSpec(std::move(tile));
}
struct Preview::Impl {
    struct Request { std::uint64_t generation; contracts::Configuration configuration; };
    std::function<void(std::shared_ptr<PreviewResult const>)> publish;
    Render render;
    std::filesystem::path storage_directory;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::chrono::steady_clock::time_point due;
    std::uint64_t latest=0;
    bool stopped=false;
    std::thread worker;
    Impl(std::function<void(std::shared_ptr<PreviewResult const>)> callback,Render renderer,std::filesystem::path directory)
        : publish(std::move(callback)),render(std::move(renderer)),storage_directory(std::move(directory)) {}
    void Run() noexcept {
        auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        knitting::TileCache cache(16u*1024u*1024u);
        std::unique_ptr<rendering::GpuSnapshot> snapshot;
        std::optional<contracts::ImportPreferences> imported_preferences;
        integration::ImportedAppearance imported;
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock,[&]{return stopped || pending.has_value();});
                if (stopped) break;
                if (std::chrono::steady_clock::now()<due) {
                    wake.wait_until(lock,due); continue;
                }
                request=std::move(*pending); pending.reset();
            }
            try {
                auto result=std::make_shared<PreviewResult>();
                result->generation=request.generation; result->configuration=std::move(request.configuration);
                try {
                    if (FAILED(apartment)) throw std::system_error(static_cast<int>(apartment),std::system_category(),"Preview COM initialization");
                    if (render) result->image=render(result->configuration);
                    else {
                        rendering::KnitFrameSpec frame;
                        frame.bandDip=result->configuration.border_width_dip;
                        auto shadow=static_cast<float>(geometry::ShadowMarginPixels(
                            static_cast<std::uint32_t>(std::ceil(frame.bandDip))));
                        frame.widthDip=400.f-2.f*shadow; frame.heightDip=140.f-2.f*shadow;
                        auto const& preferences=result->configuration.imports;
                        if (!imported_preferences || *imported_preferences!=preferences) {
                            if (preferences.charts.empty()) {
                                imported=integration::ImportedAppearance(preferences,std::make_shared<const imports::ImportCatalogue>());
                            } else {
                                auto saved=configuration::Storage(storage_directory).Load();
                                if (saved.state!=configuration::StorageState::ready || saved.configuration.imports!=preferences)
                                    throw std::invalid_argument("Imported preview requires the matching host-saved catalogue");
                                imported=integration::ImportedAppearance(preferences,std::move(saved.catalogue),&imported);
                            }
                            imported_preferences=preferences;
                        }
                        auto tile=MakeAppearance(result->configuration,&imported);
                        tile.rasterScale=2;
                        auto perimeter=rendering::KnitPreparation::PreparePerimeter(frame,std::move(tile),cache);
                        if (!snapshot) snapshot=std::make_unique<rendering::GpuSnapshot>();
                        try { result->image=snapshot->Render(perimeter); }
                        catch (...) { snapshot.reset(); throw; }
                    }
                } catch (std::exception const& error) {
                    std::string text=error.what();
                    result->error.assign(text.begin(),text.end());
                }
                bool current;
                { std::lock_guard lock(mutex); current=!stopped && request.generation==latest; }
                if (current) publish(std::move(result));
            } catch (...) { OutputDebugStringW(L"WindowSweaters: preview publication failed.\n"); }
        }
        snapshot.reset();
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};
Preview::Preview(std::function<void(std::shared_ptr<PreviewResult const>)> publish,Render render,std::filesystem::path directory)
    : impl_(std::make_unique<Impl>(std::move(publish),std::move(render),std::move(directory))) {
    impl_->worker=std::thread([state=impl_.get()]{state->Run();});
}
Preview::~Preview() { Stop(); }
std::uint64_t Preview::Submit(contracts::Configuration const& configuration) {
    if (!contracts::Valid(configuration)) throw std::invalid_argument("Invalid preview configuration");
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) return impl_->latest;
    auto generation=++impl_->latest;
    impl_->pending=Impl::Request{generation,configuration};
    impl_->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
    impl_->wake.notify_one(); return generation;
}
void Preview::Stop() noexcept {
    { std::lock_guard lock(impl_->mutex); impl_->stopped=true; impl_->pending.reset(); impl_->wake.notify_all(); }
    if (impl_->worker.joinable()) impl_->worker.join();
}
}
