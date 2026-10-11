#include "IconPalette.h"
#include "IconMotif.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

// ZigzagContrast is adapted from saragordic/window-sweaters src/autoyarn.m @ a49dff52483dafdbe27f972bd271cd28d37cef1c (GPL-3.0).
namespace window_sweaters::icon_yarn {
namespace {

struct Hsl { double h, s, l; };
constexpr std::uint32_t Cream = 0xfff6f0deu;

Hsl ToHsl(std::uint32_t argb) noexcept {
    const double r = ((argb >> 16) & 255) / 255.0;
    const double g = ((argb >> 8) & 255) / 255.0;
    const double b = (argb & 255) / 255.0;
    const double high = std::max({r, g, b}), low = std::min({r, g, b});
    Hsl value{0, 0, (high + low) / 2};
    if (high == low) return value;
    const double delta = high - low;
    value.s = value.l > .5 ? delta / (2 - high - low) : delta / (high + low);
    if (high == r) value.h = (g - b) / delta + (g < b ? 6 : 0);
    else if (high == g) value.h = (b - r) / delta + 2;
    else value.h = (r - g) / delta + 4;
    value.h /= 6;
    return value;
}

std::uint32_t FromHsl(Hsl value) noexcept {
    const double q = value.l < .5 ? value.l * (1 + value.s)
                                 : value.l + value.s - value.l * value.s;
    const double p = 2 * value.l - q;
    std::uint32_t color = 0xff000000u;
    const double channels[]{value.h + 1.0 / 3, value.h, value.h - 1.0 / 3};
    for (unsigned i = 0; i < 3; ++i) {
        double t = channels[i];
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        const double c = t < 1.0 / 6 ? p + (q - p) * 6 * t
                       : t < .5 ? q
                       : t < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - t) * 6 : p;
        color |= static_cast<std::uint32_t>(c * 255) << (16 - i * 8);
    }
    return color;
}

struct Rgb {
    int r{}, g{}, b{};
    auto operator<=>(const Rgb&) const = default;
};
struct Straight { Rgb color; std::uint8_t a{}; };
using Raw = std::array<Rgb, 3>;
using Floats = std::array<double, 3>;
struct Hls { double h, l, s; };
constexpr Rgb Gold{237, 174, 73};

Rgb FromArgb(std::uint32_t argb) noexcept {
    return {static_cast<int>((argb >> 16) & 255), static_cast<int>((argb >> 8) & 255), static_cast<int>(argb & 255)};
}
int Luma(Rgb c) noexcept { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }
int Distance(Rgb a, Rgb b) noexcept {
    return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
}
Rgb Mix(Rgb a, Rgb b, int n, int den) noexcept {
    return {(a.r * (den - n) + b.r * n) / den, (a.g * (den - n) + b.g * n) / den, (a.b * (den - n) + b.b * n) / den};
}

double Wrap(double value) noexcept {
    const double mod = std::fmod(value, 1.0);
    return mod == 0 ? 0.0 : mod < 0 ? mod + 1.0 : mod;
}

Hls RgbToHls(Floats c) noexcept {
    const double high = std::max({c[0], c[1], c[2]}), low = std::min({c[0], c[1], c[2]});
    const double sum = high + low, range = high - low, l = sum / 2.0;
    if (low == high) return {0.0, l, 0.0};
    const double s = l <= 0.5 ? range / sum : range / (2.0 - high - low);
    const double rc = (high - c[0]) / range, gc = (high - c[1]) / range, bc = (high - c[2]) / range;
    const double h = c[0] == high ? bc - gc : c[1] == high ? 2.0 + rc - bc : 4.0 + gc - rc;
    return {Wrap(h / 6.0), l, s};
}

double HueValue(double m1, double m2, double hue) noexcept {
    hue = Wrap(hue);
    if (hue < 1.0 / 6.0) return m1 + (m2 - m1) * hue * 6.0;
    if (hue < 0.5) return m2;
    if (hue < 2.0 / 3.0) return m1 + (m2 - m1) * (2.0 / 3.0 - hue) * 6.0;
    return m1;
}

Floats HlsToRgb(double h, double l, double s) noexcept {
    if (s == 0.0) return {l, l, l};
    const double m2 = l <= 0.5 ? l * (1.0 + s) : l + s - (l * s);
    const double m1 = 2.0 * l - m2;
    return {HueValue(m1, m2, h + 1.0 / 3.0), HueValue(m1, m2, h), HueValue(m1, m2, h - 1.0 / 3.0)};
}

Floats Unit(Rgb c) noexcept { return {c.r / 255.0, c.g / 255.0, c.b / 255.0}; }

std::uint32_t Argb(Floats c) noexcept {
    std::uint32_t argb = 0xff000000u;
    for (unsigned i = 0; i < 3; ++i)
        argb |= static_cast<std::uint32_t>(std::nearbyint(c[i] * 255)) << (16 - i * 8);
    return argb;
}

Floats Soft(Rgb raw, double& lightness) noexcept {
    const auto hls = RgbToHls(Unit(raw));
    const double s = hls.s < 0.08 ? 0.0 : std::min(.60, std::max(.22, hls.s * .48));
    lightness = std::min(.72, std::max(.30, hls.l * .85 + .10));
    return HlsToRgb(hls.h, lightness, s);
}

void Separate(Floats& color, double lightness, double baseLightness) noexcept {
    if (std::abs(lightness - baseLightness) >= .18) return;
    const auto hls = RgbToHls(color);
    color = HlsToRgb(hls.h, baseLightness + (baseLightness < .5 ? .22 : -.22), hls.s);
}

Raw Dominant(const std::array<Straight, IconPixelCount>& image) {
    struct Bucket { int count{}; std::array<int, 3> sum{}; };
    std::array<Bucket, 512> buckets{};
    for (const auto& p : image) {
        if (p.a < 128) continue;
        auto& bucket = buckets[(p.color.r >> 5) << 6 | (p.color.g >> 5) << 3 | (p.color.b >> 5)];
        ++bucket.count;
        bucket.sum[0] += p.color.r; bucket.sum[1] += p.color.g; bucket.sum[2] += p.color.b;
    }
    std::vector<std::pair<int, Rgb>> ranked;
    int total = 0;
    for (const auto& bucket : buckets) {
        if (!bucket.count) continue;
        ranked.push_back({bucket.count, {bucket.sum[0] / bucket.count, bucket.sum[1] / bucket.count, bucket.sum[2] / bucket.count}});
        total += bucket.count;
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>{});
    const Rgb base = ranked.front().second;
    std::vector<Rgb> candidates;
    for (auto item = ranked.begin() + 1; item != ranked.end(); ++item)
        if (item->first * 33 >= total && Distance(item->second, base) >= 3600) candidates.push_back(item->second);
    std::optional<Rgb> contrast;
    for (const auto& candidate : candidates)
        if (!contrast || std::abs(Luma(candidate) - Luma(base)) > std::abs(Luma(*contrast) - Luma(base))) contrast = candidate;
    if (!contrast) contrast = Luma(base) < 140 ? Mix(base, {255, 255, 255}, 6, 10) : Mix(base, {0, 0, 0}, 55, 100);
    const auto accent = std::find_if(candidates.begin(), candidates.end(),
        [&](const Rgb& candidate) { return Distance(candidate, *contrast) >= 3600; });
    return {base, *contrast, accent != candidates.end() ? *accent : Mix(*contrast, Gold, 1, 2)};
}

std::string Glyph(const std::array<Straight, IconPixelCount>& image, const Raw& raw, int side) {
    constexpr int edge = static_cast<int>(IconSide);
    int x0 = edge, x1 = 0, y0 = edge, y1 = 0;
    for (int y = 0; y < edge; ++y)
        for (int x = 0; x < edge; ++x)
            if (image[y * edge + x].a >= 128) {
                x0 = std::min(x0, x); x1 = std::max(x1, x + 1);
                y0 = std::min(y0, y); y1 = std::max(y1, y + 1);
            }
    const int width = x1 - x0, height = y1 - y0;
    std::vector<std::string> rows(side, std::string(side, '.'));
    for (int gy = 0; gy < side; ++gy)
        for (int gx = 0; gx < side; ++gx) {
            std::array<int, 3> votes{};
            for (int y = y0 + gy * height / side; y < y0 + (gy + 1) * height / side; ++y)
                for (int x = x0 + gx * width / side; x < x0 + (gx + 1) * width / side; ++x) {
                    const auto& p = image[y * edge + x];
                    if (p.a < 128) { ++votes[0]; continue; }
                    std::size_t nearest = 0;
                    for (std::size_t i = 1; i < 3; ++i)
                        if (Distance(p.color, raw[i]) < Distance(p.color, raw[nearest])) nearest = i;
                    ++votes[nearest];
                }
            rows[gy][gx] = ".ab"[std::max_element(votes.begin(), votes.end()) - votes.begin()];
        }
    RemoveIsolatedCells(rows);
    std::string glyph;
    for (const auto& row : rows) glyph += row;
    const auto cells = side * side;
    const auto dots = static_cast<int>(std::count(glyph.begin(), glyph.end(), '.'));
    if (cells / 6 <= cells - dots && dots >= cells / 5) return glyph;
    return std::string(DiamondGlyph(side));
}

std::uint32_t Fnv(std::uint32_t hash, std::uint32_t byte) noexcept { return (hash ^ byte) * 16777619u; }

}

