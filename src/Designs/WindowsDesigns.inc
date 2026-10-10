// SPDX-License-Identifier: GPL-3.0-only
// Authored for the Windows port; not imported. ImportCatalogue.py never reads or writes this file.
// Same chart contract as AuthoredCharts.inc: top-down rows, '.' is base yarn, a..f index the yarn.
// To add a design: append its rows/yarn, a chart view, a theme view, and a WindowsThemeCoverage entry.

static const char* const k_edge_rows[] = {
    "......aaa...", ".....a...a..", "....a..bb.a.",
    "...a..b..ba.", ".aa....bba..", "cccccccccccc" };
static const uint32_t k_edge_yarn[6] = { 0xff63c7b2u, 0xffa9dcefu, 0xff9fd59au };

static const char* const k_store_rows[] = {
    "..aa........", ".a..a.......", "aaaaaa......",
    "aaaaaa..bc..", "aaaaaa..de..", "aaaaaa......" };
static const uint32_t k_store_yarn[6] = { 0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au };

static const char* const k_copilot_rows[] = {
    "dd...aaa...d", "ddd.aaaaa.dd", "..daa...add.",
    "..bac...cdb.", "bbb.ccccc.bb", "bb...ccc...b" };
static const uint32_t k_copilot_yarn[6] = { 0xff6f9bd8u, 0xff5fb3a6u, 0xffe58aa7u, 0xffeea066u };

static const char* const k_github_copilot_rows[] = {
    "............", ".aaaa..aaaa.", "ba.caaaac.ab",
    "ba.ca..ac.ab", ".aaaa..aaaa.", "............" };
static const uint32_t k_github_copilot_yarn[6] = { 0xffa184e6u, 0xff767d8au, 0xffe6def5u };

static const char* const k_zed_rows[] = {
    "aaaaa.bbbbb.", "...a.....b..", "..a.....b...",
    ".a.....b....", "a.....b.....", "aaaaa.bbbbb." };
static const uint32_t k_zed_yarn[6] = { 0xffefece3u, 0xff4f7fdcu };

static const char* const k_explorer_rows[] = {
    "bb.....cc...", "bbbbbb..cc..", "aaaaaa...cc.",
    "aaaaaa..cc..", "cccccc.cc...", "............" };
static const uint32_t k_explorer_yarn[6] = { 0xffeab447u, 0xffc98a2cu, 0xff3f80d0u };

static const char* const k_solitaire_rows[] = { ".b.b....a...", "bbbbb..aaa..", "bbbbb.aaaaa.", ".bbb..aaaaa.", "..b.....a...", "............" };
static const uint32_t k_solitaire_yarn[6] = { 0xfff2ead8u, 0xffd8675fu };

static const char* const k_pale_blue_dot_rows[] = { "c.......c...", ".bd.........", "..bbd...c...", "...bbbaa....", "c.....aa...c", "........c..." };
static const uint32_t k_pale_blue_dot_yarn[6] = { 0xff9ccbebu, 0xffe8dfbfu, 0xff4d6288u, 0xffdde6f2u };

static const char* const k_get_started_rows[] = { "..aaa...b...", ".aaaaa.bab..", ".aaaaa..b...", "..aaa.......", "..ccc.....b.", "...c........" };
static const uint32_t k_get_started_yarn[6] = { 0xfffbf1d6u, 0xfff2b441u, 0xffa9cff0u };

static const char* const k_steam_rows[] = { ".a..a...bb..", "aaaaaa.bbbb.", "aaccaa.bbbb.", "aaccaa..bb..", "aaaaaa...c..", ".a..a...c..." };
static const uint32_t k_steam_yarn[6] = { 0xff7fb5d9u, 0xffdfe8eeu, 0xff6b8aa6u };

static const char* const k_clipchamp_rows[] = { "aa..aa..aa..", "............", "ccaaccaaccaa", "accaaccaacca", "............", "bb..bb..bb.." };
static const uint32_t k_clipchamp_yarn[6] = { 0xfff1ead8u, 0xff4fb3a9u, 0xffe9806eu };

