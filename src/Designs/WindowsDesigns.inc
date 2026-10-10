// SPDX-License-Identifier: GPL-3.0-only
// Authored for the Windows port; not imported.
// Same chart contract as AuthoredCharts.inc: top-down rows, '.' is base yarn, a..f index the yarn.

static const char* const k_edge_rows[] = {
    "......aaa.........aaa...",
    "....aabbba......aaccca..",
    "...abbb..ba....accc..ca.",
    "..abbb....a...accc....a.",
    ".abbbb.......acccc......",
    "dddddddddddddddddddddddd",
};
static const uint32_t k_edge_yarn[6] = { 0xffe9f6f1u, 0xff4fbfa8u, 0xff6fc6eau, 0xff9fd59au };

static const char* const k_store_rows[] = {
    "a.bb.aa.cc.aa.dd.aa.ee.a",
    ".bbbb..cccc..dddd..eeee.",
    "bbbbbbccccccddddddeeeeee",
    "bbbbbbccccccddddddeeeeee",
    ".bbbb..cccc..dddd..eeee.",
    "a.bb.aa.cc.aa.dd.aa.ee.a",
};
static const uint32_t k_store_yarn[6] = { 0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au };

static const char* const k_copilot_rows[] = {
    "....aaaa....................cccc................",
    "..aaaaaaaa..b....a.....b..cccccccc..d....c.....d",
    ".aaa.d..aaa.bb..aaa...bb.ccc.b..ccc.dd..ccc...dd",
    "aa..ddd...aa.bbb.a..bbb.cc..bbb...cc.ddd.c..ddd.",
    "a....d.....a..bbbbbbbb..c....b.....c..dddddddd..",
    "................bbbb....................dddd....",
};
static const uint32_t k_copilot_yarn[6] = { 0xff6f9bd8u, 0xff5fb3a6u, 0xffe58aa7u, 0xffeea066u };

static const char* const k_github_copilot_rows[] = {
    "dd.bb.cc.aa.cc.dd.bb.dd.",
    "dd.bb.cc.aa.cc.dd.bb.dd.",
    "........................",
    "cc.dd.bb.dd.cc.aa.dd.bb.",
    "cc.dd.bb.dd.cc.aa.dd.bb.",
    "........................",
};
static const uint32_t k_github_copilot_yarn[6] = { 0xffa184e6u, 0xff0e4429u, 0xff26a641u, 0xff39d353u };

static const char* const k_zed_rows[] = {
    "aa..b...aa..b...",
    "a..b...aa..b...a",
    "..b...aa..b...aa",
    ".b...aa..b...aa.",
    "b...aa..b...aa..",
    "...aa..b...aa..b",
};
static const uint32_t k_zed_yarn[6] = { 0xffefece3u, 0xff4f7fdcu };

static const char* const k_explorer_rows[] = {
    "aa....bb....aa....bb....",
    "aaaaaabbbbbbaaaaaabbbbbb",
    "aaaaaabbbbbbaaaaaabbbbbb",
    "........................",
    "cc.dd.cc.ee.cc.dd.cc.ee.",
    ".cc.dd.cc.ee.cc.dd.cc.ee",
};
static const uint32_t k_explorer_yarn[6] = { 0xffeab447u, 0xffc98a2cu, 0xff3f80d0u, 0xff8fb8e8u, 0xffe58aa7u };

static const char* const k_solitaire_rows[] = {
    "..aa....bb..",
    ".aaaa..bbbb.",
    "aaaaaabbbbbb",
    "aaaaaabbbbbb",
    ".aaaa..bbbb.",
    "..aa....bb..",
};
static const uint32_t k_solitaire_yarn[6] = { 0xfff2ead8u, 0xffd8675fu };

static const char* const k_pale_blue_dot_rows[] = {
    "c......d.....b.........d......",
    ".c..........b.....d...........",
    "..c...d....b..............d...",
    "...c......b.......a...........",
    "....c....b...d..........d.....",
    ".....c..b.................d...",
};
static const uint32_t k_pale_blue_dot_yarn[6] = { 0xff9ccbebu, 0xffe8dfbfu, 0xff4d6288u, 0xffdde6f2u };

static const char* const k_get_started_rows[] = {
    "........aa.b",
    "......aa....",
    "....aa......",
    "..aa........",
    "aa..........",
    "c..c..c..c..",
};
static const uint32_t k_get_started_yarn[6] = { 0xfffbf1d6u, 0xfff2b441u, 0xffa9cff0u };

static const char* const k_steam_rows[] = {
    ".bbb.....bbb....",
    "bbbbb...bbbbb...",
    ".bbb.....bbb....",
    ".....aaa.....aaa",
    "a...aaaaa...aaaa",
    ".....aaa.....aaa",
};
static const uint32_t k_steam_yarn[6] = { 0xff7fb5d9u, 0xffdfe8eeu };

