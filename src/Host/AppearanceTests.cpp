#include "Appearance.h"
#include "..\Settings\Preview.h"
#include "..\Rendering\KnitRaster.h"
#include <cstdio>
#include <windows.h>
#include <objbase.h>

int main() {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) return 1;
    try {
        using namespace window_sweaters;
        knitting::TileCache hostCache, previewCache;
        std::size_t checks{};
        auto compare = [&](contracts::Configuration configuration) {
            auto hostTile = host::MakeAppearance(configuration);
            auto previewTile = settings::MakeAppearance(configuration);
            hostTile.rasterScale = 2;
            previewTile.rasterScale = 2;
            rendering::KnitFrameSpec frame;
            frame.widthDip = 400; frame.heightDip = 140; frame.bandDip = configuration.border_width_dip;
            const auto actual = rendering::KnitRaster::RenderFrame(frame, hostTile, hostCache);
            const auto preview = rendering::KnitRaster::RenderFrame(frame, previewTile, previewCache);
            if (actual.width != preview.width || actual.height != preview.height || actual.pixels != preview.pixels)
                throw std::runtime_error("Host/Settings production appearance pixel parity");
            ++checks;
        };
        contracts::Configuration configuration;
        for (std::size_t index = 0; index < designs::ws_design_theme_count(); ++index) {
            const auto theme = designs::ws_design_theme_at(index);
            configuration.design_selection = contracts::DesignSelection::original;
            configuration.design_id = theme->id;
            configuration.yarn_argb = theme->base_argb;
            compare(configuration);
        }
        configuration.design_selection = contracts::DesignSelection::plain;
        configuration.design_id.clear();
        for (const auto stitch : {contracts::StitchStyle::stockinette, contracts::StitchStyle::rib,
            contracts::StitchStyle::garter}) {
            for (const auto material : {contracts::MaterialStyle::plain, contracts::MaterialStyle::raised}) {
                configuration.stitch_style = stitch;
                configuration.material_style = material;
                configuration.stitch_rows = 3.5f;
                configuration.yarn_argb = 0xffa25f39u;
                compare(configuration);
            }
        }
        configuration.design_selection = contracts::DesignSelection::by_app;
        const auto fallback = host::MakeAppearance(configuration);
        const auto representative = settings::MakeAppearance(configuration);
        if (fallback.chart || fallback.baseArgb != configuration.yarn_argb || !representative.chart ||
            representative.chart->semanticId != "finder")
            throw std::runtime_error("ByApp Host plain fallback remains distinct from labelled Settings example");
        std::printf("PASS: %zu Host/Settings production appearance pixel-parity cases (37 original, six stitch/material); ByApp Host plain fallback is explicit\n", checks);
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        CoUninitialize();
        return 1;
    }
}
