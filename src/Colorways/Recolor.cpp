// SPDX-License-Identifier: GPL-3.0-only
#include "Recolor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace window_sweaters::colorways {
namespace {
constexpr double degrees = 3.141592653589793 / 180;
constexpr double lightest = 0.97, darkest = 0.10, lightness_cap = 0.62, neutral_chroma = 0.045;
constexpr double keep_hue = 0.25, minimum_contrast = 0.153, minimum_distinct = 0.08, tone_match = 0.6;

struct Lch { double L, C, h; };
struct Yarn {
    double L0, C0, h0, dL;
    bool neutral;
    int side = 1;
    double offset = 0, L = 0, C = 0, h = 0;
};

double Wrap(double d) { return std::fmod(std::fmod(d + 180, 360) + 360, 360) - 180; }
double Circ(double a, double b) { return std::abs(Wrap(a - b)); }
double Slerp(double a, double b, double t) { return a + Wrap(b - a) * t; }
double Normal(double h) { return std::fmod(std::fmod(h, 360) + 360, 360); }
double ToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double ToSrgb(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }

Lch ToLch(std::uint32_t argb) {
    const double r = ToLinear(((argb >> 16) & 255) / 255.0);
    const double g = ToLinear(((argb >> 8) & 255) / 255.0);
    const double b = ToLinear((argb & 255) / 255.0);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return {L, std::hypot(A, B), std::fmod(std::atan2(B, A) / degrees + 360, 360)};
}

std::array<double, 3> Linear(double L, double C, double h) {
    const double a = C * std::cos(h * degrees), b = C * std::sin(h * degrees);
    const double l = std::pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
    const double m = std::pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
    const double s = std::pow(L - 0.0894841775 * a - 1.2914855480 * b, 3);
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

bool InGamut(double L, double C, double h) {
    const auto rgb = Linear(L, C, h);
    return std::all_of(rgb.begin(), rgb.end(), [](double v) { return v >= -1e-4 && v <= 1 + 1e-4; });
}

double ClampChroma(double L, double C, double h) {
    if (InGamut(L, C, h)) return C;
    double low = 0, high = C;
    for (int i = 0; i < 24; ++i) {
        const double middle = (low + high) / 2;
        (InGamut(L, middle, h) ? low : high) = middle;
    }
    return low;
}

std::uint32_t ToArgb(double L, double C, double h) {
    const auto rgb = Linear(L, ClampChroma(L, C, h), h);
    std::uint32_t argb = 0xff000000u;
    for (int i = 0; i < 3; ++i) {
        const double channel = std::clamp(ToSrgb(rgb[i]), 0.0, 1.0) * 255;
        argb |= static_cast<std::uint32_t>(std::floor(channel + 0.5)) << (16 - 8 * i);
    }
    return argb;
}

double DeltaE(double L1, double C1, double h1, double L2, double C2, double h2) {
    return std::hypot(L1 - L2, C1 * std::cos(h1 * degrees) - C2 * std::cos(h2 * degrees),
        C1 * std::sin(h1 * degrees) - C2 * std::sin(h2 * degrees));
}
double DeltaE(const Yarn& p, const Yarn& q) { return DeltaE(p.L, p.C, p.h, q.L, q.C, q.h); }

void AssignLightness(std::vector<Yarn>& yarns, double base) {
    const auto room = [base](int side) { return side > 0 ? lightest - base : base - darkest; };
    for (auto& y : yarns) {
        y.side = std::abs(y.dL) < 0.01 ? (room(1) >= room(-1) ? 1 : -1) : (y.dL > 0 ? 1 : -1);
        y.offset = std::min(std::abs(y.dL), lightness_cap);
        if (room(y.side) < std::max(minimum_contrast + 0.08, std::min(y.offset, 0.25)) && room(-y.side) > room(y.side))
            y.side = -y.side;
    }
    for (int side : {1, -1}) {
        const double r = room(side), t = 0.6 * r;
        for (auto& y : yarns)
            if (y.side == side && y.offset > t) y.offset = t + (r - t) * (1 - std::exp(-(y.offset - t) / (r - t)));
    }
    for (auto& y : yarns) {
        y.offset = std::max(y.offset, minimum_contrast);
        y.L = base + y.side * y.offset;
    }
}

std::vector<double> PickHarmony(const std::vector<Yarn>& yarns, double reference) {
    static constexpr std::array<std::array<double, 7>, 3> harmonies{{
        {0, 30, -30, 60, -60, 90, -90},
        {180, 0, 150, 210, 30, -30, 60},
        {150, 210, 0, 30, -30, 180, 60},
    }};
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < yarns.size(); ++i) if (!yarns[i].neutral) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t p, std::size_t q) { return yarns[p].C0 > yarns[q].C0; });
    std::vector<double> best;
    std::optional<double> best_cost;
    for (const auto& anchors : harmonies) {
        std::array<int, 7> used{};
        std::vector<double> pick(yarns.size());
        double cost = 0;
        for (auto index : order) {
            const double d = Wrap(yarns[index].h0 - reference);
            std::size_t chosen = 0;
            double chosen_cost = 1e9;
            for (std::size_t i = 0; i < anchors.size(); ++i) {
                const double c = Circ(d, anchors[i]) + 45 * used[i];
                if (c < chosen_cost) { chosen_cost = c; chosen = i; }
            }
            ++used[chosen]; pick[index] = anchors[chosen]; cost += Circ(d, anchors[chosen]);
        }
        if (!best_cost || cost < *best_cost) { best_cost = cost; best = std::move(pick); }
    }
    return best;
}

