#include "IconPalette.h"

#include <algorithm>
#include <cmath>

// Palette mathematics adapted from saragordic/window-sweaters src/autoyarn.m,
// a49dff52483dafdbe27f972bd271cd28d37cef1c. GPL-3.0; see repository LICENSE/NOTICE.md.
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
        // Upstream truncates, rather than rounds, each byte.
        color |= static_cast<std::uint32_t>(c * 255) << (16 - i * 8);
    }
    return color;
}

std::uint32_t BinColor(unsigned bin) noexcept {
    return 0xff000000u | (((bin >> 8) & 15) * 17 << 16)
                       | (((bin >> 4) & 15) * 17 << 8) | ((bin & 15) * 17);
}

} // namespace

std::uint32_t Soften(std::uint32_t opaqueArgb) noexcept {
    Hsl value = ToHsl(opaqueArgb);
    value.s = std::clamp(value.s * .48, .22, .60);
    value.l = std::clamp(value.l * .85 + .10, .30, .72);
    return FromHsl(value);
}

std::uint32_t ZigzagContrast(std::uint32_t opaqueArgb) noexcept {
    Hsl value = ToHsl(opaqueArgb);
    if (value.l < .62) return Cream;
    value.l -= .26;
    return FromHsl(value);
}

PaletteResult SamplePalette(std::span<const PremultipliedRgba8> pixels) noexcept {
    if (pixels.size() != IconPixelCount) return Fallback{FallbackReason::InvalidPixels};
    std::array<unsigned, 4096> counts{};
    unsigned kept = 0;
    for (const auto& pixel : pixels) {
        if (pixel.r > pixel.a || pixel.g > pixel.a || pixel.b > pixel.a)
            return Fallback{FallbackReason::InvalidPixels};
        if (pixel.a < 217) continue;
        const double r = static_cast<double>(pixel.r) / pixel.a;
        const double g = static_cast<double>(pixel.g) / pixel.a;
        const double b = static_cast<double>(pixel.b) / pixel.a;
        const double high = std::max({r, g, b}), low = std::min({r, g, b});
        const double saturation = high <= 0 ? 0 : (high - low) / high;
        if (saturation < .18 || high < .12 || high - low < .10) continue;
        const auto bin = (static_cast<unsigned>(r * 15) << 8)
                       | (static_cast<unsigned>(g * 15) << 4)
                       | static_cast<unsigned>(b * 15);
        ++counts[bin];
        ++kept;
    }
    if (kept < IconPixelCount / 60)
        return Fallback{FallbackReason::InsufficientColorCoverage};

    std::array<std::uint32_t, 3> picked{};
    unsigned selected = 0;
    for (; selected < picked.size(); ++selected) {
        unsigned bestCount = 0, bestBin = 0;
        for (unsigned bin = 0; bin < counts.size(); ++bin) {
            if (!counts[bin]) continue;
            const Hsl candidate = ToHsl(BinColor(bin));
            bool separated = true;
            for (unsigned j = 0; j < selected; ++j) {
                const Hsl previous = ToHsl(picked[j]);
                double distance = std::abs(candidate.h - previous.h);
                if (distance > .5) distance = 1 - distance;
                if (distance < .06 && std::abs(candidate.l - previous.l) < .20)
                    separated = false;
            }
            if (separated && counts[bin] > bestCount) {
                bestCount = counts[bin];
                bestBin = bin;
            }
        }
        if (!bestCount) break;
        picked[selected] = BinColor(bestBin);
    }
    const Hsl baseRaw = ToHsl(picked[0]);
    SampledPalette palette{Soften(picked[0]), Cream};
    bool foundContrast = false;
    for (unsigned j = 1; j < selected; ++j) {
        if (std::abs(ToHsl(picked[j]).l - baseRaw.l) >= .20) {
            palette.contrastArgb = Soften(picked[j]);
            foundContrast = true;
            break;
        }
    }
    if (!foundContrast ||
        std::abs(ToHsl(palette.baseArgb).l - ToHsl(palette.contrastArgb).l) < .20)
        palette.contrastArgb = ZigzagContrast(palette.baseArgb);
    return palette;
}

Motif StableMotif(std::string_view identity) noexcept {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : identity) hash = (hash ^ byte) * 16777619u;
    constexpr Motif motifs[]{Motif::Zigzag, Motif::Picnic, Motif::Twinkle};
    return motifs[hash % 3];
}

std::string_view MotifName(Motif motif) noexcept {
    switch (motif) {
    case Motif::Zigzag: return "zigzag";
    case Motif::Picnic: return "picnic";
    case Motif::Twinkle: return "twinkle";
    }
    return {};
}

} // namespace window_sweaters::icon_yarn
