// SPDX-License-Identifier: GPL-3.0-only
#include "../../src/Designs/Catalogue.h"
#include "ExpectedCatalogue.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>

using namespace window_sweaters::designs;

extern "C" int catalogue_c_smoke(void);

static_assert(std::size(expected_charts) == 47);
static_assert(std::size(expected_themes) == 37);
constexpr std::size_t pinned_alias_count = 44;
static_assert(std::is_same_v<decltype(ws_design_chart_at(0)), const ChartView*>);
static_assert(std::is_same_v<decltype(ws_design_theme_at(0)), const ThemeView*>);
static_assert(std::is_same_v<decltype(ChartView::cells), const char* const*>);
static_assert(std::is_same_v<decltype(ChartView::palette_argb), const uint32_t*>);

static void check(bool condition, const char* contract, const char* name = "") {
    if (!condition)
        throw std::runtime_error(std::string(contract) + ": " + name);
}

static bool equal(const char* actual, const char* expected) {
    return actual && std::strcmp(actual, expected) == 0;
}

static void test_complete_charts() {
    check(ws_design_chart_count() >= std::size(expected_charts), "Every pinned repeat is present");
    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < std::size(expected_charts); ++i) {
        const auto& expected = expected_charts[i];
        const auto* chart = ws_design_chart_by_name(expected.name);
        check(chart != nullptr, "Every authored chart is selectable", expected.name);
        check(chart == ws_design_chart_at(i), "Pinned chart order", expected.name);
        check(equal(chart->name, expected.name), "Stable chart name", expected.name);
        check(names.emplace(chart->name).second, "Unique chart names", expected.name);
        check(chart->width == expected.width && chart->height == expected.height,
              "Literal dimensions, including legacy repeats", expected.name);
        for (std::size_t yarn = 0; yarn < 6; ++yarn)
            check(chart->palette_argb[yarn] == expected.palette[yarn],
                  "Literal ARGB palette and zero unused slots", expected.name);

        std::uint64_t hash = 14695981039346656037ull;
        bool has_base = false;
        bool has_contrast = false;
        for (std::uint32_t y = 0; y < chart->height; ++y) {
            check(chart->cells[y] && std::strlen(chart->cells[y]) == chart->width,
                  "Uniform authored row width", expected.name);
            for (std::uint32_t x = 0; x < chart->width; ++x) {
                const char cell = chart->cells[y][x];
                check(cell == '.' || (cell >= 'a' && cell <= 'f'),
                      "Original cell alphabet", expected.name);
                hash = (hash ^ static_cast<unsigned char>(cell)) * 1099511628211ull;
                const auto color = ws_design_authored_cell_argb(chart, x, y);
                const auto expected_color = cell == '.' ? 0 : expected.palette[cell - 'a'];
                check(color == expected_color, "Top-down literal cell sampling", expected.name);
                const auto rendered = ws_design_cell_argb(chart, x, y);
                const auto expected_rendered =
                    ((chart->flags & ChartOuterCuff) && color == chart->cuff_argb) ? 0 : color;
                check(rendered == expected_rendered, "Cuff extraction leaves rows unchanged", expected.name);
                has_base |= rendered == 0;
                has_contrast |= rendered != 0;
                check(rendered == 0 || (rendered >> 24) == 255,
                      "Base sentinel or opaque ARGB", expected.name);
            }
            hash = (hash ^ '\n') * 1099511628211ull;
        }
        check(hash == expected.rows_hash, "Every original authored row matches pinned fingerprint", expected.name);
        check(has_base && (has_contrast || chart->cuff_argb),
              "Actual base plus contrast/cuff data, not a placeholder", expected.name);
    }
}