static const char* const k_clipchamp_rows[] = {
    "a.a.a.a.a.a.a.a.",
    "cccc....bbbb....",
    "cccc....bbbb....",
    "....bbbb....cccc",
    "....bbbb....cccc",
    ".a.a.a.a.a.a.a.a",
};
static const uint32_t k_clipchamp_yarn[6] = { 0xfff1ead8u, 0xff4fb3a9u, 0xffe9806eu };

static const char* const k_to_do_rows[] = {
    "......a........a........a..",
    "...c.a........a........a...",
    "a...a....a...a....a...a....",
    ".a.a......a.a......a.a.....",
    "..a........a........a......",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbb",
};
static const uint32_t k_to_do_yarn[6] = { 0xfff4f1e6u, 0xff9ccdf0u, 0xfff0c95au };

static const char* const k_ms_photos_rows[] = {
    "...a.......c....",
    "..aba...........",
    ".bbbbb.....a....",
    "bbbbbbb...aba..c",
    "bbbbbbbb.bbbbb..",
    "dddddddddddddddd",
};
static const uint32_t k_ms_photos_yarn[6] = { 0xfff3ecd8u, 0xff3fb0b0u, 0xfff2c35au, 0xff8fd3d0u };

static const char* const k_calculator_rows[] = {
    ".a.....b....",
    "aaa...b.b...",
    ".a.....b....",
    "...c.c....a.",
    "....c....aaa",
    "...c.c....a.",
};
static const uint32_t k_calculator_yarn[6] = { 0xffd7dce3u, 0xff5b9bd5u, 0xffee9a4du };

static const char* const k_xbox_rows[] = {
    "a...a...a...a...a...a...a...a...",
    ".a.a..d..a.a.....a.a..c..a.a....",
    "..a.......a.......a.......a.....",
    ".a.a.....a.a..b..a.a.....a.a..a.",
    "a...a...a...a...a...a...a...a...",
    "................................",
};
static const uint32_t k_xbox_yarn[6] = { 0xff52b043u, 0xffe0605au, 0xff5b9be0u, 0xffefc94cu };

static const char* const k_windows_app_rows[] = {
    "aaaa..bbbb..cccc....",
    "aaaa..bbbb..cccc....",
    "..aaaa..bbbb..cccc..",
    "..aaaa..bbbb..cccc..",
    "....aaaa..bbbb..cccc",
    "....aaaa..bbbb..cccc",
};
static const uint32_t k_windows_app_yarn[6] = { 0xffe4eefbu, 0xff5ecbe8u, 0xffa990e0u };

static const char* const k_windows_terminal_rows[] = {
    "aa....bb....dd....ee....ff....cc....",
    ".aa....bb....dd....ee....ff....cc...",
    "..aa....bb....dd....ee....ff....cc..",
    "..aa....bb....dd....ee....ff....cc..",
    ".aa....bb....dd....ee....ff....cc...",
    "aa....bb....dd....ee....ff....cc....",
};
static const uint32_t k_windows_terminal_yarn[6] = { 0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu };

static const char* const k_weather_rows[] = {
    "c.....a.....",
    ".c.....a....",
    "...a.....c..",
    "....a.....c.",
    "a.....c.....",
    ".a.....c..b.",
};
static const uint32_t k_weather_yarn[6] = { 0xfff3f0e8u, 0xfff0c95au, 0xff2b4f8fu };

#define WINDOWS_CHART(name, width, rows, yarn, flags) \
    { name, width, (uint32_t)(sizeof(rows) / sizeof((rows)[0])), yarn, flags, 0, 0, rows }

static const ChartView k_windows_chart_views[] = {
    WINDOWS_CHART("atelier-edge", 24, k_edge_rows, k_edge_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-store", 24, k_store_rows, k_store_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-copilot", 48, k_copilot_rows, k_copilot_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-github-copilot", 24, k_github_copilot_rows, k_github_copilot_yarn,
                  ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-zed", 16, k_zed_rows, k_zed_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-explorer", 24, k_explorer_rows, k_explorer_yarn,
                  ChartDefinedYarn | ChartSculptedYarn | ChartFittedRepeat),
    WINDOWS_CHART("atelier-solitaire", 12, k_solitaire_rows, k_solitaire_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-pale-blue-dot", 30, k_pale_blue_dot_rows, k_pale_blue_dot_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-get-started", 12, k_get_started_rows, k_get_started_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-steam", 16, k_steam_rows, k_steam_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-clipchamp", 16, k_clipchamp_rows, k_clipchamp_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-to-do", 27, k_to_do_rows, k_to_do_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-ms-photos", 16, k_ms_photos_rows, k_ms_photos_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-calculator", 12, k_calculator_rows, k_calculator_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-xbox", 32, k_xbox_rows, k_xbox_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-windows-app", 20, k_windows_app_rows, k_windows_app_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-windows-terminal", 36, k_windows_terminal_rows, k_windows_terminal_yarn, ChartDefinedYarn | ChartSculptedYarn),
    WINDOWS_CHART("atelier-weather", 12, k_weather_rows, k_weather_yarn, ChartDefinedYarn | ChartSculptedYarn),
};