static const char* const k_to_do_rows[] = { ".....a.bbbb.", "a...a..b..b.", ".a.a...b..b.", "..a....bbbb.", "............", "bb..bb..bb.." };
static const uint32_t k_to_do_yarn[6] = { 0xfff4f1e6u, 0xff9ccdf0u };

static const char* const k_ms_photos_rows[] = { "...a....cc..", "..aaa..cccc.", ".aaaba..cc..", "aaabbba.....", "bbbbbbbbbbbb", "dddddddddddd" };
static const uint32_t k_ms_photos_yarn[6] = { 0xfff3ecd8u, 0xff3fb0b0u, 0xfff2c35au, 0xff8fd3d0u };

static const char* const k_calculator_rows[] = { "................", ".a......c.c.bbb.", "aaa.bbb..c......", ".a......c.c.bbb.", "................", "dddddddddddddddd" };
static const uint32_t k_calculator_yarn[6] = { 0xffd7dce3u, 0xff5b9bd5u, 0xffee9a4du, 0xff6c7684u };

static const char* const k_xbox_rows[] = { "a...a....d..", ".a.a........", "..a...c...b.", ".a.a........", "a...a....a..", "............" };
static const uint32_t k_xbox_yarn[6] = { 0xff52b043u, 0xffe0605au, 0xff5b9be0u, 0xffefc94cu };

static const char* const k_windows_app_rows[] = { "cccccc......", "c....c......", "c..aaaaaaa..", "c..abbbbba..", "cccabbbbba..", "...aaaaaaa.." };
static const uint32_t k_windows_app_yarn[6] = { 0xffe4eefbu, 0xff5ecbe8u, 0xffa990e0u };

static const char* const k_windows_terminal_rows[] = { "aa......ccc.", ".aa.....ccc.", "..aa....ccc.", ".aa.....ccc.", "aa..........", "b.d.e.f.b.d." };
static const uint32_t k_windows_terminal_yarn[6] = { 0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu };

static const char* const k_weather_rows[] = { "..b...aa....", ".bbb.aaaaa..", "..b.aaaaaaa.", "......c..c..", ".....c..c..c", "............" };
static const uint32_t k_weather_yarn[6] = { 0xfff3f0e8u, 0xfff0c95au, 0xff2b4f8fu };

#define WINDOWS_CHART(name, width, rows, yarn, flags) \
    { name, width, (uint32_t)(sizeof(rows) / sizeof((rows)[0])), yarn, flags, 0, 0, rows }