std::uint32_t ZigzagContrast(std::uint32_t opaqueArgb) noexcept {
    Hsl value = ToHsl(opaqueArgb);
    if (value.l < .62) return Cream;
    value.l -= .26;
    return FromHsl(value);
}

std::uint32_t FallbackAccent(std::uint32_t baseArgb, std::uint32_t contrastArgb) noexcept {
    double lightness{};
    auto accent = Soft(Mix(FromArgb(contrastArgb), Gold, 1, 2), lightness);
    Separate(accent, lightness, RgbToHls(Unit(FromArgb(baseArgb))).l);
    return Argb(accent);
}

PaletteResult SamplePalette(std::span<const PremultipliedRgba8> pixels, std::string_view identity) {
    if (pixels.size() != IconPixelCount) return Fallback{FallbackReason::InvalidPixels};
    std::array<Straight, IconPixelCount> image{};
    std::size_t opaque = 0;
    for (std::size_t i = 0; i < IconPixelCount; ++i) {
        const auto& p = pixels[i];
        if (p.r > p.a || p.g > p.a || p.b > p.a) return Fallback{FallbackReason::InvalidPixels};
        if (!p.a) continue;
        const auto straight = [&](std::uint8_t c) { return (c * 255 + p.a / 2) / p.a; };
        image[i] = {{straight(p.r), straight(p.g), straight(p.b)}, p.a};
        if (p.a >= 128) ++opaque;
    }
    if (opaque < IconPixelCount / 60) return Fallback{FallbackReason::InsufficientColorCoverage};
    const auto raw = Dominant(image);
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : identity) hash = Fnv(hash, byte);
    const auto glyph = Glyph(image, raw, 5 + static_cast<int>(hash & 1));
    for (const char cell : glyph) hash = Fnv(hash, static_cast<unsigned char>(cell));
    for (const auto& color : raw)
        for (const int channel : {color.r, color.g, color.b}) hash = Fnv(hash, static_cast<std::uint32_t>(channel) >> 4);
    std::array<Floats, 3> soft;
    std::array<double, 3> lightness{};
    for (std::size_t i = 0; i < 3; ++i) soft[i] = Soft(raw[i], lightness[i]);
    for (std::size_t i = 1; i < 3; ++i) Separate(soft[i], lightness[i], lightness[0]);
    return SampledPalette{Argb(soft[0]), Argb(soft[1]), Argb(soft[2]), glyph, hash};
}

}
