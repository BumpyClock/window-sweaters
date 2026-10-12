// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Colorways\Recolor.h"
#include "..\..\src\Designs\Catalogue.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace window_sweaters;

namespace {
std::size_t checks = 0;
void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

struct Lab { double L, a, b; };
Lab ToLab(std::uint32_t argb) {
    const auto linear = [](std::uint32_t channel) {
        const double c = channel / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double r = linear((argb >> 16) & 255), g = linear((argb >> 8) & 255), b = linear(argb & 255);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}
double Chroma(std::uint32_t argb) { const auto c = ToLab(argb); return std::hypot(c.a, c.b); }
double Distance(std::uint32_t p, std::uint32_t q) {
    const auto a = ToLab(p), b = ToLab(q);
    return std::hypot(a.L - b.L, a.a - b.a, a.b - b.b);
}
double MinimumDistance(const std::vector<std::uint32_t>& yarns) {
    double nearest = 9;
    for (std::size_t i = 0; i < yarns.size(); ++i)
        for (std::size_t j = 0; j < i; ++j) nearest = std::min(nearest, Distance(yarns[i], yarns[j]));
    return nearest;
}
std::string Hex(std::uint32_t argb) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text = "#";
    for (int shift = 20; shift >= 0; shift -= 4) text += digits[(argb >> shift) & 15];
    return text;
}

struct Palette { std::uint32_t base; std::vector<std::uint32_t> yarns; };

const std::vector<Palette>& PrototypePalettes() {
    static const std::vector<Palette> palettes{
        {0xff1e3b66u, {0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au}},
        {0xfff5eedcu, {0xffeab447u, 0xffc98a2cu, 0xff3f80d0u, 0xff8fb8e8u, 0xffe58aa7u}},
        {0xff0c0c0cu, {0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu}},
        {0xfff3ebdcu, {0xff6f9bd8u, 0xff5fb3a6u, 0xffe58aa7u, 0xffeea066u}},
        {0xff131c33u, {0xff9ccbebu, 0xffe8dfbfu, 0xff4d6288u, 0xffdde6f2u}},
        {0xff1c211du, {0xff52b043u, 0xffe0605au, 0xff5b9be0u, 0xffefc94cu}},
        {0xff1d5b93u, {0xffe9f6f1u, 0xff4fbfa8u, 0xff6fc6eau, 0xff9fd59au}},
        {0xff2b5fa8u, {0xffe4eefbu, 0xff5ecbe8u, 0xffa990e0u}},
        {0xff5b9fd6u, {0xfff3f0e8u, 0xfff0c95au, 0xff2b4f8fu}},
        {0xff2f6db5u, {0xfffbf1d6u, 0xfff2b441u, 0xffa9cff0u}},
        {0xfff4f0e6u, {0xffd8675bu, 0xff6ba776u, 0xff4285f4u, 0xffedcc70u}},
        {0xfff8f0dbu, {0xffefa470u, 0xffeccb67u, 0xff95ba78u, 0xff7bbbc9u, 0xff9991c1u, 0xffd98bacu}},
        {0xffad7197u, {0xffeaaf96u, 0xffdcd092u, 0xff8dbccfu, 0xffafc5a1u, 0xffded3e9u}},
        {0xff542a52u, {0xff63c5dfu, 0xff82c5a6u, 0xffedc45eu, 0xffe986a3u}},
        {0xfff6f3ebu, {0xff579fdeu, 0xffe5787au, 0xffeba451u, 0xffccc6bau}},
        {0xfff5f5f2u, {0xff494947u}},
        {0xff5865f2u, {0xfff2eee4u}},
        {0xffe64e70u, {0xfff7acc0u, 0xfffff0dbu}},
        {0xff643a9au, {0xffff873eu, 0xffffc167u}},
        {0xff26251eu, {0xffc4c2b9u}},
        {0xff674c94u, {0xfff6f0deu, 0xffe0b0ffu}},
        {0xff2f5f85u, {0xff97aabcu, 0xfff2d06au}},
        {0xff7babbbu, {0xff2b677bu, 0xfff6f0deu}},
        {0xffd1495bu, {0xfff6f0deu, 0xffedae49u}},
        {0xff2b2d31u, {0xff5ec27du}},
        {0xfff2d768u, {0xff2e2a4au, 0xffe0577au}},
    };
    return palettes;
}

std::vector<Palette> CataloguePalettes() {
    std::vector<Palette> palettes;
    for (std::size_t i = 0; i < designs::ws_design_theme_count(); ++i) {
        const auto* theme = designs::ws_design_theme_at(i);
        const auto* chart = designs::ws_design_chart_by_name(theme->chart_name);
        if (!chart) continue;
        Palette palette{theme->base_argb, {}};
        for (std::uint32_t y = 0; y < chart->height; ++y)
            for (std::uint32_t x = 0; x < chart->width; ++x) {
                const auto argb = designs::ws_design_cell_argb(chart, x, y);
                if ((argb >> 24) >= 128 && std::find(palette.yarns.begin(), palette.yarns.end(), argb) == palette.yarns.end())
                    palette.yarns.push_back(argb);
            }
        if (!palette.yarns.empty()) palettes.push_back(std::move(palette));
    }
    return palettes;
}

constexpr std::uint32_t chosen_bases[] = {
    0xffe86aa0u, 0xff9caf88u, 0xff1b2a49u, 0xffd9a521u, 0xffb9a7e0u, 0xff3a3d42u,
    0xfff4ecd8u, 0xffff7f6bu, 0xfff8d3dfu, 0xff8a8a8au, 0xffffffffu, 0xff000000u,
};

void Golden() {
    struct Case { std::uint32_t base; std::vector<std::uint32_t> yarns; std::uint32_t chosen; std::vector<std::uint32_t> expected; };
    const Case cases[] = {
        {0xff1e3b66u, {0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au}, 0xffe86aa0u, {0xfffff0f5u, 0xfff2f47cu, 0xffbffeffu, 0xfff5ccfbu, 0xff8bfbbeu}},
        {0xff1e3b66u, {0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au}, 0xff1b2a49u, {0xffc0c9dbu, 0xffc76039u, 0xff82a84eu, 0xff689bd6u, 0xffd7b03eu}},
        {0xff1e3b66u, {0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au}, 0xfff8d3dfu, {0xff3d2f33u, 0xff838000u, 0xff007475u, 0xff81528bu, 0xff005833u}},
        {0xff1e3b66u, {0xfff4efe2u, 0xffe0705fu, 0xff8cbf6au, 0xff6fb3e8u, 0xfff0c95au}, 0xff8a8a8au, {0xfff5f3ecu, 0xfff5cec7u, 0xffc9fda8u, 0xffc5e5ffu, 0xffffe49cu}},
        {0xfff5eedcu, {0xffeab447u, 0xffc98a2cu, 0xff3f80d0u, 0xff8fb8e8u, 0xffe58aa7u}, 0xffe86aa0u, {0xffaf4b31u, 0xff833000u, 0xff003e50u, 0xff197382u, 0xff844176u}},
        {0xfff5eedcu, {0xffeab447u, 0xffc98a2cu, 0xff3f80d0u, 0xff8fb8e8u, 0xffe58aa7u}, 0xff1b2a49u, {0xff8f2d13u, 0xff008773u, 0xffcf658bu, 0xff7b4664u, 0xff467227u}},
        {0xfff5eedcu, {0xffeab447u, 0xffc98a2cu, 0xff3f80d0u, 0xff8fb8e8u, 0xffe58aa7u}, 0xfff8d3dfu, {0xffe79a52u, 0xffc76f47u, 0xff007c9fu, 0xff63b3c5u, 0xffc87eb5u}},
        {0xfff5eedcu, {0xffeab447u, 0xffc98a2cu, 0xff3f80d0u, 0xff8fb8e8u, 0xffe58aa7u}, 0xff8a8a8au, {0xff964019u, 0xff6e2000u, 0xff002856u, 0xff335883u, 0xff7d2c4bu}},
        {0xff0c0c0cu, {0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu}, 0xffe86aa0u, {0xfffff3dbu, 0xffafebfcu, 0xffdecfd4u, 0xffe8c1ffu, 0xfffbca8cu, 0xffbcfdceu}},
        {0xff0c0c0cu, {0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu}, 0xff1b2a49u, {0xff73dfd2u, 0xffdba5edu, 0xffc3ccdfu, 0xfffaa269u, 0xff91e48bu, 0xff93c4ffu}},
        {0xff0c0c0cu, {0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu}, 0xfff8d3dfu, {0xff273a00u, 0xff004959u, 0xff3a2b30u, 0xff5b2a70u, 0xff5c2407u, 0xff23593au}},
        {0xff0c0c0cu, {0xff8fbf7fu, 0xff6f9ae0u, 0xffe6e1d3u, 0xffd9737au, 0xffe0c070u, 0xff6fbfcfu}, 0xff8a8a8au, {0xffdcffd0u, 0xffeaf2ffu, 0xffd5d2cbu, 0xffe5b1b2u, 0xfff6dda0u, 0xffa8ecfau}},
        {0xff542a52u, {0xff63c5dfu, 0xff82c5a6u, 0xffedc45eu, 0xffe986a3u}, 0xffe86aa0u, {0xffdef6ffu, 0xff99fee7u, 0xffeaffa0u, 0xffebd6d5u}},
        {0xff542a52u, {0xff63c5dfu, 0xff82c5a6u, 0xffedc45eu, 0xffe986a3u}, 0xff1b2a49u, {0xff6cb284u, 0xffa2a367u, 0xfff29763u, 0xffad7cccu}},
        {0xff542a52u, {0xff63c5dfu, 0xff82c5a6u, 0xffedc45eu, 0xffe986a3u}, 0xfff8d3dfu, {0xff006984u, 0xff257869u, 0xff285c07u, 0xffa5494bu}},
        {0xff542a52u, {0xff63c5dfu, 0xff82c5a6u, 0xffedc45eu, 0xffe986a3u}, 0xff8a8a8au, {0xffcef4ffu, 0xffb5ffdcu, 0xffffeec7u, 0xfff5d6ddu}},
        {0xfff5f5f2u, {0xff494947u}, 0xffe86aa0u, {0xff210f16u}},
        {0xfff5f5f2u, {0xff494947u}, 0xff1b2a49u, {0xffbcc3d0u}},
        {0xfff5f5f2u, {0xff494947u}, 0xfff8d3dfu, {0xff40373au}},
        {0xfff5f5f2u, {0xff494947u}, 0xff8a8a8au, {0xff0f0f0eu}},
        {0xfff2d768u, {0xff2e2a4au, 0xffe0577au}, 0xffe86aa0u, {0xff00191cu, 0xff782d98u}},
        {0xfff2d768u, {0xff2e2a4au, 0xffe0577au}, 0xff1b2a49u, {0xffe4bba5u, 0xff006fb1u}},
        {0xfff2d768u, {0xff2e2a4au, 0xffe0577au}, 0xfff8d3dfu, {0xff043f49u, 0xffb676cdu}},
        {0xfff2d768u, {0xff2e2a4au, 0xffe0577au}, 0xff8a8a8au, {0xff0e0825u, 0xff801639u}},
        {0xfff6f3ebu, {0xff579fdeu, 0xffe5787au, 0xffeba451u, 0xffccc6bau}, 0xffe86aa0u, {0xff00516fu, 0xff87233cu, 0xff9f412eu, 0xff81656fu}},
        {0xfff6f3ebu, {0xff579fdeu, 0xffe5787au, 0xffeba451u, 0xffccc6bau}, 0xff1b2a49u, {0xff00898bu, 0xffa35191u, 0xff993842u, 0xff4b5362u}},
        {0xfff6f3ebu, {0xff579fdeu, 0xffe5787au, 0xffeba451u, 0xffccc6bau}, 0xfff8d3dfu, {0xff1f91c1u, 0xffca6777u, 0xffdd8a53u, 0xffbaa8aeu}},
        {0xfff6f3ebu, {0xff579fdeu, 0xffe5787au, 0xffeba451u, 0xffccc6bau}, 0xff8a8a8au, {0xff003d68u, 0xff731621u, 0xff893418u, 0xff605e58u}},
    };
    for (const auto& c : cases) {
        const auto actual = colorways::AdaptPalette(c.base, c.yarns, c.chosen);
        std::string text;
        for (auto argb : actual) text += Hex(argb) + " ";
        Check(actual == c.expected, "prototype golden " + Hex(c.base) + " -> " + Hex(c.chosen) + ": " + text);
    }
}

void Legibility(const std::vector<Palette>& palettes, const char* source) {
    for (const auto& palette : palettes)
        for (auto chosen : chosen_bases) {
            const auto adapted = colorways::AdaptPalette(palette.base, palette.yarns, chosen);
            const auto label = std::string(source) + " " + Hex(palette.base) + " -> " + Hex(chosen);
            Check(adapted.size() == palette.yarns.size(), label + " keeps yarn count");
            Check(adapted == colorways::AdaptPalette(palette.base, palette.yarns, chosen), label + " is deterministic");
            const double base_lightness = ToLab(chosen).L;
            const double tint = std::min(0.03, 0.3 * Chroma(chosen));
            for (std::size_t i = 0; i < adapted.size(); ++i) {
                Check((adapted[i] >> 24) == 255, label + " stays opaque");
                Check(std::abs(ToLab(adapted[i]).L - base_lightness) >= 0.15, label + " keeps lightness contrast with the base");
                const double original = Chroma(palette.yarns[i]);
                if (original < 0.045)
                    Check(Chroma(adapted[i]) <= 0.5 * original + tint + 0.085, label + " keeps neutrals quiet");
            }
            const double need = std::min(0.08, 0.85 * MinimumDistance(palette.yarns));
            Check(MinimumDistance(adapted) >= need - 0.005, label + " keeps yarns distinct");
        }
}

void Shape() {
    Check(colorways::AdaptPalette(0xff336699u, {}, 0xffe86aa0u).empty(), "empty palette adapts to empty");
    const std::vector<std::uint32_t> repeated{0xfff4efe2u, 0x00e0705fu, 0xfff4efe2u, 0xffe0705fu};
    const auto adapted = colorways::AdaptPalette(0xff1e3b66u, repeated, 0xffe86aa0u);
    Check(adapted.size() == 4 && adapted[0] == adapted[2] && adapted[1] == adapted[3] && adapted[0] != adapted[1],
        "repeated yarns adapt identically; input alpha is ignored");
    const std::vector<std::uint32_t> pair{0xfff4efe2u, 0xffe0705fu};
    const auto forward = colorways::AdaptPalette(0xff1e3b66u, pair, 0xffe86aa0u);
    Check(forward == std::vector<std::uint32_t>{adapted[0], adapted[1]}, "duplicates do not change distinct results");
    const auto lone = colorways::AdaptPalette(0xfff5f5f2u, std::vector<std::uint32_t>{0xff494947u}, 0xff1b2a49u);
    Check(Chroma(lone[0]) <= 0.5 * Chroma(0xff494947u) + 0.03 + 0.002, "a lone neutral stays a tinted neutral");
}
}

std::size_t RecolorTests() {
    Golden();
    Shape();
    Legibility(PrototypePalettes(), "prototype");
    Legibility(CataloguePalettes(), "catalogue");
    return checks;
}
