#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace window_sweaters::icon_yarn {

inline constexpr std::size_t IconSide = 48;
inline constexpr std::size_t IconPixelCount = IconSide * IconSide;

// sRGB bytes, top-down rows; RGB is premultiplied by alpha, not linear-light RGB.
struct PremultipliedRgba8 {
    std::uint8_t r{}, g{}, b{}, a{};
    bool operator==(const PremultipliedRgba8&) const = default;
};
using IconPixels = std::array<PremultipliedRgba8, IconPixelCount>;

struct SampledPalette {
    std::uint32_t baseArgb;
    std::uint32_t contrastArgb;
    bool operator==(const SampledPalette&) const = default;
};

enum class FallbackReason {
    InvalidPixels,
    InsufficientColorCoverage,
    IconUnavailable,
    UnsupportedSource,
    SourceTooLarge,
    InvalidResource,
    DecodeFailed,
    DeadlineExceeded,
    Cancelled,
    ResourceLimit,
    QueryFailed
};

struct Fallback {
    FallbackReason reason;
    std::uint32_t nativeError{};
    bool operator==(const Fallback&) const = default;
};
using PaletteResult = std::variant<SampledPalette, Fallback>;
using PixelResult = std::variant<IconPixels, Fallback>;

PaletteResult SamplePalette(std::span<const PremultipliedRgba8> pixels) noexcept;
std::uint32_t Soften(std::uint32_t opaqueArgb) noexcept;
std::uint32_t ZigzagContrast(std::uint32_t opaqueArgb) noexcept;

// The caller supplies a persistent, canonical, kind-prefixed UTF-8 identity, never a PID,
// title, display name or executable basename. FNV is deliberately case-sensitive.
enum class Motif { Zigzag, Picnic, Twinkle };
Motif StableMotif(std::string_view canonicalAppIdentity) noexcept;
std::string_view MotifName(Motif motif) noexcept;

} // namespace window_sweaters::icon_yarn
