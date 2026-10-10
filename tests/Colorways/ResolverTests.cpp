// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Colorways\Resolver.h"
#include "..\..\src\Designs\Catalogue.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace window_sweaters;
using namespace window_sweaters::colorways;

namespace {
std::size_t checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
applications::AppIdentity Executable(std::wstring_view path) {
    auto identity = applications::AppIdentity::FromExecutablePath(path);
    if (!identity) throw std::runtime_error("invalid executable test fixture");
    return *identity;
}
applications::AppIdentity Package(std::wstring_view key) {
    auto identity = applications::AppIdentity::FromPackagedAumid(key);
    if (!identity) throw std::runtime_error("invalid package test fixture");
    return *identity;
}
ResolvedAppearance Appearance(ResolutionResult result) {
    if (const auto* value = std::get_if<ResolvedAppearance>(&result)) return *value;
    throw std::runtime_error("unexpected resolution error");
}
void Error(ResolutionResult result, ResolutionError expected) {
    Check(std::holds_alternative<ResolutionError>(result) &&
          std::get<ResolutionError>(result) == expected, "typed resolution error");
}
const WindowsInstallRoots roots{
    L"C:\\Program Files", L"C:\\Program Files (x86)", L"C:\\Users\\Fixture\\AppData\\Local", L"C:\\Windows"
};
const auto chrome = Executable(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
const auto unknown = Executable(L"C:\\Tools\\Unknown.exe");

void KnownMappings() {
    struct Fixture { std::wstring_view path; std::string_view theme; std::uint32_t base; std::string_view chart; };
    const Fixture fixtures[] = {
        {L"C:\\Users\\Fixture\\AppData\\Local\\Microsoft\\Teams\\current\\Teams.exe", "microsoft-teams", 0xff9283c1, "atelier-teams"},
        {L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "google-chrome", 0xfff4f0e6, "atelier-chrome"},
        {L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe", "google-chrome", 0xfff4f0e6, "atelier-chrome"},
        {L"C:\\Users\\Fixture\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe", "google-chrome", 0xfff4f0e6, "atelier-chrome"},
        {L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe", "vs-code", 0xff237caf, "atelier-vscode"},
        {L"C:\\Program Files\\Microsoft VS Code\\Code.exe", "vs-code", 0xff237caf, "atelier-vscode"},
        {L"C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe", "vs-code", 0xff237caf, "atelier-vscode"},
        {L"C:\\Program Files\\Mozilla Firefox\\firefox.exe", "firefox", 0xff643a9a, "atelier-firefox"},
        {L"C:\\Program Files (x86)\\Mozilla Firefox\\firefox.exe", "firefox", 0xff643a9a, "atelier-firefox"},
        {L"C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE", "microsoft-word", 0xff2855a1, "atelier-word"},
        {L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\WINWORD.EXE", "microsoft-word", 0xff2855a1, "atelier-word"},
        {L"C:\\Program Files\\Microsoft Office\\root\\Office16\\EXCEL.EXE", "microsoft-excel", 0xff28674f, "atelier-excel"},
        {L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\EXCEL.EXE", "microsoft-excel", 0xff28674f, "atelier-excel"},
        {L"C:\\Program Files\\Microsoft Office\\root\\Office16\\POWERPNT.EXE", "microsoft-powerpoint", 0xffb9573d, "atelier-powerpoint"},
        {L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\POWERPNT.EXE", "microsoft-powerpoint", 0xffb9573d, "atelier-powerpoint"},
        {L"C:\\Program Files\\Microsoft Office\\root\\Office16\\OUTLOOK.EXE", "microsoft-outlook", 0xff176bb7, "atelier-outlook"},
        {L"C:\\Program Files (x86)\\Microsoft Office\\root\\Office16\\OUTLOOK.EXE", "microsoft-outlook", 0xff176bb7, "atelier-outlook"},
        {L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", "microsoft-edge", 0xff1d5b93, "atelier-edge"},
        {L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\Zed\\Zed.exe", "zed", 0xff1d2026, "atelier-zed"},
        {L"C:\\Windows\\explorer.exe", "file-explorer", 0xfff5eedc, "atelier-explorer"},
    };
    for (const auto& fixture : fixtures) {
        auto identity = Executable(fixture.path);
        const auto value = Appearance(ResolveAppearance(&identity, {}, {}, roots));
        Check(value.mapping && value.mapping->theme_id == fixture.theme, "exact documented Windows path mapping");
        Check(value.base_argb == fixture.base, "authored Windows fixture base");
        Check(value.pattern == PatternChoice(AuthoredChart{std::string(fixture.chart)}), "authored Windows fixture chart");
        Check(value.color_source == ColorSource::AuthoredApplication &&
              value.chart_colors == ChartColorPolicy::LiteralAuthored &&
              !value.contrast_argb, "authored literal palette, not icon substitution");
        Check(value.mapping->provenance.starts_with("https://"), "mapping has public provenance");
    }
    const auto copilotApp = Executable(L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\GitHub Copilot\\github.exe");
    const auto copilotAppearance = Appearance(ResolveAppearance(&copilotApp, {}, {}, roots));
    Check(copilotAppearance.theme_id == "github-copilot" && copilotAppearance.base_argb == 0xff222733 &&
        copilotAppearance.pattern == PatternChoice(AuthoredChart{"atelier-github-copilot"}),
        "measured standalone GitHub Copilot path receives its literal authored design");
    Check(copilotAppearance.mapping &&
        copilotAppearance.mapping->provenance.starts_with("Measured 2026-10-10") &&
        copilotAppearance.chart_colors == ChartColorPolicy::LiteralAuthored &&
        copilotAppearance.color_source == ColorSource::AuthoredApplication,
        "local path evidence stays distinct from public installer or publisher verification");
    auto teams = Package(L"MSTeams_8wekyb3d8bbwe!MSTeams");
    const auto packaged = Appearance(ResolveAppearance(&teams, {}));
    Check(packaged.base_argb == 0xff9283c1 && packaged.theme_id == "microsoft-teams", "documented new Teams package/publisher");
    Check(packaged.mapping && packaged.mapping->executable == L"ms-teams.exe", "new Teams executable provenance");
    Check(packaged.pattern == PatternChoice(AuthoredChart{"atelier-teams"}), "packaged and classic Teams same authored profile");
    for (const auto& [aumid, theme] : {std::pair{L"Microsoft.WindowsStore_8wekyb3d8bbwe!App", "microsoft-store"},
             std::pair{L"Microsoft.MicrosoftOfficeHub_8wekyb3d8bbwe!Microsoft.MicrosoftOfficeHub", "microsoft-copilot"},
             std::pair{L"Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe!App", "microsoft-solitaire"},
             std::pair{L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", "microsoft-photos"},
             std::pair{L"Microsoft.OneDrive.Photos_8wekyb3d8bbwe!OneDrive.App", "microsoft-photos"},
             std::pair{L"Microsoft.Todos_8wekyb3d8bbwe!App", "microsoft-to-do"},
             std::pair{L"Clipchamp.Clipchamp_yxz26nhyzhsrt!App", "clipchamp"},
             std::pair{L"Microsoft.GamingApp_8wekyb3d8bbwe!Microsoft.Xbox.App", "xbox"},
             std::pair{L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", "windows-calculator"},
             std::pair{L"MicrosoftCorporationII.Windows365_8wekyb3d8bbwe!Windows365", "windows-app"},
             std::pair{L"Microsoft.BingWeather_8wekyb3d8bbwe!App", "msn-weather"},
             std::pair{L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", "windows-terminal"}}) {
        auto identity = Package(aumid);
        Check(Appearance(ResolveAppearance(&identity, {})).theme_id == theme, "documented Windows package AUMID mapping");
    }
    auto mixed = Executable(L"c:\\PROGRAM FILES\\google\\CHROME\\application\\CHROME.EXE");
    Check(Appearance(ResolveAppearance(&mixed, {}, {}, roots)).theme_id == "google-chrome", "Windows path ordinal case matching");
    auto extended = Executable(L"\\\\?\\C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
    Check(Appearance(ResolveAppearance(&extended, {}, {}, roots)).theme_id == "google-chrome", "identity normalization not basename alias");
    Check(!MatchWindowsApplication(chrome, {}), "unknown install roots fail closed");
    auto relocated = Executable(L"D:\\Apps\\Google\\Chrome\\Application\\chrome.exe");
    auto custom_roots = roots;
    custom_roots.program_files = L"D:\\Apps\\";
    Check(MatchWindowsApplication(relocated, custom_roots) != nullptr, "caller supplies actual OS root; drive not hard-coded");
}

void UnmappedAndSimilarNames() {
    const std::wstring_view paths[] = {
        L"C:\\Tools\\chrome.exe",
        L"C:\\Tools\\Teams.exe",
        L"C:\\Tools\\ms-teams.exe",
        L"C:\\Tools\\Code.exe",
        L"C:\\Tools\\github.exe",
        L"C:\\Users\\Fixture\\AppData\\Local\\Programs\\GitHub Copilot CLI\\github.exe",
        L"C:\\Tools\\Firefox.exe",
        L"C:\\Tools\\WINWORD.EXE",
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome-helper.exe",
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe.bak",
        L"C:\\Program Files\\Google\\Chrome Beta\\Application\\chrome.exe",
        L"C:\\Program Files\\Microsoft VS Code Insiders\\Code.exe",
        L"C:\\Program Files\\Microsoft VS Code\\Subfolder\\Code.exe",
        L"C:\\Users\\Other\\AppData\\Local\\Microsoft\\Teams\\current\\Teams.exe",
        L"C:\\Users\\Fixture\\AppData\\Local\\Microsoft\\Teams\\Update.exe",
        L"C:\\Users\\Fixture\\AppData\\Local\\Microsoft\\Teams\\current\\TeamsHelper.exe",
        L"C:\\Users\\Fixture\\AppData\\Local\\Spotify\\Spotify.exe",
        L"C:\\Users\\Fixture\\AppData\\Local\\slack\\Slack.exe",
        L"C:\\Windows\\SysWOW64\\explorer.exe",
        L"C:\\Windows\\System32\\notepad.exe",
        L"C:\\Windows\\System32\\cmd.exe",
        L"\\\\server\\share\\Google\\Chrome\\Application\\chrome.exe"
    };
    for (const auto path : paths) {
        auto identity = Executable(path);
        const auto value = Appearance(ResolveAppearance(&identity, {}, {}, roots));
        Check(!value.mapping && !value.theme_id, "unverified/similar basename has no automatic theme");
        Check(value.color_source == ColorSource::StableFallback &&
              value.pattern == PatternChoice(PlainPattern{}), "unmapped deterministic plain fallback");
    }
    const std::wstring_view packages[] = {
        L"MSTeams_aaaaaaaaaaaaa!MSTeams",
        L"MSTeams_8wekyb3d8bbwe!Helper",
        L"Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe!App",
        L"MicrosoftCorporationII.Windows365_8wekyb3d8bbwe!ConnectionShell",
        L"Microsoft.GamingApp_8wekyb3d8bbwe!Microsoft.Xbox.WidgetsApp",
        L"msteams_8wekyb3d8bbwe!MSTeams",
        L"com.tinyspeck.slackdesktop_8yrtsj140pw4g!Slack",
        L"Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"
    };
    for (const auto key : packages) {
        auto identity = Package(key);
        Check(!Appearance(ResolveAppearance(&identity, {}, {}, roots)).mapping, "unverified publisher/relative app ID has no mapping");
    }
    auto metadata = applications::ResolveMetadata(
        applications::PackagedAumid{L"UnrelatedApp_8wekyb3d8bbwe!App"},
        applications::ExecutablePath{L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"});
    const auto& packaged = std::get<applications::AppMetadata>(metadata);
    Check(packaged.ExecutableAlias() == L"chrome.exe" &&
          !Appearance(ResolveAppearance(&packaged.Identity(), {}, {}, roots)).mapping,
          "verified basename alias cannot override unrelated package identity");
}

void FallbackAndRelaunch() {
    // Literal outputs independently evaluated from the pinned source with Python integer arithmetic.
    Check(StableHash(L"") == 0xab3e7c0b, "empty identity FNV plus Murmur finalizer");
    Check(StableHash(L"C:\\Tools\\Unknown.exe") == 0x8e66e825, "path FNV plus Murmur finalizer");
    Check(StableHash(L"c:\\TOOLS\\unknown.EXE") == 0x8e66e825, "ASCII case-folded fallback");
    Check(StableHash(L"MSTeams_8wekyb3d8bbwe!MSTeams") == 0x8e2bb5e1, "package fallback stable hash");
    Check(StableHash(L"APP") == 0xe8172e69, "upstream app-name hash vector");
    Check(StableHash(L"C:\\\u5de5\u5177\\\U0001f600\\app.exe") == 0x3b91c5a7, "UTF-8 identity hash includes Unicode and surrogate pairs");
    const std::uint32_t expected[] = {0xffff6b00, 0xff651fff, 0xff5f27cd, 0xff7c9885, 0xffff9f7a, 0xff3f6b6e, 0xff686e74};
    const std::uint32_t empty[] = {0xff7c3aff, 0xffff9100, 0xffee5a24, 0xff7c9885, 0xffb08ed4, 0xff5c6b8a, 0xff686e74};
    const std::uint32_t expected_palettes[][10] = {
        {0xffff2d95,0xff00d9ff,0xffffd400,0xff7c3aff,0xff00e676,0xffff6b00,0xffff1744,0xff00b8d4},
        {0xfff50057,0xff00e5ff,0xffc6ff00,0xff651fff,0xff1de9b6,0xffff9100},
        {0xffe8175d,0xff0fb9b1,0xfffec230,0xff5f27cd,0xff10ac84,0xffee5a24},
        {0xffd1495b,0xff4e8098,0xffedae49,0xff7c9885,0xff9b6a8f,0xffe8846b,0xff3f7d6e,0xffc46a4e,0xff6d7ba8,0xffb8935f},
        {0xfff08ca4,0xff7fc6c4,0xffffc978,0xffb08ed4,0xff8fce90,0xffff9f7a,0xff8ab6f0,0xffe5a3d0},
        {0xff4a6b52,0xff7d8f5c,0xff3f6b6e,0xff8a7248,0xff5c6b8a,0xff6b5344,0xff2f5e4a},
        {0xff9aa0a6,0xff7c8288,0xffb4bac0,0xff686e74,0xff8d939a}
    };
    const std::size_t lengths[] = {8,6,6,10,8,7,5};
    for (std::size_t i = 0; i < 7; ++i) {
        const auto basket = static_cast<Basket>(i);
        const auto palette = BasketColors(basket);
        Check(palette.size() == lengths[i] &&
              std::equal(palette.begin(), palette.end(), expected_palettes[i]), "all seven exact upstream baskets");
        Check(FallbackColor(unknown.Key(), basket) == expected[i], "exact modulo basket output");
        ResolverSettings settings;
        settings.fallback_basket = basket;
        Check(Appearance(ResolveAppearance(nullptr, settings)).base_argb == empty[i], "unavailable metadata default fallback");
    }
    auto relaunched = Executable(L"C:\\Tools\\unknown.EXE");
    Check(unknown == relaunched, "stable relaunch identity");
    Check(Appearance(ResolveAppearance(&relaunched, {})).base_argb == 0xff7c9885, "relaunch fallback is exact and process-independent");
    Check(Appearance(ResolveAppearance(&unknown, {})).base_argb == 0xff7c9885, "default wool fallback");
    Check(Appearance(ResolveAppearance(nullptr, {}, {}, roots, AutomaticAppearance{0xff112233, {}, {}})).base_argb == 0xff7c9885,
          "unbound automatic sample cannot affect missing identity");
}

void UnicodeIdentityRelaunch() {
    Check(StableHash(L"C:\\Tools\\\u00c4pp.exe") == 0x103708ba &&
          StableHash(L"C:\\Tools\\\u00e4pp.exe") == 0x103708ba,
          "invariant Unicode uppercase keeps independent canonical hash vector");
    struct Fixture { std::wstring_view original; std::wstring_view relaunched; };
    const Fixture fixtures[] = {
        {L"C:\\Tools\\\u00c4pp.exe", L"C:\\Tools\\\u00e4pp.exe"},
        {L"C:\\\u00d6rdner\\\u00dcber\\\u00c4pp.exe", L"c:\\\u00f6RDNER\\\u00fcBER\\\u00e4PP.EXE"},
        {L"C:\\\u0391\u03b8\u03ae\u03bd\u03b1\\App.exe", L"c:\\\u03b1\u0398\u0389\u039d\u0391\\APP.EXE"},
        {L"C:\\\u0422\u0435\u0441\u0442\\App.exe", L"c:\\\u0442\u0415\u0421\u0422\\APP.EXE"},
        {L"\\\\Server\\Sh\u00c4re\\\u00d6rdner\\App.exe", L"\\\\SERVER\\SH\u00e4RE\\\u00f6RDNER\\APP.EXE"}
    };
    for (const auto& fixture : fixtures) {
        const auto original = Executable(fixture.original);
        const auto relaunched = Executable(fixture.relaunched);
        Check(original == relaunched, "Unicode path fixtures are ordinal-equal identities");
        Check(StableHash(original.Key()) == StableHash(relaunched.Key()), "ordinal-equal Unicode full paths have identical stable hashes");
        for (std::size_t i = 0; i < 7; ++i) {
            ResolverSettings settings;
            settings.fallback_basket = static_cast<Basket>(i);
            const auto before = Appearance(ResolveAppearance(&original, settings));
            const auto after = Appearance(ResolveAppearance(&relaunched, settings));
            Check(before.base_argb == after.base_argb &&
                  before.color_source == ColorSource::StableFallback &&
                  after.color_source == ColorSource::StableFallback, "Unicode relaunch casing preserves fallback in every basket");
        }
        const PersonalChart png{"personal:unicode-fixture", 7};
        const std::array<PersonalRule, 1> rules{{{original, {{0xff123456}, {0xffabcdef}}, png}}};
        const auto personal = Appearance(ResolveAppearance(&relaunched, {}, rules));
        Check(personal.base_argb == 0xff123456 && personal.pattern == PatternChoice(png) &&
              personal.color_source == ColorSource::Personal &&
              personal.chart_colors == ChartColorPolicy::PreservePersonalPixels &&
              !personal.contrast_argb, "Unicode canonicalization leaves exact personal-rule equality and PNG colors unchanged");
    }
}

void PrecedenceAndModes() {
    const AutomaticAppearance sample{0xff123456, 0xffabcdef, "twinkle"};
    std::array<PersonalRule, 2> rules{{
        {chrome, {{0xff778899}, {0xff010203}}, AuthoredChart{"checker"}},
        {chrome, {{0xff998877}, {}}, AuthoredChart{"braid"}}
    }};
    auto value = Appearance(ResolveAppearance(&chrome, {}, rules, roots, sample));
    Check(value.base_argb == 0xff778899 && value.pattern == PatternChoice(AuthoredChart{"checker"}), "personal color and chart outrank automatic author/sample");
    Check(value.color_source == ColorSource::Personal && value.pattern_source == PatternSource::Personal, "first bound personal rule wins ties");
    auto same_name = Executable(L"C:\\Tools\\chrome.exe");
    value = Appearance(ResolveAppearance(&same_name, {}, rules, roots));
    Check(value.color_source == ColorSource::StableFallback, "exact personal identity never binds same basename");
    value = Appearance(ResolveAppearance(&chrome, {}, {}, roots, sample));
    Check(value.base_argb == 0xfff4f0e6 && value.pattern == PatternChoice(AuthoredChart{"atelier-chrome"}), "By App retains authored colors and motif even with completed icon sample");
    value = Appearance(ResolveAppearance(&unknown, {}, {}, roots, sample));
    Check(value.base_argb == 0xff123456 && value.contrast_argb == 0xffabcdef &&
          value.pattern == PatternChoice(AuthoredChart{"twinkle"}) &&
          value.chart_colors == ChartColorPolicy::GeneratedContrast, "unmapped app sample extension");

    ResolverSettings settings;
    settings.global_pattern = AuthoredChart{"braid"};
    value = Appearance(ResolveAppearance(&chrome, settings, rules, roots, sample));
    Check(value.base_argb == 0xff778899 && value.pattern == PatternChoice(AuthoredChart{"braid"}) &&
          value.pattern_source == PatternSource::GlobalSelection, "global selected chart overrides personal app pattern, keeps personal base");
    settings.global_pattern = PlainPattern{};
    value = Appearance(ResolveAppearance(&chrome, settings, rules, roots, sample));
    Check(value.pattern == PatternChoice(PlainPattern{}) && value.base_argb == 0xff778899 &&
          !value.contrast_argb && value.chart_colors == ChartColorPolicy::NoChart, "global plain does not get resurrected by automatic/personal patterns");
    settings.global_pattern = ZigzagPattern{};
    value = Appearance(ResolveAppearance(&chrome, settings, rules, roots, sample));
    Check(value.base_argb == 0xff778899 && value.contrast_argb == 0xff010203 &&
          value.chart_colors == ChartColorPolicy::GeneratedContrast, "shared Zigzag personal colors and explicit contrast first");
    rules[0].colors.contrast_argb.reset();
    value = Appearance(ResolveAppearance(&chrome, settings, rules, roots, sample));
    Check(value.base_argb == 0xff778899 && value.contrast_argb == 0xfff6f0de, "personal Zigzag derives contrast; sample accent ignored");
    value = Appearance(ResolveAppearance(&chrome, settings, {}, roots, sample));
    Check(value.base_argb == 0xff123456 && value.contrast_argb == 0xfff6f0de &&
          value.pattern == PatternChoice(ZigzagPattern{}), "shared Zigzag takes sampled app base, not authored color or sampled accent");
    value = Appearance(ResolveAppearance(&chrome, settings, {}, roots));
    Check(value.base_argb == 0xfff4f0e6 && value.color_source == ColorSource::AuthoredApplication, "shared Zigzag retains authored provisional base before sample");
    settings.global_pattern = AuthoredChart{"zigzag"};
    Check(Appearance(ResolveAppearance(&chrome, settings, {}, roots, sample)).pattern == PatternChoice(ZigzagPattern{}),
          "explicit global catalogue zigzag has shared semantics");

    settings.design = SelectedTheme{"safari"};
    settings.global_pattern.reset();
    value = Appearance(ResolveAppearance(&chrome, settings, {}, roots, sample));
    Check(value.base_argb == 0xff268ed8 && value.pattern == PatternChoice(AuthoredChart{"atelier-safari"}) &&
          value.theme_id == "safari" && !value.mapping, "manual macOS design is independent of automatic Windows mapping");
    value = Appearance(ResolveAppearance(&chrome, settings, rules, roots, sample));
    Check(value.base_argb == 0xff778899 && value.pattern == PatternChoice(AuthoredChart{"atelier-safari"}), "personal base remains personal but global theme overrides app chart");
    settings.global_pattern = ZigzagPattern{};
    value = Appearance(ResolveAppearance(&chrome, settings, {}, roots, sample));
    Check(value.base_argb == 0xff268ed8 && value.color_source == ColorSource::SelectedTheme, "explicit manual theme base beats shared automatic sample");
    settings.design = ByAppDesign{};
    settings.global_pattern.reset();
    value = Appearance(ResolveAppearance(&chrome, settings, {}, roots, sample));
    Check(value.base_argb == 0xfff4f0e6 && value.theme_id == "google-chrome", "switching back to By App restores authored profile");

    rules[0].pattern.reset();
    rules[0].colors = {{0xff334455}, {}};
    value = Appearance(ResolveAppearance(&chrome, {}, rules, roots, sample));
    Check(value.base_argb == 0xff334455 && value.pattern == PatternChoice(AuthoredChart{"atelier-chrome"}), "optional color override does not erase authored app chart");
    rules[0].pattern = PlainPattern{};
    Check(Appearance(ResolveAppearance(&chrome, {}, rules, roots, sample)).pattern == PatternChoice(PlainPattern{}),
          "imported omitted chart can bind explicit personal plain");
    std::array<PersonalRule, 1> unknown_rule{{{unknown, {{0xff334455}, {}}, {}}}};
    value = Appearance(ResolveAppearance(&unknown, {}, unknown_rule, roots, sample));
    Check(value.base_argb == 0xff334455 && value.contrast_argb == 0xfff6f0de, "base-only personal override prevents unrelated sample accent");
}

void PersonalPixelsAndValidation() {
    const PersonalChart png{"personal:fixture-pattern", 42};
    const AutomaticAppearance sample{0xff112233, 0xffddeeff, "picnic"};
    std::array<PersonalRule, 1> rule{{{chrome, {{0xff445566}, {0xffaabbcc}}, png}}};
    auto value = Appearance(ResolveAppearance(&chrome, {}, rule, roots, sample));
    Check(value.pattern == PatternChoice(png) && value.chart_colors == ChartColorPolicy::PreservePersonalPixels &&
          !value.contrast_argb, "personal PNG semantic revision and literal-pixel policy; sample and contrast cannot recolor");
    ResolverSettings settings;
    settings.global_pattern = png;
    value = Appearance(ResolveAppearance(&chrome, settings, rule, roots, sample));
    Check(value.pattern == PatternChoice(png) && !value.contrast_argb &&
          value.chart_colors == ChartColorPolicy::PreservePersonalPixels, "globally selected user PNG also preserves literal pixels");
    Check(ZigzagContrast(0xff123456) == 0xfff6f0de, "exact cream contrast for darker yarn");
    Check(ZigzagContrast(0xffffffff) == 0xffbcbcbc, "byte-truncated white contrast");
    Check(ZigzagContrast(0xfff6f0de) == 0xffd9c075, "same hue/saturation cream contrast");
    Check(ZigzagContrast(0xff9f9f9f) == 0xff5c5c5c, "lightness threshold just above .62");
    Check(ZigzagContrast(0xff9e9e9e) == 0xfff6f0de, "lightness threshold just below .62");
    settings.design = SelectedTheme{"not-a-theme"};
    Error(ResolveAppearance(&chrome, settings), ResolutionError::UnknownTheme);
    settings.design = SelectedTheme{std::string("safari\0other", 12)};
    Error(ResolveAppearance(&chrome, settings), ResolutionError::UnknownTheme);
    settings.design = ByAppDesign{};
    settings.global_pattern = AuthoredChart{"not-a-chart"};
    Error(ResolveAppearance(&chrome, settings), ResolutionError::UnknownChart);
    settings.global_pattern = AuthoredChart{std::string("checker\0other", 13)};
    Error(ResolveAppearance(&chrome, settings), ResolutionError::UnknownChart);
    settings.global_pattern = PersonalChart{"", 1};
    Error(ResolveAppearance(&chrome, settings), ResolutionError::InvalidPersonalChart);
    settings.global_pattern = PersonalChart{"personal:fixture", 0};
    Error(ResolveAppearance(&chrome, settings), ResolutionError::InvalidPersonalChart);
    settings.global_pattern.reset();
    rule[0].colors.base_argb = 0x00123456;
    Error(ResolveAppearance(&chrome, settings, rule, roots), ResolutionError::InvalidColor);
    Error(ResolveAppearance(&unknown, {}, {}, roots, AutomaticAppearance{0x00123456, {}, {}}), ResolutionError::InvalidColor);
    rule[0].colors.base_argb = 0xff123456;
    rule[0].colors.contrast_argb = 0x00123456;
    settings.global_pattern = ZigzagPattern{};
    Error(ResolveAppearance(&chrome, settings, rule, roots), ResolutionError::InvalidColor);
    settings.fallback_basket = static_cast<Basket>(999);
    Error(ResolveAppearance(&chrome, settings), ResolutionError::InvalidBasket);
}

void CoverageAndManualCatalogue() {
    const auto covered = WindowsThemeCoverage();
    Check(covered.size() == designs::ws_design_theme_count(), "every catalogue theme has explicit Windows coverage");
    std::size_t mapped = 0;
    for (std::size_t i = 0; i < covered.size(); ++i) {
        const auto& entry = covered[i];
        Check(!entry.limitation.empty(), "coverage limitation explicit");
        Check(std::none_of(covered.begin(), covered.begin() + i, [&](const auto& prior) { return prior.theme_id == entry.theme_id; }), "coverage unique theme IDs");
        const auto* theme = designs::ws_design_theme_by_id(entry.theme_id.data());
        Check(theme != nullptr, "coverage references immutable catalogue");
        ResolverSettings settings;
        settings.design = SelectedTheme{std::string(entry.theme_id)};
        const auto value = Appearance(ResolveAppearance(&unknown, settings, {}, roots));
        Check(value.base_argb == theme->base_argb && value.pattern == PatternChoice(AuthoredChart{theme->chart_name}), "every mapped/unresolved/mac-only theme remains manual");
        const bool has_mapping = std::any_of(WindowsMappings().begin(), WindowsMappings().end(), [&](const auto& map) { return map.theme_id == entry.theme_id; });
        Check(has_mapping == (entry.coverage == WindowsCoverage::Mapped), "coverage and implemented mapping agree");
        if (has_mapping) ++mapped;
    }
    Check(mapped > 0, "at least one theme maps to exact Windows variants");
    for (const auto& map : WindowsMappings())
        Check(std::any_of(covered.begin(), covered.end(), [&](const auto& entry) {
                  return entry.theme_id == map.theme_id && entry.coverage == WindowsCoverage::Mapped;
              }) && !map.provenance.empty(), "every exact variant belongs to a Mapped theme and cites provenance");
    const auto native = QueryWindowsInstallRoots();
    Check(!native.program_files.empty() && !native.local_app_data.empty() && !native.windows.empty(), "native OS Known Folder boundary responsive; no application install probing");
}
} // namespace

int main() {
    try {
        KnownMappings();
        UnmappedAndSimilarNames();
        FallbackAndRelaunch();
        UnicodeIdentityRelaunch();
        PrecedenceAndModes();
        PersonalPixelsAndValidation();
        CoverageAndManualCatalogue();
        std::cout << "PASS: " << checks << " resolver checks; fixtures only, no installed-application demonstration\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
