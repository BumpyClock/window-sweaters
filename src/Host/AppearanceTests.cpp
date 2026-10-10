#include "Appearance.h"
#include "..\Settings\Preview.h"
#include "..\Rendering\KnitRaster.h"
#include <array>
#include <cstdio>
#include <string_view>
#include <windows.h>
#include <objbase.h>

// The isolated adapter project supplies resolver dependencies; the existing
// Host test target remains buildable until its runtime/project ownership handoff.
#ifdef WS_HOST_BY_APP_ADAPTER_TESTS
namespace {
using namespace window_sweaters;
std::size_t adapterChecks{};
std::size_t optionalOwnerFixtures{};
void Check(bool condition, const char* message) {
    ++adapterChecks;
    if (!condition) throw std::runtime_error(message);
}
applications::AppIdentity Executable(std::wstring_view path) {
    auto identity = applications::AppIdentity::FromExecutablePath(path);
    if (!identity) throw std::runtime_error("Invalid executable fixture");
    return *identity;
}
applications::AppIdentity Package(std::wstring_view aumid) {
    auto identity = applications::AppIdentity::FromPackagedAumid(aumid);
    if (!identity) throw std::runtime_error("Invalid package fixture");
    return *identity;
}
const colorways::WindowsInstallRoots roots = [] {
    colorways::WindowsInstallRoots value;
    value.program_files = L"C:\\Program Files";
    value.program_files_x86 = L"C:\\Program Files (x86)";
    value.local_app_data = L"C:\\Users\\Fixture\\AppData\\Local";
    return value;
}();
const auto chrome = Executable(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
const auto code = Executable(L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe");
const auto unknown = Executable(L"C:\\Tools\\Unknown.exe");

contracts::Configuration ByAppConfiguration() {
    contracts::Configuration configuration;
    configuration.design_selection = contracts::DesignSelection::by_app;
    configuration.yarn_argb = 0xff123456u;
    configuration.stitch_style = contracts::StitchStyle::garter;
    configuration.material_style = contracts::MaterialStyle::plain;
    configuration.stitch_rows = 3.5f;
    configuration.border_width_dip = 16.25f;
    return configuration;
}
void ResolutionError(const colorways::ResolutionResult& result, colorways::ResolutionError expected) {
    try {
        (void)host::MakeByAppAppearance(ByAppConfiguration(), result);
    } catch (const host::AppearanceResolutionError& error) {
        Check(error.Error() == expected, "Adapter preserves exact resolution error");
        return;
    }
    throw std::runtime_error("Resolution error became a successful appearance");
}
template<class Function>
void Rejected(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        ++adapterChecks;
        return;
    }
    throw std::runtime_error("Unsupported adapter input became a successful appearance");
}

void ManualIdentityIndependence() {
    const std::array identities{chrome, code, unknown, Package(L"Microsoft.WindowsStore_8wekyb3d8bbwe!App")};
    auto configuration = ByAppConfiguration();
    knitting::TileCache cache;
    configuration.design_selection = contracts::DesignSelection::original;
    for (std::size_t index = 0; index < designs::ws_design_theme_count(); ++index) {
        configuration.design_id = designs::ws_design_theme_at(index)->id;
        const auto baseline = cache.Get(host::MakeAppearance(configuration));
        for (const auto& identity : identities) {
            const auto tile = host::MakeAppearance(configuration, identity, roots);
            Check(tile.baseArgb == 0xff123456u && tile.bandDip == 16.5f && tile.gauge.rows == 3.5f,
                "Manual theme preserves saved base yarn, gauge and normalized band");
            Check(tile.chart && tile.chart->semanticId == configuration.design_id &&
                  tile.stitch == knitting::Stitch::Stockinette &&
                  tile.material == (configuration.design_id == "messages" ? knitting::Material::Plain : knitting::Material::Raised),
                "Manual theme keeps exact semantic identity and authored material");
            Check(cache.Get(tile) == baseline, "Manual theme tile key is independent of target identity");
        }
    }
    configuration.design_selection = contracts::DesignSelection::plain;
    configuration.design_id.clear();
    for (const auto stitch : {contracts::StitchStyle::stockinette, contracts::StitchStyle::rib, contracts::StitchStyle::garter}) {
        for (const auto material : {contracts::MaterialStyle::plain, contracts::MaterialStyle::raised}) {
            configuration.stitch_style = stitch;
            configuration.material_style = material;
            const auto baseline = cache.Get(host::MakeAppearance(configuration));
            for (const auto& identity : identities) {
                const auto tile = host::MakeAppearance(configuration, identity, roots);
                Check(!tile.chart && tile.baseArgb == 0xff123456u && tile.gauge.rows == 3.5f && tile.bandDip == 16.5f &&
                      tile.stitch == static_cast<knitting::Stitch>(stitch) &&
                      tile.material == static_cast<knitting::Material>(material),
                    "Manual plain retains all six stitch/material combinations");
                Check(cache.Get(tile) == baseline, "Manual plain tile key is independent of target identity");
            }
        }
    }
}

void KnownOwners() {
    struct Fixture {
        bool packaged;
        std::wstring_view identity;
        const char* theme;
        std::uint32_t base, flags = 48, width = 12;
    };
    const Fixture originalFixtures[] = {
        {false, L"C:\\Users\\Fixture\\AppData\\Local\\Microsoft\\Teams\\current\\Teams.exe", "microsoft-teams", 0xff9283c1},
        {false, L"C:\\Program Files\\Microsoft VS Code\\Code.exe", "vs-code", 0xff237caf},
        {false, L"C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe", "vs-code", 0xff237caf},
        {false, L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe", "vs-code", 0xff237caf},
        {false, L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "google-chrome", 0xfff4f0e6, 56, 112},
        {false, L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe", "google-chrome", 0xfff4f0e6, 56, 112},
        {false, L"C:\\Users\\Fixture\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe", "google-chrome", 0xfff4f0e6, 56, 112},
        {false, L"C:\\Program Files\\Mozilla Firefox\\firefox.exe", "firefox", 0xff643a9a},
        {false, L"C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe", "firefox", 0xff643a9a},
        {false, L"C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE", "microsoft-word", 0xff2855a1},
        {false, L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\WINWORD.EXE", "microsoft-word", 0xff2855a1},
        {false, L"C:\\Program Files\\Microsoft Office\\root\\Office16\\EXCEL.EXE", "microsoft-excel", 0xff28674f},
        {false, L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\EXCEL.EXE", "microsoft-excel", 0xff28674f},
        {false, L"C:\\Program Files\\Microsoft Office\\root\\Office16\\POWERPNT.EXE", "microsoft-powerpoint", 0xffb9573d},
        {false, L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\POWERPNT.EXE", "microsoft-powerpoint", 0xffb9573d},
        {false, L"C:\\Program Files\\Microsoft Office\\root\\Office16\\OUTLOOK.EXE", "microsoft-outlook", 0xff176bb7},
        {false, L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\OUTLOOK.EXE", "microsoft-outlook", 0xff176bb7},
        {true, L"MSTeams_8wekyb3d8bbwe!MSTeams", "microsoft-teams", 0xff9283c1},
    };
    // Additional catalogue themes are integration inputs, not prerequisites
    // for the original 37-design baseline or its verified Windows mappings.
    const Fixture additionalFixtures[] = {
        {false, L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", "microsoft-edge", 0xff1d5b93},
        {false, L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\Zed\\Zed.exe", "zed", 0xff1d2026},
        {false, L"C:\\Windows\\explorer.exe", "file-explorer", 0xfff5eedc, 56},
        {true, L"Microsoft.WindowsStore_8wekyb3d8bbwe!App", "microsoft-store", 0xff1e3b66},
        {true, L"Microsoft.MicrosoftOfficeHub_8wekyb3d8bbwe!Microsoft.MicrosoftOfficeHub", "microsoft-copilot", 0xfff3ebdc},
        {true, L"Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe!App", "microsoft-solitaire", 0xff3f7a58},
        {true, L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", "microsoft-photos", 0xff1f5fa8},
        {true, L"Microsoft.OneDrive.Photos_8wekyb3d8bbwe!OneDrive.App", "microsoft-photos", 0xff1f5fa8},
        {true, L"Microsoft.Todos_8wekyb3d8bbwe!App", "microsoft-to-do", 0xff3b73c8},
        {true, L"Clipchamp.Clipchamp_yxz26nhyzhsrt!App", "clipchamp", 0xff5b3fa0},
        {true, L"Microsoft.GamingApp_8wekyb3d8bbwe!Microsoft.Xbox.App", "xbox", 0xff1c211d},
        {true, L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", "windows-calculator", 0xff3b3f46, 48, 16},
        {true, L"MicrosoftCorporationII.Windows365_8wekyb3d8bbwe!Windows365", "windows-app", 0xff2b5fa8},
        {true, L"Microsoft.BingWeather_8wekyb3d8bbwe!App", "msn-weather", 0xff5b9fd6},
        {true, L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", "windows-terminal", 0xff0c0c0c},
    };
    const auto checkOwner = [](const Fixture& fixture, const applications::AppIdentity& identity) {
        const auto tile = host::MakeAppearance(ByAppConfiguration(), identity, roots);
        Check(tile.chart && tile.chart->semanticId == fixture.theme && tile.baseArgb == fixture.base,
            "Exact supported Windows owner uses its authored theme and base yarn");
        Check(tile.chart->width == fixture.width && tile.chart->height == 6,
            "Authored repeat dimensions retained");
        Check(tile.chart->appearance.flags == fixture.flags && tile.chart->appearance.outsideAnchored &&
              tile.chart->appearance.cornerArgb == 0 && tile.chart->appearance.cuffArgb == 0,
            "Exact authored finishing/anchoring flags retained");
        Check(tile.stitch == knitting::Stitch::Stockinette && tile.material == knitting::Material::Raised &&
              tile.gauge.rows == 3.5f && tile.bandDip == 16.5f,
            "Authored renderer controls stitch/material; saved gauge and band retained");
    };
    for (const auto& fixture : originalFixtures) {
        const auto identity = fixture.packaged ? Package(fixture.identity) : Executable(fixture.identity);
        checkOwner(fixture, identity);
    }
    for (const auto& fixture : additionalFixtures) {
        const auto identity = fixture.packaged ? Package(fixture.identity) : Executable(fixture.identity);
        const auto mapping = colorways::MatchWindowsApplication(identity, roots);
        if (!designs::ws_design_theme_by_id(fixture.theme) || !mapping || mapping->theme_id != fixture.theme)
            continue;
        checkOwner(fixture, identity);
        ++optionalOwnerFixtures;
    }
    const auto chromeTile = host::MakeAppearance(ByAppConfiguration(), chrome, roots);
    const auto& cells = chromeTile.chart->argbCells;
    Check(cells[0] == 0 && cells[4] == 0xffd8675bu && cells[20] == 0xff6ba776u &&
          cells[36] == 0xffedcc70u && cells[100] == 0xff4285f4u && cells[3 * 112] == 0xffd8675bu,
        "Chrome literal authored red/green/yellow/blue yarn and base sentinel");
    const auto codeTile = host::MakeAppearance(ByAppConfiguration(), code, roots);
    Check(codeTile.chart->argbCells[0] == 0xffb6e1edu && codeTile.chart->argbCells[1] == 0 &&
          codeTile.chart->argbCells[5] == 0xff17374fu && codeTile.baseArgb == 0xff237cafu,
        "VS Code literal ice/ink palette is not recolored using saved yarn");
}

void NormalizationAndFallback() {
    auto configuration = ByAppConfiguration();
    knitting::TileCache cache;
    const auto mapped = cache.Get(host::MakeAppearance(configuration, chrome, roots));
    for (const auto path : {L"c:\\PROGRAM FILES\\google\\CHROME\\application\\CHROME.EXE",
                           L"\\\\?\\C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
                           L"C:/Program Files/Google/Chrome/Application/chrome.exe"}) {
        Check(cache.Get(host::MakeAppearance(configuration, Executable(path), roots)) == mapped,
            "Supported executable normalization keeps exact authored tile key");
    }
    for (const auto path : {L"C:\\Tools\\chrome.exe", L"C:\\Program Files\\Google\\Chrome Beta\\Application\\chrome.exe",
                           L"C:\\Program Files\\Microsoft VS Code Insiders\\Code.exe",
                           L"C:\\Program Files\\Google\\Chrome\\Application\\chrome-helper.exe"}) {
        Check(!host::MakeAppearance(configuration, Executable(path), roots).chart,
            "Basename/helper/channel resemblance never supplies an authored chart");
    }
    for (const auto aumid : {L"msteams_8wekyb3d8bbwe!MSTeams", L"MSTeams_8wekyb3d8bbwe!Helper",
                            L"MSTeams_aaaaaaaaaaaaa!MSTeams", L"Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe!App"}) {
        Check(!host::MakeAppearance(configuration, Package(aumid), roots).chart,
            "Package case, application, publisher and channel must match supported identity exactly");
    }
    Check(!host::MakeAppearance(configuration, chrome, {}).chart,
        "Failed trusted-root lookup does not guess a mapped install");
    auto relocatedRoots = roots;
    relocatedRoots.program_files = L"D:\\Programs\\";
    const auto relocated = Executable(L"D:\\Programs\\Google\\Chrome\\Application\\chrome.exe");
    Check(cache.Get(host::MakeAppearance(configuration, relocated, relocatedRoots)) == mapped,
        "Caller-cached relocated OS root controls exact mapping without hard-coded drive");
    const auto fallback = host::MakeAppearance(configuration, unknown, roots);
    Check(!fallback.chart && fallback.baseArgb == 0xff7c9885u &&
          fallback.stitch == knitting::Stitch::Garter && fallback.material == knitting::Material::Plain,
        "Unknown validated identity uses literal stable Wool fallback and saved plain stitch/material");
    const auto before = cache.Get(fallback);
    configuration.revision = 77;
    configuration.yarn_argb = 0xffabcdefu;
    const auto relaunched = Executable(L"\\\\?\\c:\\TOOLS\\unknown.EXE");
    for (int repeat = 0; repeat < 8; ++repeat)
        Check(cache.Get(host::MakeAppearance(configuration, relaunched, roots)) == before,
            "Unmatched appearance survives repeated resolution, revision and relaunch casing");
    const auto upper = Executable(L"C:\\Tools\\\u00c4pp.exe");
    const auto lower = Executable(L"C:\\Tools\\\u00e4pp.exe");
    Check(cache.Get(host::MakeAppearance(configuration, upper, roots)) ==
          cache.Get(host::MakeAppearance(configuration, lower, roots)),
        "Ordinal-equal Unicode identity fallback keeps its tile key");
    auto metadata = applications::ResolveMetadata(applications::PackagedAumid{L"UnrelatedApp_8wekyb3d8bbwe!App"},
        applications::ExecutablePath{L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"});
    const auto& owner = std::get<applications::AppMetadata>(metadata);
    Check(!host::MakeAppearance(configuration, owner.Identity(), roots).chart,
        "Packaged owner identity wins over its mapped executable basename alias");
    const applications::MetadataResult unavailable = applications::MetadataUnavailable{
        applications::UnavailableReason::AccessDenied, applications::QueryStage::ApplicationId, ERROR_ACCESS_DENIED};
    Check(!configuration.eligibility.Allows(unavailable) && configuration.eligibility.Allows(metadata),
        "Existing metadata gate denies unavailable owner instead of creating a fallback identity");
}

void ResolverBoundaryAndFinishing() {
    const auto configuration = ByAppConfiguration();
    colorways::ResolverSettings settings;
    settings.design = colorways::SelectedTheme{"does-not-exist"};
    ResolutionError(colorways::ResolveAppearance(&chrome, settings, {}, roots), colorways::ResolutionError::UnknownTheme);
    settings = {};
    settings.global_pattern = colorways::AuthoredChart{"does-not-exist"};
    ResolutionError(colorways::ResolveAppearance(&chrome, settings, {}, roots), colorways::ResolutionError::UnknownChart);
    settings = {};
    settings.fallback_basket = static_cast<colorways::Basket>(99);
    ResolutionError(colorways::ResolveAppearance(&chrome, settings, {}, roots), colorways::ResolutionError::InvalidBasket);
    std::array<colorways::PersonalRule, 1> rules{{{chrome, {{0x00112233u}, {}}, {}}}};
    ResolutionError(colorways::ResolveAppearance(&chrome, {}, rules, roots), colorways::ResolutionError::InvalidColor);
    rules[0].colors.base_argb = 0xff112233u;
    rules[0].pattern = colorways::PersonalChart{"invalid", 0};
    ResolutionError(colorways::ResolveAppearance(&chrome, {}, rules, roots), colorways::ResolutionError::InvalidPersonalChart);
    rules[0].pattern.reset();
    auto tile = host::MakeByAppAppearance(configuration, colorways::ResolveAppearance(&chrome, {}, rules, roots));
    Check(tile.baseArgb == 0xff112233u && tile.chart->semanticId == "google-chrome" &&
          tile.chart->argbCells[4] == 0xffd8675bu,
        "Resolver personal base overrides only base yarn, not literal authored contrast");
    rules[0].pattern = colorways::PlainPattern{};
    tile = host::MakeByAppAppearance(configuration, colorways::ResolveAppearance(&chrome, {}, rules, roots));
    Check(tile.baseArgb == 0xff112233u && !tile.chart && tile.stitch == knitting::Stitch::Garter &&
          tile.material == knitting::Material::Plain, "Resolver personal plain outranks mapped pattern");
    settings = {};
    settings.global_pattern = colorways::AuthoredChart{"checker"};
    tile = host::MakeByAppAppearance(configuration, colorways::ResolveAppearance(&chrome, settings, rules, roots));
    Check(tile.chart->semanticId == "google-chrome" && tile.chart->width == 4 && tile.chart->height == 4 &&
          tile.baseArgb == 0xff112233u && tile.chart->argbCells[0] == 0 && tile.chart->argbCells[2] == 0xfff2eee4u,
        "Explicit resolver chart outranks personal plain without losing personal base");
    settings.global_pattern = colorways::ZigzagPattern{};
    Rejected([&] { (void)host::MakeByAppAppearance(configuration, colorways::ResolveAppearance(&chrome, settings, {}, roots)); });
    rules[0].pattern = colorways::PersonalChart{"valid-but-no-asset-adapter", 1};
    Rejected([&] { (void)host::MakeByAppAppearance(configuration, colorways::ResolveAppearance(&chrome, {}, rules, roots)); });
    Rejected([&] { (void)host::MakeByAppAppearance(configuration,
        colorways::ResolveAppearance(&unknown, {}, {}, roots, colorways::AutomaticAppearance{0xff112233u, {}, "twinkle"})); });
    for (const auto theme : {"notes", "calendar", "whatsapp", "messages"}) {
        settings = {};
        settings.design = colorways::SelectedTheme{theme};
        tile = host::MakeByAppAppearance(configuration, colorways::ResolveAppearance(&chrome, settings, {}, roots));
        if (tile.chart->semanticId == "notes" || tile.chart->semanticId == "calendar") {
            const bool notes = tile.chart->semanticId == "notes";
            Check(tile.chart->appearance.flags == 50 && tile.chart->appearance.cuffArgb ==
                  (notes ? 0xffefc852u : 0xffe55c52u) && tile.chart->argbCells[0] == 0 &&
                  tile.chart->argbCells[12] == 0 && tile.chart->argbCells[60] == (notes ? 0xfffffcf3u : 0u),
                "Authored continuous cuff colors survive; cuff-colored chart cells remain base sentinels");
        } else if (tile.chart->semanticId == "whatsapp") {
            Check(tile.chart->appearance.flags == 49 && tile.chart->appearance.cornerArgb == 0 &&
                  tile.baseArgb == 0xff6ea77bu, "Solid corners retain authored base-yarn semantics");
        } else {
            Check(tile.chart->appearance.flags == 36 && tile.material == knitting::Material::Plain &&
                  tile.stitch == knitting::Stitch::Stockinette && tile.chart->argbCells[18] == 0xfff8f7e8u,
                "Round-dot chart keeps literal palette and deliberately unsculpted stockinette");
        }
    }
    auto inconsistent = colorways::ResolveAppearance(&chrome, {}, {}, roots);
    std::get<colorways::ResolvedAppearance>(inconsistent).theme_id.reset();
    ResolutionError(inconsistent, colorways::ResolutionError::UnknownTheme);
    std::get<colorways::ResolvedAppearance>(inconsistent).theme_id = "missing-authored-theme";
    ResolutionError(inconsistent, colorways::ResolutionError::UnknownTheme);
    inconsistent = colorways::ResolveAppearance(&chrome, {}, {}, roots);
    std::get<colorways::ResolvedAppearance>(inconsistent).pattern = colorways::AuthoredChart{"missing-authored-chart"};
    ResolutionError(inconsistent, colorways::ResolutionError::UnknownChart);
    auto invalid = configuration;
    invalid.yarn_argb = 0;
    Rejected([&] { (void)host::MakeAppearance(invalid, chrome, roots); });
    invalid = configuration;
    invalid.design_selection = contracts::DesignSelection::original;
    invalid.design_id = "missing-authored-theme";
    Rejected([&] { (void)host::MakeAppearance(invalid, chrome, roots); });
}

void TargetTileKeys() {
    auto configuration = ByAppConfiguration();
    knitting::TileCache cache;
    auto chromeSpec = host::MakeAppearance(configuration, chrome, roots);
    auto codeSpec = host::MakeAppearance(configuration, code, roots);
    chromeSpec.rasterScale = codeSpec.rasterScale = 2;
    const auto first = cache.Get(chromeSpec);
    const auto second = cache.Get(codeSpec);
    Check(first != second && cache.GenerationCount() == 2 && first->image.pixels != second->image.pixels,
        "Two app owners create distinct production tile keys and rendered pixel payloads");
    auto refreshed = host::MakeAppearance(configuration, chrome, roots);
    refreshed.rasterScale = 2;
    Check(cache.Get(refreshed) == first && cache.GenerationCount() == 2, "Equivalent authored chart instances reuse the tile key");
    auto changed = chromeSpec;
    changed.baseArgb = 0xff010203u;
    Check(cache.Get(changed) != first, "Base-yarn change invalidates authored tile key");
    changed = chromeSpec;
    auto finishing = changed.chart->appearance;
    finishing.flags |= designs::ChartSolidCorners;
    changed.chart = std::make_shared<const knitting::Chart>(changed.chart->semanticId, changed.chart->contentRevision,
        changed.chart->width, changed.chart->height, changed.chart->argbCells, finishing);
    Check(cache.Get(changed) != first, "Authored finishing change invalidates tile key");
    changed = chromeSpec;
    ++changed.gauge.rows;
    Check(cache.Get(changed) != first, "Gauge change invalidates authored tile key");
}
}
#endif

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
            throw std::runtime_error("Identity-free Host placeholder remains distinct from labelled Settings example");
        std::printf("PASS: %zu Host/Settings production appearance pixel-parity cases (%zu catalogue, six stitch/material); identity-free Host placeholder is explicit\n",
            checks, designs::ws_design_theme_count());
#ifdef WS_HOST_BY_APP_ADAPTER_TESTS
        ManualIdentityIndependence();
        KnownOwners();
        NormalizationAndFallback();
        ResolverBoundaryAndFinishing();
        TargetTileKeys();
        std::printf("PASS: %zu identity-aware Host adapter checks (18 required original owner fixtures, %zu optional extensions); native two-app desktop demonstration is a separate gate\n",
            adapterChecks, optionalOwnerFixtures);
#endif
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        CoUninitialize();
        return 1;
    }
}
