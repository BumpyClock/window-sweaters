// SPDX-License-Identifier: GPL-3.0-only
// Bounded RFC1950/1951 check: WIC can report success for damaged IDAT bytes.
#include "Internal.h"
#include <algorithm>

namespace window_sweaters::imports::detail {
namespace {
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool Read(unsigned count, unsigned& value) {
        if (count > 16 || count > bytes_.size() * 8 - offset_) return false;
        value = 0;
        for (unsigned bit = 0; bit < count; ++bit, ++offset_)
            value |= ((bytes_[offset_ / 8] >> (offset_ % 8)) & 1u) << bit;
        return true;
    }
    void Align() { offset_ = (offset_ + 7) & ~std::size_t{7}; }
    std::size_t ConsumedBytes() const { return (offset_ + 7) / 8; }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

class Huffman {
public:
    bool Build(std::span<const unsigned> lengths, bool code_alphabet = false) {
        counts_.fill(0);
        unsigned maximum = 0;
        for (const auto length : lengths) {
            if (length > 15) return false;
            ++counts_[length];
            maximum = (std::max)(maximum, length);
        }
        empty_ = maximum == 0;
        if (empty_) return !code_alphabet;
        int remaining = 1;
        for (unsigned length = 1; length != 16; ++length) {
            remaining = remaining * 2 - static_cast<int>(counts_[length]);
            if (remaining < 0) return false;
        }
        if (remaining > 0 && (code_alphabet || maximum != 1)) return false;
        std::array<unsigned, 16> offsets{};
        for (unsigned length = 1; length != 15; ++length)
            offsets[length + 1] = offsets[length] + counts_[length];
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol)
            if (lengths[symbol])
                symbols_[offsets[lengths[symbol]]++] = symbol;
        return true;
    }
    bool Decode(Bits& bits, unsigned& symbol) const {
        if (empty_) return false;
        unsigned code = 0, first = 0, index = 0;
        for (unsigned length = 1; length != 16; ++length) {
            unsigned bit = 0;
            if (!bits.Read(1, bit)) return false;
            code |= bit;
            const auto count = counts_[length];
            if (code >= first && code - first < count) {
                symbol = symbols_[index + code - first];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }
private:
    bool empty_ = true;
    std::array<unsigned, 16> counts_{};
    std::array<unsigned, 288> symbols_{};
};

bool Trees(Bits& bits, unsigned kind, Huffman& literal, Huffman& distance) {
    std::array<unsigned, 288> lengths{};
    std::array<unsigned, 32> distances{};
    if (kind == 1) {
        std::fill(lengths.begin(), lengths.begin() + 144, 8u);
        std::fill(lengths.begin() + 144, lengths.begin() + 256, 9u);
        std::fill(lengths.begin() + 256, lengths.begin() + 280, 7u);
        std::fill(lengths.begin() + 280, lengths.end(), 8u);
        distances.fill(5);
        return literal.Build(lengths) && distance.Build(distances);
    }
    unsigned nl = 0, nd = 0, nc = 0;
    if (!bits.Read(5, nl) || !bits.Read(5, nd) || !bits.Read(4, nc)) return false;
    nl += 257; nd += 1; nc += 4;
    if (nl > 286) return false;
    constexpr std::array<unsigned, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::array<unsigned, 19> code_lengths{};
    for (unsigned i = 0; i < nc; ++i)
        if (!bits.Read(3, code_lengths[order[i]])) return false;
    Huffman codes;
    if (!codes.Build(code_lengths, true)) return false;
    std::array<unsigned, 318> combined{};
    unsigned used = 0;
    while (used < nl + nd) {
        unsigned value = 0;
        if (!codes.Decode(bits, value)) return false;
        if (value < 16) {
            combined[used++] = value;
            continue;
        }
        unsigned extra = 0, repeat = 0, previous = 0;
        if (value == 16) {
            if (!used || !bits.Read(2, extra)) return false;
            repeat = extra + 3; previous = combined[used - 1];
        } else if (value == 17) {
            if (!bits.Read(3, extra)) return false;
            repeat = extra + 3;
        } else if (value == 18) {
            if (!bits.Read(7, extra)) return false;
            repeat = extra + 11;
        } else return false;
        if (repeat > nl + nd - used) return false;
        std::fill(combined.begin() + used, combined.begin() + used + repeat, previous);
        used += repeat;
    }
    if (combined[256] == 0) return false;
    return literal.Build(std::span(combined).first(nl)) &&
        distance.Build(std::span(combined).subspan(nl, nd));
}

bool Inflate(std::span<const std::uint8_t> compressed,
    std::vector<std::uint8_t>& output, std::size_t limit, unsigned window_size) {
    Bits bits(compressed);
    unsigned final = 0;
    do {
        unsigned kind = 0;
        if (!bits.Read(1, final) || !bits.Read(2, kind) || kind == 3) return false;
        if (kind == 0) {
            bits.Align();
            unsigned count = 0, complement = 0;
            if (!bits.Read(16, count) || !bits.Read(16, complement) ||
                (count ^ complement) != 65535 || count > limit - output.size()) return false;
            for (unsigned i = 0; i != count; ++i) {
                unsigned byte = 0;
                if (!bits.Read(8, byte)) return false;
                output.push_back(static_cast<std::uint8_t>(byte));
            }
            continue;
        }
        Huffman literal, distance;
        if (!Trees(bits, kind, literal, distance)) return false;
        for (;;) {
            unsigned symbol = 0;
            if (!literal.Decode(bits, symbol)) return false;
            if (symbol == 256) break;
            if (symbol < 256) {
                if (output.size() == limit) return false;
                output.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol > 285) return false;
            constexpr std::array<unsigned, 29> length_base{
                3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            constexpr std::array<unsigned, 29> length_bits{
                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            unsigned extra = 0;
            if (!bits.Read(length_bits[symbol - 257], extra)) return false;
            const auto length = length_base[symbol - 257] + extra;
            if (!distance.Decode(bits, symbol) || symbol > 29) return false;
            constexpr std::array<unsigned, 30> distance_base{
                1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                6145, 8193, 12289, 16385, 24577};
            constexpr std::array<unsigned, 30> distance_bits{
                0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
            if (!bits.Read(distance_bits[symbol], extra)) return false;
            const auto back = distance_base[symbol] + extra;
            if (back > output.size() || back > window_size || length > limit - output.size()) return false;
            for (unsigned i = 0; i != length; ++i)
                output.push_back(output[output.size() - back]);
        }
    } while (!final);
    return bits.ConsumedBytes() == compressed.size() && output.size() == limit;
}

std::uint32_t Be(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}
std::uint32_t Adler(std::span<const std::uint8_t> bytes) {
    std::uint32_t low = 1, high = 0;
    for (const auto byte : bytes) {
        low = (low + byte) % 65521;
        high = (high + low) % 65521;
    }
    return (high << 16) | low;
}
struct Pass { unsigned x, y, dx, dy; };
constexpr std::array<Pass, 7> adam7{{
    {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}
}};
unsigned PassSize(unsigned full, unsigned start, unsigned step) {
    return full <= start ? 0 : (full - start + step - 1) / step;
}
}

Result<std::monostate> ValidatePngDeflate(std::span<const std::uint8_t> encoded,
    std::uint32_t width, std::uint32_t height, unsigned depth, unsigned color_type, bool interlaced) {
    const unsigned channels = color_type == 2 ? 3 : color_type == 4 ? 2 : color_type == 6 ? 4 : 1;
    const auto bits_per_pixel = channels * depth;
    std::array<std::pair<unsigned, unsigned>, 7> rows{};
    if (!interlaced) rows[0] = {width, height};
    else for (std::size_t i = 0; i != adam7.size(); ++i)
        rows[i] = {PassSize(width, adam7[i].x, adam7[i].dx), PassSize(height, adam7[i].y, adam7[i].dy)};
    std::size_t expected = 0;
    for (const auto [w, h] : rows)
        if (w && h) expected += static_cast<std::size_t>(h) * ((w * bits_per_pixel + 7) / 8 + 1);
    std::vector<std::uint8_t> zlib;
    for (std::size_t offset = 8; offset + 12 <= encoded.size();) {
        const auto length = Be(encoded.subspan(offset, 4));
        if (std::string_view(reinterpret_cast<const char*>(encoded.data() + offset + 4), 4) == "IDAT")
            zlib.insert(zlib.end(), encoded.begin() + offset + 8, encoded.begin() + offset + 8 + length);
        offset += static_cast<std::size_t>(length) + 12;
    }
    if (zlib.size() < 6)
        return Error{ErrorCode::InvalidPng, "PNG image data has no complete zlib stream"};
    std::vector<std::uint8_t> filtered;
    filtered.reserve(expected);
    if (!Inflate(std::span(zlib).subspan(2, zlib.size() - 6), filtered, expected,
        1u << ((zlib[0] >> 4) + 8)))
        return Error{ErrorCode::InvalidPng, "PNG deflate stream is malformed, truncated, or exceeds its scanline bound"};
    if (Adler(filtered) != Be(std::span(zlib).last(4)))
        return Error{ErrorCode::InvalidPng, "PNG image data Adler-32 mismatch"};
    std::size_t offset = 0;
    for (const auto [w, h] : rows) if (w && h) {
        const auto stride = (w * bits_per_pixel + 7) / 8 + 1;
        for (unsigned y = 0; y != h; ++y, offset += stride)
            if (filtered[offset] > 4)
                return Error{ErrorCode::InvalidPng, "PNG has an invalid scanline filter"};
    }
    return std::monostate{};
}

}
