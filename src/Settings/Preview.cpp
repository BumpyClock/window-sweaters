#include "Preview.h"
#include "..\Integration\IconAppearance.h"
#include <windows.h>
#include <objbase.h>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <system_error>

namespace window_sweaters::settings {
knitting::TileSpec MakeAppearance(contracts::Configuration const& configuration) {
    if (!contracts::Valid(configuration)) throw std::invalid_argument("Invalid preview configuration");
    knitting::TileSpec tile;
    tile.bandDip=configuration.border_width_dip;
    tile.baseArgb=configuration.yarn_argb;
    tile.gauge.rows=configuration.stitch_rows;
    tile.stitch=static_cast<knitting::Stitch>(configuration.stitch_style);
    tile.material=static_cast<knitting::Material>(configuration.material_style);
    if (configuration.design_selection==contracts::DesignSelection::shared_zigzag) {
        tile.baseArgb=0xff769cc0u;
        tile.chart=integration::MakeGeneratedChart("zigzag",icon_yarn::ZigzagContrast(tile.baseArgb));
    } else if (configuration.design_selection!=contracts::DesignSelection::plain) {
        auto id=configuration.design_selection==contracts::DesignSelection::original
            ? configuration.design_id.c_str() : "finder";
        auto theme=designs::ws_design_theme_by_id(id);
        auto chart=theme?designs::ws_design_chart_by_name(theme->chart_name):nullptr;
        if (!chart) throw std::invalid_argument("Original design chart is unavailable");
        tile.chart=knitting::MakeDesignChart(*chart,theme->id);
    }
    return knitting::NormalizeTileSpec(std::move(tile));
}
struct Preview::Impl {
    struct Request { std::uint64_t generation; contracts::Configuration configuration; };
    std::function<void(std::shared_ptr<PreviewResult const>)> publish;
    Render render;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::chrono::steady_clock::time_point due;
    std::uint64_t latest=0;
    bool stopped=false;
    std::thread worker;
    Impl(std::function<void(std::shared_ptr<PreviewResult const>)> callback,Render renderer)
        : publish(std::move(callback)),render(std::move(renderer)) {}
    void Run() noexcept {
        auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        knitting::TileCache cache(16u*1024u*1024u);
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
                        frame.widthDip=400.f; frame.heightDip=140.f;
                        frame.bandDip=result->configuration.border_width_dip;
                        auto tile=MakeAppearance(result->configuration);
                        // Fixed 1x output and bounded 2x production supersampling keep
                        // preview memory independent of monitor size and DPI changes.
                        tile.rasterScale=2;
                        result->image=rendering::KnitRaster::RenderFrame(frame,std::move(tile),cache);
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
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};
Preview::Preview(std::function<void(std::shared_ptr<PreviewResult const>)> publish,Render render)
    : impl_(std::make_unique<Impl>(std::move(publish),std::move(render))) {
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