static void test_catalogue_invariants() {
    constexpr std::uint32_t known_flags = ChartSolidCorners | ChartOuterCuff | ChartRoundDots
        | ChartFittedRepeat | ChartSculptedYarn | ChartDefinedYarn;
    std::unordered_set<std::string> names, referenced;
    for (std::size_t i = 0; i < ws_design_chart_count(); ++i) {
        const auto* chart = ws_design_chart_at(i);
        check(chart && chart->name && names.emplace(chart->name).second, "Unique chart names", chart ? chart->name : "");
        check(ws_design_chart_by_name(chart->name) == chart, "Every chart resolves by name", chart->name);
        check(chart->width > 0 && chart->height > 0 && chart->height <= 12, "Chart dimensions in range", chart->name);
        check((chart->flags & ~known_flags) == 0 && (chart->flags & ChartDefinedYarn), "Known material flags", chart->name);
        bool has_base = false, has_contrast = false;
        for (std::uint32_t y = 0; y < chart->height; ++y) {
            check(chart->cells[y] && std::strlen(chart->cells[y]) == chart->width, "Uniform row width", chart->name);
            for (std::uint32_t x = 0; x < chart->width; ++x) {
                const char cell = chart->cells[y][x];
                check(cell == '.' || (cell >= 'a' && cell <= 'f'), "Cell alphabet", chart->name);
                const auto color = ws_design_authored_cell_argb(chart, x, y);
                check(cell == '.' || (color >> 24) == 255, "Used palette slots are opaque ARGB", chart->name);
                has_base |= color == 0;
                has_contrast |= color != 0;
            }
        }
        check(has_base && has_contrast, "Base plus contrast data, not a placeholder", chart->name);
    }
    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < ws_design_theme_count(); ++i) {
        const auto* theme = ws_design_theme_at(i);
        check(theme && theme->id && ids.emplace(theme->id).second, "Unique semantic theme IDs", theme ? theme->id : "");
        check(ws_design_theme_by_id(theme->id) == theme, "Every theme resolves by ID", theme->id);
        check(theme->name && *theme->name && (theme->base_argb >> 24) == 255, "Named opaque base yarn", theme->id);
        check(ws_design_chart_by_name(theme->chart_name) != nullptr, "Every theme chart resolves", theme->id);
        check(theme->palette_description && *theme->palette_description
              && theme->pattern_description && *theme->pattern_description,
              "Presentation metadata present", theme->id);
        check((i < std::size(expected_themes)) == (theme->upstream_match != nullptr),
              "Only pinned themes carry macOS source metadata", theme->id);
        referenced.emplace(theme->chart_name);
    }
    for (std::size_t i = std::size(expected_charts); i < ws_design_chart_count(); ++i)
        check(referenced.contains(ws_design_chart_at(i)->name),
              "Every Windows-authored chart belongs to a theme", ws_design_chart_at(i)->name);
}

static void test_material_flags() {
    const std::unordered_set<std::string> fitted = {
        "atelier-finder", "atelier-terminal", "atelier-grok",
        "atelier-granola", "atelier-illustrator", "atelier-chrome"
    };
    for (std::size_t i = 0; i < std::size(expected_charts); ++i) {
        const auto* chart = ws_design_chart_at(i);
        const std::string name = chart->name;
        std::uint32_t flags = ChartDefinedYarn | ChartSculptedYarn;
        if (name == "atelier-messages")
            flags = ChartDefinedYarn | ChartRoundDots;
        if (name == "atelier-whatsapp")
            flags |= ChartSolidCorners;
        if (name == "atelier-notes" || name == "atelier-calendar")
            flags |= ChartOuterCuff;
        if (fitted.contains(name))
            flags |= ChartFittedRepeat;
        check(chart->flags == flags, "Exact original material/corner/cuff/repeat flags", chart->name);
        check(chart->corner_argb == 0, "Solid corners follow selected base yarn", chart->name);
        const auto cuff = name == "atelier-notes" ? 0xffefc852u
                        : name == "atelier-calendar" ? 0xffe55c52u : 0;
        check(chart->cuff_argb == cuff, "Literal continuous outer cuff color", chart->name);
    }
    const auto* notes = ws_design_chart_by_name("atelier-notes");
    check(ws_design_authored_cell_argb(notes, 0, 0) == 0xffefc852u,
          "Notes original golden row retained");
    check(ws_design_cell_argb(notes, 0, 0) == 0, "Notes golden cuff removed from rectangular repeat");
    check(ws_design_cell_argb(notes, 0, 5) == 0xfffffcf3u, "Notes inner ivory row retained");
    const auto* calendar = ws_design_chart_by_name("atelier-calendar");
    check(ws_design_authored_cell_argb(calendar, 11, 1) == 0xffe55c52u,
          "Calendar original red row retained");
    check(ws_design_cell_argb(calendar, 11, 1) == 0, "Calendar red cuff removed from rectangular repeat");
    const auto* photos = ws_design_chart_by_name("atelier-photos");
    check(ws_design_cell_argb(photos, 11, 5) == 0xffd98bacu, "Sixth contrast f is literal ARGB");
    const auto* checker = ws_design_chart_by_name("checker");
    check(ws_design_cell_argb(checker, 0, 0) == 0
          && ws_design_cell_argb(checker, 0, 3) == 0xfff2eee4u, "Authored rows are not vertically flipped");
}