#undef WINDOWS_CHART

static const ThemeView k_windows_theme_views[] = {
    { "microsoft-edge", "Microsoft Edge", NULL, "atelier-edge", 0xff1d5b93u,
      "Ocean blue, foam white, sea-glass teal, aqua, seafoam", "Foam-capped breakers curling over a seafoam line" },
    { "microsoft-store", "Microsoft Store", NULL, "atelier-store", 0xff1e3b66u,
      "Store navy, ivory, red, green, sky, gold", "Argyle diamonds cycling red, green, sky and gold with an ivory overcheck" },
    { "microsoft-copilot", "Microsoft Copilot", NULL, "atelier-copilot", 0xfff3ebdcu,
      "Oat cream, cornflower, sea teal, rose, apricot", "Swooping ribbon that changes colour, with sparkles in each hollow" },
    { "github-copilot", "GitHub Copilot", NULL, "atelier-github-copilot", 0xff222733u,
      "Canvas ink, copilot violet, contribution greens", "Contribution-graph tweed of green squares with violet highlights" },
    { "zed", "Zed", NULL, "atelier-zed", 0xff1d2026u,
      "Ink, oat white, Zed blue", "Racing diagonal stripes in ivory and blue" },
    { "file-explorer", "File Explorer", NULL, "atelier-explorer", 0xfff5eedcu,
      "Parchment, folder gold, amber, Windows blue, sky, rose", "Stepped gold folder-tab stripe over breadcrumb stitches" },
    { "microsoft-solitaire", "Microsoft Solitaire Collection", NULL, "atelier-solitaire", 0xff3f7a58u,
      "Felt green, cream, coral", "Harlequin card-back diamonds in cream and coral" },
    { "pale-blue-dot", "Pale Blue Dot", NULL, "atelier-pale-blue-dot", 0xff131c33u,
      "Night navy, pale blue, cream, slate, starlight", "Sunbeams slanting across a starfield, with one pale blue dot" },
    { "get-started", "Get Started", NULL, "atelier-get-started", 0xff2f6db5u,
      "Windows blue, cream, amber, sky", "Cream stairs climbing to amber sparks over a sky dotted line" },
    { "steam", "Steam", NULL, "atelier-steam", 0xff233447u,
      "Steel navy, steam blue, vapour white", "Staggered puffs of vapour white and steam blue" },
    { "clipchamp", "Clipchamp", NULL, "atelier-clipchamp", 0xff5b3fa0u,
      "Clipchamp violet, cream, teal, coral", "Coral and teal checks between cream perforations" },
    { "microsoft-to-do", "Microsoft To Do", NULL, "atelier-to-do", 0xff3b73c8u,
      "To Do blue, cream, sky blue, gold", "Running cream ticks over a sky ruled line, with a gold dot" },
    { "microsoft-photos", "Microsoft Photos", NULL, "atelier-ms-photos", 0xff1f5fa8u,
      "Photos blue, cream, lagoon teal, sun gold, seafoam", "Fair Isle peaks with cream tips and gold dots over a seafoam hem" },
    { "windows-calculator", "Calculator", NULL, "atelier-calculator", 0xff3b3f46u,
      "Graphite, silver, blue, orange", "Silver, blue and orange plus and times stitches in a staggered check" },
    { "xbox", "Xbox", NULL, "atelier-xbox", 0xff1c211du,
      "Charcoal, Xbox green, coral, cornflower, butter", "Green kiss stitches with yellow, red, blue and green button dots" },
    { "windows-app", "Windows App", NULL, "atelier-windows-app", 0xff2b5fa8u,
      "Windows blue, ice white, cyan, soft violet", "Ice, cyan and violet panes cascading in steps" },
    { "windows-terminal", "Windows Terminal", NULL, "atelier-windows-terminal", 0xff0c0c0cu,
      "Campbell black, sage, cornflower, cream, rose, straw, teal", "Prompt chevrons marching through the Campbell colours" },
    { "msn-weather", "Weather", NULL, "atelier-weather", 0xff5b9fd6u,
      "Sky blue, cream, sun gold, rain blue", "Slanting rain dashes in cream and rain blue, with a gold glint" },
};