static const ChartView k_windows_chart_views[] = {
    WINDOWS_CHART("atelier-edge", 12, k_edge_rows, k_edge_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-store", 12, k_store_rows, k_store_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-copilot", 12, k_copilot_rows, k_copilot_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-github-copilot", 12, k_github_copilot_rows, k_github_copilot_yarn,
                  ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-zed", 12, k_zed_rows, k_zed_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-explorer", 12, k_explorer_rows, k_explorer_yarn,
                  ChartDefinedYarn | ChartSculptedYarn | ChartFittedRepeat),
    WINDOWS_CHART("atelier-solitaire", 12, k_solitaire_rows, k_solitaire_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-pale-blue-dot", 12, k_pale_blue_dot_rows, k_pale_blue_dot_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-get-started", 12, k_get_started_rows, k_get_started_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-steam", 12, k_steam_rows, k_steam_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-clipchamp", 12, k_clipchamp_rows, k_clipchamp_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-to-do", 12, k_to_do_rows, k_to_do_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-ms-photos", 12, k_ms_photos_rows, k_ms_photos_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-calculator", 16, k_calculator_rows, k_calculator_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-xbox", 12, k_xbox_rows, k_xbox_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-windows-app", 12, k_windows_app_rows, k_windows_app_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-windows-terminal", 12, k_windows_terminal_rows, k_windows_terminal_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-weather", 12, k_weather_rows, k_weather_yarn, ChartDefinedYarn | ChartSculptedYarn),
};

#undef WINDOWS_CHART

// upstream_match is NULL: these themes have no macOS source metadata.
static const ThemeView k_windows_theme_views[] = {
    { "microsoft-edge", "Microsoft Edge", NULL, "atelier-edge", 0xff1d5b93u,
      "Ocean blue, sea-glass teal, aqua, seafoam", "Curling wave scrolls on a seafoam line" },
    { "microsoft-store", "Microsoft Store", NULL, "atelier-store", 0xff1e3b66u,
      "Store navy, ivory, red, green, sky, gold", "Ivory tote bags with four-colour checks" },
    { "microsoft-copilot", "Microsoft Copilot", NULL, "atelier-copilot", 0xfff3ebdcu,
      "Oat cream, cornflower, sea teal, rose, apricot", "Interlocking pastel ribbon loops on oat knit" },
    { "github-copilot", "GitHub Copilot", NULL, "atelier-github-copilot", 0xff222733u,
      "Canvas ink, copilot violet, slate, pale lilac", "Linked pilot goggles on a slate strap" },
    { "zed", "Zed", NULL, "atelier-zed", 0xff1d2026u,
      "Ink, oat white, Zed blue", "Alternating ivory and blue Z stitches" },
    { "file-explorer", "File Explorer", NULL, "atelier-explorer", 0xfff5eedcu,
      "Parchment, folder gold, amber, Windows blue", "Folder tabs with breadcrumb chevrons" },
    { "microsoft-solitaire", "Microsoft Solitaire Collection", NULL, "atelier-solitaire", 0xff3f7a58u,
      "Felt green, cream, coral", "Cream spades and coral hearts on felt green knit" },
    { "pale-blue-dot", "Pale Blue Dot", NULL, "atelier-pale-blue-dot", 0xff131c33u,
      "Night navy, pale blue, cream, slate", "Sunbeam and pale blue dot on night-navy knit" },
    { "get-started", "Get Started", NULL, "atelier-get-started", 0xff2f6db5u,
      "Windows blue, cream bulb, amber spark, sky", "Cream lightbulbs and amber sparkles on blue knit" },
    { "steam", "Steam", NULL, "atelier-steam", 0xff233447u,
      "Steel navy, steam blue, vapour white, slate", "Blue gears with white steam puffs on navy knit" },
    { "clipchamp", "Clipchamp", NULL, "atelier-clipchamp", 0xff5b3fa0u,
      "Clipchamp violet, cream, teal, coral", "Coral clapperboard stripes between film sprockets on violet knit" },
    { "microsoft-to-do", "Microsoft To Do", NULL, "atelier-to-do", 0xff3b73c8u,
      "To Do blue, cream, sky blue", "Cream ticks beside sky boxes on blue knit" },
    { "microsoft-photos", "Microsoft Photos", NULL, "atelier-ms-photos", 0xff1f5fa8u,
      "Photos blue, cream, lagoon teal, sun gold, seafoam", "Cream peaks under golden suns over a teal shore" },
    { "windows-calculator", "Calculator", NULL, "atelier-calculator", 0xff3b3f46u,
      "Graphite, slate, blue, orange", "Plus, minus, times and equals stitches on graphite knit" },
    { "xbox", "Xbox", NULL, "atelier-xbox", 0xff1c211du,
      "Charcoal, Xbox green, coral, cornflower, butter", "Green X crosses beside four-colour button dots" },
    { "windows-app", "Windows App", NULL, "atelier-windows-app", 0xff2b5fa8u,
      "Windows blue, ice white, cyan, soft violet", "Cascading ice windows with cyan panes on blue knit" },
    { "windows-terminal", "Windows Terminal", NULL, "atelier-windows-terminal", 0xff0c0c0cu,
      "Campbell black, sage, cornflower, cream, rose, straw, teal", "Sage prompt chevrons and cream cursor blocks on black knit" },
    { "msn-weather", "Weather", NULL, "atelier-weather", 0xff5b9fd6u,
      "Sky blue, cream, sun gold, rain blue", "Sun, cloud and raindrops on sky blue knit" },
};