static void test_complete_themes() {
    check(ws_design_theme_count() >= std::size(expected_themes), "Every pinned app theme is present");
    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < std::size(expected_themes); ++i) {
        const auto& expected = expected_themes[i];
        const auto* theme = ws_design_theme_by_id(expected.id);
        check(theme != nullptr, "Every authored theme is manually selectable", expected.id);
        check(theme == ws_design_theme_at(i), "Pinned presentation order", expected.id);
        check(equal(theme->id, expected.id) && ids.emplace(theme->id).second,
              "Unique semantic persistent ID", expected.id);
        check(equal(theme->name, expected.name), "Original display name", expected.id);
        check(equal(theme->upstream_match, expected.match), "Original source metadata, not invented Windows identity", expected.id);
        check(equal(theme->chart_name, expected.chart), "Exact theme-to-chart mapping", expected.id);
        check(theme->base_argb == expected.base, "Literal authored base ARGB", expected.id);
        check(ws_design_chart_by_name(theme->chart_name) != nullptr, "Every theme chart resolves", expected.id);
        check(theme->palette_description && *theme->palette_description
              && theme->pattern_description && *theme->pattern_description,
              "Source presentation metadata retained", expected.id);
    }
    check(equal(ws_design_theme_by_id("codex")->chart_name, "atelier-chatgpt")
          && equal(ws_design_theme_by_id("chatgpt")->chart_name, "atelier-chatgpt"),
          "Codex and ChatGPT share original atelier-chatgpt");
    check(ws_design_theme_by_id("codex") != ws_design_theme_by_id("chatgpt"),
          "Shared chart does not merge semantic theme identity");
    check(ws_design_chart_by_name("atelier-codex") != nullptr,
          "Unassigned atelier-codex repeat remains selectable");
    check(equal(ws_design_theme_by_id("discord")->chart_name, "checker"), "Discord keeps legacy checker");
    check(ws_design_chart_by_name("atelier-discord") == nullptr, "No invented Discord chart");
    check(equal(ws_design_theme_by_id("notes")->pattern_description,
                "Ivory knit with a continuous curved golden cuff"), "Notes original cuff description");
    check(equal(ws_design_theme_by_id("finder")->palette_description,
                "Finder blue, ice blue, navy"), "Finder original palette description");
    check(ws_design_theme_by_id("safari") && ws_design_theme_by_id("apple-music"),
          "macOS-only themes remain manually selectable");
}