void Fit(std::vector<Yarn>& yarns, double base, const std::vector<std::vector<double>>& original) {
    static constexpr double lightness_steps[] = {0, 0.03, -0.03, 0.06, -0.06, 0.1, -0.1, 0.15, -0.15, 0.22, -0.22, 0.3, -0.3};
    static constexpr double hue_steps[] = {0, 15, -15, 30, -30, 50, -50, 80, -80, 120, -120, 180};
    static constexpr double chroma_steps[] = {0, 0.04, 0.08};
    const auto need = [&](std::size_t a, std::size_t b) {
        return std::min(minimum_distinct, 0.85 * original[std::min(a, b)][std::max(a, b)]);
    };
    for (auto& y : yarns) y.C = ClampChroma(y.L, std::max(0.0, y.C), y.h);
    struct Move { double score, L, C, h; };
    const auto try_move = [&](std::size_t j) {
        const auto& y = yarns[j];
        std::optional<Move> best;
        for (double dl : lightness_steps) for (double dh : hue_steps) for (double dc : chroma_steps) {
            const double L = y.L + dl, h = y.h + dh;
            if (L < darkest || L > lightest || std::abs(L - base) < minimum_contrast) continue;
            const double C = ClampChroma(L, y.C + dc, h);
            double nearest = 1e9;
            for (std::size_t k = 0; k < yarns.size(); ++k)
                if (k != j) nearest = std::min(nearest, DeltaE(L, C, h, yarns[k].L, yarns[k].C, yarns[k].h) / need(k, j));
            const double score = std::min(nearest, 1.0) * 10 - (std::abs(dl) * 2 + std::abs(dh) / 120 + dc);
            if (!best || score > best->score) best = Move{score, L, C, h};
        }
        return best;
    };
    for (int pass = 0; pass < 30; ++pass) {
        bool moved = false;
        for (std::size_t j = 1; j < yarns.size(); ++j)
            for (std::size_t i = 0; i < j; ++i) {
                if (DeltaE(yarns[i], yarns[j]) >= need(i, j)) continue;
                const auto move_j = try_move(j), move_i = try_move(i);
                const bool use_i = move_i && (!move_j || move_i->score > move_j->score + 0.5);
                const auto& move = use_i ? move_i : move_j;
                if (!move) continue;
                auto& target = yarns[use_i ? i : j];
                target.L = move->L; target.C = move->C; target.h = move->h;
                moved = true;
            }
        if (!moved) break;
    }
}
}

std::vector<std::uint32_t> AdaptPalette(std::uint32_t original_base_argb,
    std::span<const std::uint32_t> yarns_argb, std::uint32_t new_base_argb) {
    std::vector<std::uint32_t> distinct;
    for (auto argb : yarns_argb)
        if (std::find(distinct.begin(), distinct.end(), argb | 0xff000000u) == distinct.end())
            distinct.push_back(argb | 0xff000000u);
    const auto b0 = ToLch(original_base_argb), b1 = ToLch(new_base_argb);
    std::vector<Yarn> yarns;
    for (auto argb : distinct) {
        const auto c = ToLch(argb);
        yarns.push_back({c.L, c.C, c.h, c.L - b0.L, c.C < neutral_chroma});
    }
    std::vector<std::vector<double>> original(yarns.size(), std::vector<double>(yarns.size()));
    for (std::size_t p = 0; p < yarns.size(); ++p)
        for (std::size_t q = 0; q < yarns.size(); ++q)
            original[p][q] = DeltaE(yarns[p].L0, yarns[p].C0, yarns[p].h0, yarns[q].L0, yarns[q].C0, yarns[q].h0);
    AssignLightness(yarns, b1.L);

    const double w0 = std::clamp((b0.C - 0.015) / 0.03, 0.0, 1.0);
    const double w1 = std::clamp((b1.C - 0.015) / 0.03, 0.0, 1.0);
    const double rotation = w0 * w1 * Wrap(b1.h - b0.h);
    const double cap = std::max(0.12, 0.05 + 1.5 * b1.C);
    const double tint = std::min(0.03, 0.3 * b1.C);
    std::vector<double> harmony;
    if (std::any_of(yarns.begin(), yarns.end(), [](const Yarn& y) { return !y.neutral; })) {
        double reference = b0.h;
        if (w0 < 0.5) {
            double x = 0, y = 0;
            for (const auto& yarn : yarns)
                if (!yarn.neutral) { x += yarn.C0 * std::cos(yarn.h0 * degrees); y += yarn.C0 * std::sin(yarn.h0 * degrees); }
            reference = std::fmod(std::atan2(y, x) / degrees + 360, 360);
        }
        harmony = PickHarmony(yarns, reference);
    }
    for (std::size_t i = 0; i < yarns.size(); ++i) {
        auto& y = yarns[i];
        double h, C;
        if (y.neutral) {
            h = b1.h;
            C = 0.5 * y.C0 + tint;
        } else {
            const double rotated = y.h0 + rotation;
            const double harmonised = Slerp(y.h0, b1.h + harmony[i], w1);
            h = Slerp(Slerp(rotated, harmonised, 0.5), y.h0, keep_hue);
            C = y.C0 + (std::min(y.C0, cap) - y.C0) * tone_match;
        }
        h = Normal(h);
        if (y.L < 0.55 && C > 0.04 && h > 45 && h < 125) h = Slerp(h, h < 85 ? 25 : 145, 0.7);
        y.h = Normal(h); y.C = C;
    }
    Fit(yarns, b1.L, original);

    std::vector<std::uint32_t> result;
    result.reserve(yarns_argb.size());
    for (auto argb : yarns_argb) {
        const auto index = static_cast<std::size_t>(std::find(distinct.begin(), distinct.end(), argb | 0xff000000u) - distinct.begin());
        result.push_back(ToArgb(yarns[index].L, yarns[index].C, yarns[index].h));
    }
    return result;
}

}