static void test_source_aliases() {
    check(ws_design_theme_alias_count() == pinned_alias_count, "All original source aliases, none invented");
    std::unordered_set<std::string> aliases;
    for (std::size_t i = 0; i < ws_design_theme_alias_count(); ++i) {
        const auto* alias = ws_design_theme_alias_at(i);
        check(alias && aliases.emplace(alias->upstream_match).second, "Unique original aliases");
        check(ws_design_theme_by_id(alias->theme_id) != nullptr, "Alias resolves to semantic theme");
    }
    const std::array<ThemeAliasView, 7> extra = {{
        { "MSTeams", "microsoft-teams" }, { "Teams", "microsoft-teams" },
        { "Chrome", "google-chrome" }, { "Chromium", "google-chrome" },
        { "Zoom", "zoom" }, { "Apple Music", "apple-music" }, { "Visual Studio Code", "vs-code" }
    }};
    for (const auto& expected : extra) {
        bool found = false;
        for (std::size_t i = 0; i < ws_design_theme_alias_count(); ++i) {
            const auto* alias = ws_design_theme_alias_at(i);
            if (equal(alias->upstream_match, expected.upstream_match))
                found = equal(alias->theme_id, expected.theme_id);
        }
        check(found, "Exact original alternate process-name metadata", expected.upstream_match);
    }
    for (const auto& theme : expected_themes) {
        bool found = false;
        for (std::size_t i = 0; i < ws_design_theme_alias_count(); ++i) {
            const auto* alias = ws_design_theme_alias_at(i);
            found |= equal(alias->upstream_match, theme.match) && equal(alias->theme_id, theme.id);
        }
        check(found, "Original primary match alias", theme.id);
    }
}

static void test_read_only_contract() {
    check(catalogue_c_smoke() == 1, "C17 consumer links to same immutable C API");
    check(!ws_design_chart_at(ws_design_chart_count()) && !ws_design_chart_at(std::numeric_limits<std::size_t>::max()),
          "Out-of-range chart indices rejected");
    check(!ws_design_theme_at(ws_design_theme_count()) && !ws_design_theme_at(std::numeric_limits<std::size_t>::max())
          && !ws_design_theme_alias_at(ws_design_theme_alias_count()),
          "Out-of-range theme and alias indices rejected");
    check(!ws_design_chart_by_name(nullptr) && !ws_design_chart_by_name("")
          && !ws_design_chart_by_name("Atelier-Finder") && !ws_design_chart_by_name("missing"),
          "Chart identifiers require exact stable names");
    check(!ws_design_theme_by_id(nullptr) && !ws_design_theme_by_id("")
          && !ws_design_theme_by_id("Finder") && !ws_design_theme_by_id("missing"),
          "Theme identifiers require exact stable semantic IDs");
    const auto* chart = ws_design_chart_by_name("atelier-finder");
    check(ws_design_cell_argb(nullptr, 0, 0) == 0
          && ws_design_authored_cell_argb(nullptr, 0, 0) == 0
          && ws_design_cell_argb(chart, 16, 0) == 0
          && ws_design_cell_argb(chart, 0, 6) == 0
          && ws_design_cell_argb(chart, UINT32_MAX, UINT32_MAX) == 0,
          "Invalid/null/boundary cell requests return base sentinel safely");

    std::atomic<bool> stable = true;
    std::array<std::thread, 4> readers;
    for (auto& reader : readers)
        reader = std::thread([&] {
            for (int i = 0; i < 1000; ++i)
                if (ws_design_chart_by_name("atelier-finder") != chart
                    || ws_design_cell_argb(chart, 2, 0) != 0xffb7e4f7u
                    || ws_design_theme_by_id("finder")->base_argb != 0xff258de0u)
                    stable = false;
        });
    for (auto& reader : readers)
        reader.join();
    check(stable.load(), "Concurrent reads preserve process-lifetime views without shared chart state");
}

int main() {
    try {
        test_complete_charts();
        test_catalogue_invariants();
        test_material_flags();
        test_complete_themes();
        test_source_aliases();
        test_read_only_contract();
        std::printf("PASS: %zu themes (%zu pinned), %zu charts (%zu exact pinned repeats), %zu source aliases\n",
                    ws_design_theme_count(), std::size(expected_themes),
                    ws_design_chart_count(), std::size(expected_charts), ws_design_theme_alias_count());
        std::puts("PASS: every row fingerprint, literal palette/dimensions, special flags, C17/C++20 read-only API");
        std::puts("Data parity only; material variants and WinUI integration are not complete.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
