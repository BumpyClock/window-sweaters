// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Imports\Imports.h"
#include "..\..\src\Designs\Catalogue.h"
#include "..\..\src\Colorways\Resolver.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winioctl.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace window_sweaters;
using namespace window_sweaters::imports;
using Microsoft::WRL::ComPtr;

namespace {
unsigned checks = 0;
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
void Hr(HRESULT value, const char* reason) {
    if (FAILED(value)) {
        std::cerr << reason << ": HRESULT 0x" << std::hex << static_cast<unsigned long>(value) << '\n';
        throw std::runtime_error(reason);
    }
}
template<class T> T Value(Result<T> result) {
    if (const auto* error = std::get_if<Error>(&result)) {
        std::cerr << error->reason << " line " << error->line << " native " << error->native_error << '\n';
        throw std::runtime_error("Expected successful import");
    }
    return std::get<T>(std::move(result));
}
template<class T> void Reject(Result<T> result, ErrorCode code, const char* reason) {
    const auto* error = std::get_if<Error>(&result);
    if (error && error->code != code)
        std::cerr << "Wrong rejection: " << error->reason << '\n';
    Check(error && error->code == code, reason);
}
ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> factory;
    Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "Create WIC test factory");
    return factory;
}
std::vector<std::uint8_t> Encode(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint32_t> colors, std::span<const std::uint8_t> indices = {}, bool interlaced = false) {
    auto factory = Factory();
    ComPtr<IStream> stream;
    Hr(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create PNG test stream");
    ComPtr<IWICBitmapEncoder> encoder;
    Hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG test encoder");
    Hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG test encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Hr(encoder->CreateNewFrame(&frame, &properties), "Create PNG test frame");
    if (interlaced) {
        PROPBAG2 option{};
        option.pstrName = const_cast<wchar_t*>(L"InterlaceOption");
        VARIANT value{};
        value.vt = VT_BOOL; value.boolVal = VARIANT_TRUE;
        Hr(properties->Write(1, &option, &value), "Set Adam7 PNG test option");
    }
    Hr(frame->Initialize(properties.Get()), "Initialize PNG test frame");
    Hr(frame->SetSize(width, height), "Set PNG test dimensions");
    GUID format = indices.empty() ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat8bppIndexed;
    const GUID requested = format;
    Hr(frame->SetPixelFormat(&format), "Set PNG test pixel format");
    Check(format == requested, "WIC supports the requested literal test format");
    std::vector<std::uint8_t> pixels;
    if (indices.empty()) {
        Check(colors.size() == static_cast<std::size_t>(width) * height, "RGBA fixture dimensions agree");
        pixels.reserve(colors.size() * 4);
        for (const auto color : colors) {
            pixels.push_back(static_cast<std::uint8_t>(color));
            pixels.push_back(static_cast<std::uint8_t>(color >> 8));
            pixels.push_back(static_cast<std::uint8_t>(color >> 16));
            pixels.push_back(static_cast<std::uint8_t>(color >> 24));
        }
    } else {
        Check(indices.size() == static_cast<std::size_t>(width) * height, "Indexed fixture dimensions agree");
        ComPtr<IWICPalette> palette;
        Hr(factory->CreatePalette(&palette), "Create test palette");
        std::vector<WICColor> mutable_colors(colors.begin(), colors.end());
        Hr(palette->InitializeCustom(mutable_colors.data(), static_cast<UINT>(mutable_colors.size())),
            "Initialize literal indexed test palette");
        Hr(frame->SetPalette(palette.Get()), "Set indexed PNG test palette");
        pixels.assign(indices.begin(), indices.end());
    }
    Hr(frame->WritePixels(height, width * (indices.empty() ? 4 : 1),
        static_cast<UINT>(pixels.size()), pixels.data()), "Write PNG test pixels");
    Hr(frame->Commit(), "Commit PNG test frame");
    Hr(encoder->Commit(), "Commit PNG test encoder");
    STATSTG stat{};
    Hr(stream->Stat(&stat, STATFLAG_NONAME), "Size PNG test stream");
    Check(stat.cbSize.QuadPart <= maximum_png_bytes, "Generated fixture stays within encoded limit");
    Hr(stream->Seek({}, STREAM_SEEK_SET, nullptr), "Seek PNG test stream");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG received = 0;
    Hr(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &received), "Read encoded test PNG");
    Check(received == bytes.size(), "Encoded fixture read is complete");
    return bytes;
}
std::vector<std::uint8_t> Solid(std::uint32_t argb, std::uint32_t width = 1, std::uint32_t height = 1) {
    return Encode(width, height, std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height, argb));
}
std::uint32_t Be(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}
void PutBe(std::span<std::uint8_t> bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}
std::uint32_t Crc(std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0xffffffffu;
    for (auto byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit != 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return value ^ 0xffffffffu;
}
void FixCrc(std::vector<std::uint8_t>& bytes, std::size_t start) {
    const auto length = Be(std::span(bytes).subspan(start, 4));
    PutBe(std::span(bytes).subspan(start + length + 8, 4),
        Crc(std::span(bytes).subspan(start + 4, length + 4)));
}
std::size_t Chunk(std::span<const std::uint8_t> bytes, std::string_view type) {
    for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
        if (std::string_view(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4) == type)
            return offset;
        offset += Be(bytes.subspan(offset, 4)) + 12;
    }
    throw std::runtime_error("Fixture chunk not found");
}
std::vector<std::uint8_t> ExtraChunk(std::vector<std::uint8_t> bytes, std::string_view type,
    std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> chunk(12 + payload.size());
    PutBe(std::span(chunk).first(4), static_cast<std::uint32_t>(payload.size()));
    std::copy(type.begin(), type.end(), chunk.begin() + 4);
    std::copy(payload.begin(), payload.end(), chunk.begin() + 8);
    FixCrc(chunk, 0);
    const auto before = Chunk(bytes, "IEND");
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(before), chunk.begin(), chunk.end());
    return bytes;
}
std::vector<std::uint8_t> RepackIdat(std::span<const std::uint8_t> png,
    std::span<const std::uint8_t> zlib) {
    std::vector<std::uint8_t> result(png.begin(), png.begin() + 8);
    bool inserted = false;
    for (std::size_t offset = 8; offset + 12 <= png.size();) {
        const auto length = Be(png.subspan(offset, 4));
        if (std::string_view(reinterpret_cast<const char*>(png.data() + offset + 4), 4) == "IDAT") {
            if (!inserted) {
                std::vector<std::uint8_t> chunk(12 + zlib.size());
                PutBe(std::span(chunk).first(4), static_cast<std::uint32_t>(zlib.size()));
                std::copy_n("IDAT", 4, chunk.begin() + 4);
                std::copy(zlib.begin(), zlib.end(), chunk.begin() + 8);
                FixCrc(chunk, 0);
                result.insert(result.end(), chunk.begin(), chunk.end());
                inserted = true;
            }
        } else {
            result.insert(result.end(), png.begin() + offset, png.begin() + offset + length + 12);
        }
        offset += static_cast<std::size_t>(length) + 12;
    }
    return result;
}
std::uint32_t Adler(std::span<const std::uint8_t> bytes) {
    std::uint32_t a = 1, b = 0;
    for (const auto value : bytes) { a = (a + value) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}
std::vector<std::uint8_t> Zlib(std::span<const std::uint8_t> filtered, bool fixed) {
    std::vector<std::uint8_t> stream{0x78, 0x01};
    if (!fixed) {
        const auto count = static_cast<std::uint16_t>(filtered.size());
        stream.insert(stream.end(), {1, static_cast<std::uint8_t>(count),
            static_cast<std::uint8_t>(count >> 8), static_cast<std::uint8_t>(~count),
            static_cast<std::uint8_t>((~count) >> 8)});
        stream.insert(stream.end(), filtered.begin(), filtered.end());
    } else {
        std::size_t bit = 16;
        const auto write = [&](unsigned value, unsigned length) {
            for (unsigned i = 0; i != length; ++i, ++bit) {
                if (bit / 8 == stream.size()) stream.push_back(0);
                stream[bit / 8] |= static_cast<std::uint8_t>(((value >> i) & 1) << (bit % 8));
            }
        };
        write(3, 3); // Final fixed-Huffman block.
        const auto code = [&](unsigned value, unsigned length) {
            for (unsigned i = length; i != 0; --i) write((value >> (i - 1)) & 1, 1);
        };
        for (const auto byte : filtered)
            if (byte <= 143) code(byte + 0x30, 8);
            else code(byte - 144 + 0x190, 9);
        code(0, 7); // End-of-block symbol256.
    }
    const auto offset = stream.size();
    stream.resize(offset + 4);
    PutBe(std::span(stream).last(4), Adler(filtered));
    return stream;
}

void DeflateTest() {
    const auto template_png = Solid(0xff010203);
    Check(template_png[25] == 6 && template_png[24] == 8, "WIC fixture uses eight-bit RGBA PNG rows");
    const std::array<std::uint8_t, 5> filtered{0, 1, 2, 3, 255};
    for (const bool fixed : {false, true})
        Check(Value(DecodePng(RepackIdat(template_png, Zlib(filtered, fixed)))).Cells()[0] == 0xff010203,
            "Native WIC decodes independently generated stored/fixed DEFLATE fixtures");
    auto invalid = Zlib(filtered, false);
    invalid[5] ^= 1;
    Reject(DecodePng(RepackIdat(template_png, invalid)), ErrorCode::InvalidPng,
        "Stored-block LEN/NLEN mismatch rejected");
    invalid = Zlib(filtered, true); invalid.back() ^= 1;
    Reject(DecodePng(RepackIdat(template_png, invalid)), ErrorCode::InvalidPng,
        "Damaged Adler checksum rejected before tolerant WIC decode");
    invalid = Zlib(filtered, false); invalid.insert(invalid.end() - 4, 0);
    Reject(DecodePng(RepackIdat(template_png, invalid)), ErrorCode::InvalidPng,
        "Extra compressed stream bytes are not accepted");
    invalid = Zlib(filtered, false); invalid[2] = 7;
    Reject(DecodePng(RepackIdat(template_png, invalid)), ErrorCode::InvalidPng, "Reserved DEFLATE block type rejected");
    invalid = Zlib(filtered, false); invalid[2] = 5;
    Reject(DecodePng(RepackIdat(template_png, invalid)), ErrorCode::InvalidPng, "Malformed dynamic Huffman tree rejected");
    auto wrong_filter = filtered; wrong_filter[0] = 5;
    Reject(DecodePng(RepackIdat(template_png, Zlib(wrong_filter, false))), ErrorCode::InvalidPng,
        "Invalid PNG filter rejected even with correct stream checksums");
    Reject(DecodePng(RepackIdat(template_png, Zlib(std::array<std::uint8_t, 6>{}, false))),
        ErrorCode::InvalidPng, "Decoded scanline overflow rejected before output growth");
    Reject(DecodePng(RepackIdat(template_png, Zlib(std::array<std::uint8_t, 4>{}, false))),
        ErrorCode::InvalidPng, "Decoded scanline truncation rejected");
    std::vector<std::uint32_t> pixels(256 * 256);
    std::uint32_t state = 0x12345678;
    for (auto& pixel : pixels) {
        state = state * 1664525u + 1013904223u;
        pixel = 0xff123400u | ((state >> 16) & 15);
    }
    const auto dynamic = Encode(256, 256, pixels);
    const auto data = Chunk(dynamic, "IDAT");
    Check(((dynamic[data + 10] >> 1) & 3) == 2, "WIC independently generated a dynamic-Huffman fixture");
    Check(std::ranges::equal(Value(DecodePng(dynamic)).Cells(), pixels),
        "Dynamic Huffman literals/backreferences preserve all sixteen colors at dimension limit");
    for (const auto size : {std::pair{1u, 1u}, std::pair{3u, 5u}, std::pair{16u, 13u}}) {
        std::vector<std::uint32_t> cells(static_cast<std::size_t>(size.first) * size.second);
        for (std::size_t i = 0; i != cells.size(); ++i) cells[i] = 0xff987600u | (i & 15);
        const auto interlaced = Encode(size.first, size.second, cells, {}, true);
        Check(interlaced[28] == 1, "Native WIC fixture uses Adam7 interlace");
        Check(std::ranges::equal(Value(DecodePng(interlaced)).Cells(), cells),
            "Adam7 pass bounds handle small/empty passes and preserve top-down cells");
    }
}

void RulesTest() {
    const auto parsed = Value(ParseRules(
        "# personal only\r\n Claude = #D58561 atelier-claude # note\r\n"
        "CLAUDE Helper = #123aBC\nclaude helper = #AABBCC\nSpotify = #010203 # no chart\n"));
    Check(parsed.Rules().size() == 4, "Comments do not become rules");
    Check(parsed.Rules()[0].original_prefix == "Claude" &&
        parsed.Rules()[0].base_argb == 0xffd58561 && parsed.Rules()[0].chart_name == "atelier-claude",
        "Original prefix, literal RGB, and chart semantics are retained");
    Check(parsed.Rules()[1].base_argb == 0xff123abc, "Mixed case six-digit RGB parses exactly");
    Check(!parsed.Rules()[3].chart_name, "A comment is not an optional chart");
    const auto* longest = LongestPrefix(parsed, L"cLaUdE HeLpEr (Renderer).exe");
    Check(longest && longest->base_argb == 0xff123abc, "Longest prefix wins; equal-length ties keep first rule");
    Check(LongestPrefix(parsed, L"CLAUDE.exe") == &parsed.Rules()[0], "Case-insensitive shorter prefix matches");
    Check(!LongestPrefix(parsed, L"xClaude.exe"), "Matching is a prefix, not a substring");
    Check(!LongestPrefix(parsed, L""), "Empty aliases cannot match");
    const auto unicode = Value(ParseRules("Éditeur = #ABCDEF\n"));
    Check(LongestPrefix(unicode, L"éditeur.exe") != nullptr, "Windows ordinal Unicode case matching is supported");
    for (const std::string text : {"Claude #123456", "= #123456", "Claude = 123456",
        "Claude = #12345", "Claude = #1234567", "Claude = #12345g", "Claude = #123456suffix",
        "Claude = #123456 motif trailing", "Claude = #123456 #comment\nbad"}) {
        Reject(ParseRules(text), ErrorCode::InvalidRule, "Invalid rule makes the whole candidate fail");
    }
    Reject(ParseRules("Claude = #123456 " + std::string(64, 'a')), ErrorCode::InvalidRule, "Overlong chart token rejected");
    Reject(ParseRules(std::string(64, 'a') + " = #123456"), ErrorCode::InvalidRule, "Overlong prefix rejected");
    Reject(ParseRules("#" + std::string(512, 'a')), ErrorCode::InvalidRule, "Overlong physical line rejected as a unit");
    Reject(ParseRules(std::string("A\0B = #123456", 13)), ErrorCode::InvalidUtf8, "Embedded NUL rejected");
    Reject(ParseRules(std::string("\xff = #123456")), ErrorCode::InvalidUtf8, "Malformed UTF-8 rejected");
    Reject(ParseRules(std::string(maximum_rules_bytes + 1, '#')), ErrorCode::RulesLimit, "Encoded rules budget enforced");
    std::string many;
    for (unsigned i = 0; i != 65; ++i) many += "Name = #010203\n";
    Reject(ParseRules(many), ErrorCode::RulesLimit, "Rule count bound enforced");
    Check(Value(ParseRules(" # comment\n\t \r\n")).Rules().empty(), "Comment-only import is valid and empty");
}

applications::AppMetadata Metadata(std::wstring_view path, std::optional<std::wstring> aumid = {}) {
    const applications::PackagedIdentityQuery packaged = aumid ?
        applications::PackagedIdentityQuery(applications::PackagedAumid{*aumid}) :
        applications::PackagedIdentityQuery(applications::NoPackagedIdentity{});
    const auto result = applications::ResolveMetadata(packaged, applications::ExecutablePath{std::wstring(path)});
    if (std::holds_alternative<applications::MetadataUnavailable>(result))
        std::wcerr << L"Invalid metadata fixture path: " << path << L" AUMID: " << aumid.value_or(L"(none)") << '\n';
    Check(std::holds_alternative<applications::AppMetadata>(result), "Synthetic verified process metadata resolves");
    return std::get<applications::AppMetadata>(result);
}
void ResolutionTest() {
    const auto rules = Value(ParseRules("Code = #123456\nNoSuch = #ABCDEF\nSpotify = #010203\n"));
    const std::array metadata{
        Metadata(L"C:\\EditorOne\\Code.exe"), Metadata(L"D:\\EditorTwo\\Code.exe"),
        Metadata(L"C:\\Music\\Spotify.exe"), Metadata(L"C:\\Music\\Spotify.exe")};
    const auto result = Value(ResolveRules(rules, metadata));
    Check(std::holds_alternative<Ambiguous>(result[0].match) &&
        std::get<Ambiguous>(result[0].match).candidates.size() == 2,
        "Same basename in distinct verified locations remains ambiguous");
    Check(std::holds_alternative<Unmatched>(result[1].match), "Unknown prefix is not guessed");
    Check(std::get<Matched>(result[2].match).application == metadata[2].Identity(),
        "Unique binding retains verified typed executable identity");
    Check(std::get<Matched>(result[2].match).application.Key() != L"Spotify",
        "Prefix is never persisted as AppIdentity");
    const std::array same{metadata[2], metadata[3]};
    Check(std::holds_alternative<Matched>(Value(ResolveRules(rules, same))[2].match),
        "Several windows/metadata rows of one identity do not create ambiguity");
    const std::array packaged{Metadata(L"C:\\Music\\Spotify.exe", L"Package_8wekyb3d8bbwe!Spotify")};
    Check(std::get<Matched>(Value(ResolveRules(rules, packaged))[2].match).application.Kind() ==
        applications::IdentityKind::PackagedAumid, "Packaged identity survives explicit alias translation");
    const auto no_alias = applications::ResolveMetadata(
        applications::PackagedAumid{L"CodeFamily_8wekyb3d8bbwe!Code"},
        applications::MetadataUnavailable{applications::UnavailableReason::AccessDenied,
            applications::QueryStage::ExecutablePath, ERROR_ACCESS_DENIED});
    Check(std::holds_alternative<applications::AppMetadata>(no_alias), "Package metadata can lack executable alias");
    const std::array without_alias{std::get<applications::AppMetadata>(no_alias)};
    Check(std::holds_alternative<Unmatched>(Value(ResolveRules(rules, without_alias))[0].match),
        "Package presentation text is never used as verified executable alias");
    const auto plan = Value(PlanRuleImport(rules, metadata));
    Check(plan.bindings.size() == 1 && plan.bindings[0].application == metadata[2].Identity() &&
        plan.resolutions.size() == 3, "Ambiguous/unmatched diagnostics are retained but cannot become exact bound rules");
    const std::array editor{metadata[0]};
    const auto prefixes = Value(ParseRules("Code = #123456\nCode.exe = #abcdef\ncode.exe = #010203\n"));
    const auto longest = Value(PlanRuleImport(prefixes, editor));
    Check(longest.bindings.size() == 1 && longest.bindings[0].rule.base_argb == 0xffabcdef,
        "Exact resolver adapter receives only the longest rule; same-length first-source tie retained");
    const std::array helper_apps{metadata[0], Metadata(L"C:\\Helper\\Code Helper.exe")};
    const auto helper_plan = Value(PlanRuleImport(Value(ParseRules(
        "Code = #123456\nCode Helper = #abcdef\n")), helper_apps));
    Check(helper_plan.bindings.size() == 1 && helper_plan.bindings[0].application == helper_apps[1].Identity() &&
        std::holds_alternative<Ambiguous>(helper_plan.resolutions[0].match),
        "A verified unique longer prefix can bind while the ambiguous broad prefix remains unresolved");
    const std::array multi_alias{
        Metadata(L"D:\\Zed\\Code.exe"), Metadata(L"C:\\Pkg\\Spotify.exe", L"Pkg_8wekyb3d8bbwe!App"),
        Metadata(L"C:\\Alpha\\CODE.EXE"), Metadata(L"C:\\Pkg\\Code.exe", L"Pkg_8wekyb3d8bbwe!App"),
        Metadata(L"c:\\alpha\\code.exe")};
    const auto short_only = Value(PlanRuleImport(Value(ParseRules("Sp = #010203\n")), multi_alias));
    Check(short_only.bindings.size() == 1 && short_only.bindings[0].application == multi_alias[1].Identity(),
        "A packaged identity binds through any of its verified executable aliases");
    const auto shadowed = Value(PlanRuleImport(Value(ParseRules("Sp = #010203\nCode = #123456\n")), multi_alias));
    const auto& candidates = std::get<Ambiguous>(shadowed.resolutions[1].match).candidates;
    Check(candidates.size() == 3 && std::is_sorted(candidates.begin(), candidates.end()) &&
        std::ranges::find(candidates, multi_alias[1].Identity()) != candidates.end(),
        "Ambiguous candidates are unique, sorted, and fold case-equivalent executable identities");
    Check(shadowed.bindings.empty(),
        "A longer ambiguous prefix shadows a shorter exact match for every candidate identity");
    Reject(ResolveRules(rules, std::vector<applications::AppMetadata>(4097, metadata[0])),
        ErrorCode::ResourceLimit, "Verified metadata count is bounded before matching");
    std::vector<applications::AppMetadata> many_apps;
    for (unsigned i = 0; i != 900; ++i)
        many_apps.push_back(Metadata(L"C:\\Editors\\App" + std::to_wstring(i) + L".exe"));
    std::string repeated;
    for (unsigned i = 0; i != 64; ++i) repeated += "App = #123456\n";
    Reject(ResolveRules(Value(ParseRules(repeated)), many_apps), ErrorCode::ResourceLimit,
        "Repeated ambiguous matches cannot allocate unbounded copies of verified identities");
}

void PngTest() {
    const std::array pixels{0x7f654321u, 0x80654321u, 0xffabcdefu, 0x00112233u};
    const auto encoded = Encode(2, 2, pixels);
    const auto decoded = Value(DecodePng(encoded));
    Check(decoded.Width() == 2 && decoded.Height() == 2, "PNG dimensions retained");
    Check(std::ranges::equal(decoded.Cells(), std::array{0u, 0xff654321u, 0xffabcdefu, 0u}),
        "Alpha127 uses base; alpha128 literal RGB becomes opaque; rows remain top-down");
    Check(std::ranges::equal(decoded.EncodedPng(), encoded), "Validated encoded content is owned for host persistence");
    Check(decoded.ContentHash().size() == 64, "Normalized content hash is SHA-256");
    Check(Value(DecodePng(Encode(2, 2, pixels))).ContentHash() == decoded.ContentHash(),
        "Same normalized content has stable hash independent of encoding instance");
    const auto metadata = ExtraChunk(encoded, "iCCP",
        std::array<std::uint8_t, 12>{'i', 'g', 'n', 'o', 'r', 'e', 'd', 0, 0, 0, 0, 0});
    Check(Value(DecodePng(metadata)).ContentHash() == decoded.ContentHash(),
        "Ancillary profile bytes are not exposed to WIC metadata decompression or color transforms");
    const auto low_alpha = Value(DecodePng(Solid(0x7f112233)));
    const auto transparent = Value(DecodePng(Solid(0x00445566)));
    Check(low_alpha.ContentHash() == transparent.ContentHash(), "Base-yarn sentinel normalization has stable identity");
    Check(Value(DecodePng(Solid(0xff111111, 256, 256))).Cells().size() == 65536,
        "Upstream maximum256x256 accepted, not a16row crop");
    for (const auto dimensions : {std::pair{257u, 1u}, std::pair{1u, 257u}}) {
        Reject(DecodePng(Solid(0xff112233, dimensions.first, dimensions.second)), ErrorCode::Dimensions,
            "Axis257 rejected before decoded allocation");
    }
    for (auto width : {0u, 0xffffffffu, 0x40000000u}) {
        auto invalid = encoded;
        PutBe(std::span(invalid).subspan(16, 4), width);
        FixCrc(invalid, 8);
        Reject(DecodePng(invalid), ErrorCode::Dimensions, "Zero/overflow dimensions rejected before WIC");
    }
    for (std::size_t length : {std::size_t{0}, std::size_t{7}, std::size_t{32}, encoded.size() - 1}) {
        Reject(DecodePng(std::span(encoded).first(length)), ErrorCode::InvalidPng, "Truncated PNG rejected");
    }
    auto invalid = encoded;
    invalid[0] = 0;
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "Malformed PNG signature rejected");
    invalid = encoded; invalid[29] ^= 1;
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "Header CRC failure rejected");
    invalid = encoded; PutBe(std::span(invalid).subspan(8, 4), 0xffffffffu);
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "Chunk length overflow rejected");
    invalid = encoded; invalid.push_back(0);
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "Trailing bytes rejected");
    invalid = encoded; invalid[24] = 16; FixCrc(invalid, 8);
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "16-bit channels rejected instead of silently quantized");
    invalid = encoded;
    const auto idat = Chunk(invalid, "IDAT");
    invalid[idat + 8] = 0; FixCrc(invalid, idat);
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "Malformed zlib header rejected even if WIC tolerates it");
    invalid = encoded;
    const auto data_size = Be(std::span(invalid).subspan(idat, 4));
    std::fill(invalid.begin() + static_cast<std::ptrdiff_t>(idat + 10),
        invalid.begin() + static_cast<std::ptrdiff_t>(idat + 8 + data_size), std::uint8_t{0xff});
    FixCrc(invalid, idat);
    Reject(DecodePng(invalid), ErrorCode::InvalidPng, "Damaged deflate stream rejected even if WIC reports success");
    Reject(DecodePng(std::vector<std::uint8_t>(maximum_png_bytes + 1)), ErrorCode::EncodedLimit,
        "Oversized encoded PNG rejected without decode");
    Reject(DecodePng(ExtraChunk(encoded, "acTL", std::array<std::uint8_t, 8>{})),
        ErrorCode::InvalidPng, "Animated PNG explicitly rejected");
    Reject(DecodePng(ExtraChunk(encoded, "ABCD", {})), ErrorCode::InvalidPng,
        "Unknown critical PNG chunks rejected");
    std::vector<std::uint32_t> colors;
    for (std::uint32_t i = 0; i != 16; ++i) colors.push_back(0xff123400 | i);
    Check(Value(DecodePng(Encode(16, 1, colors))).Cells().size() == 16,
        "Sixteen distinct literal yarns accepted");
    colors.push_back(0xff123410);
    Reject(DecodePng(Encode(17, 1, colors)), ErrorCode::UnsupportedYarnCount,
        "Seventeenth literal yarn rejected, never dropped");
    colors.back() = 0x7f123456;
    Reject(DecodePng(Encode(17, 1, colors)), ErrorCode::UnsupportedYarnCount,
        "Base sentinel consumes a yarn group as well");
    colors.erase(colors.begin());
    Check(Value(DecodePng(Encode(16, 1, colors))).Cells().back() == 0,
        "Fifteen literal yarns plus base sentinel accepted");
    const std::array palette{0x00112233u, 0x80654321u, 0xffabcdefu};
    const std::array<std::uint8_t, 4> indices{0, 1, 2, 0};
    Check(std::ranges::equal(Value(DecodePng(Encode(2, 2, palette, indices))).Cells(),
        std::array{0u, 0xff654321u, 0xffabcdefu, 0u}),
        "Indexed palette alpha and literal custom RGB are preserved");
    const auto indexed = Encode(3, 1, palette, std::array<std::uint8_t, 3>{0, 1, 2});
    const auto plte = Chunk(indexed, "PLTE");
    Check(Be(std::span(indexed).subspan(plte, 4)) == 9, "WIC palette has exactly three defined entries");
    const auto depth = indexed[24];
    Check(depth <= 8 && (1u << depth) > 3, "Fixture leaves an invalid representable palette index");
    std::vector<std::uint8_t> indexed_filtered(1 + (3 * depth + 7) / 8);
    indexed_filtered[1] = static_cast<std::uint8_t>(3 << (8 - depth));
    Reject(DecodePng(RepackIdat(indexed, Zlib(indexed_filtered, false))), ErrorCode::InvalidPng,
        "Undefined palette indices are rejected rather than recolored by WIC");
}

StoredChart Store(const ImportedChart& chart) {
    return {chart.Id(), chart.Name(), chart.Revision(), chart.SourceMaterial(),
        {chart.Data().EncodedPng().begin(), chart.Data().EncodedPng().end()}, chart.ContentRevision()};
}
void CandidateTest() {
    const auto first = Value(DecodePng(Solid(0xff123456)));
    const auto second = Value(DecodePng(Solid(0xffabcdef)));
    const ImportCatalogue empty;
    auto committed = Value(BuildCandidate(empty, AddChart{"personal", first}));
    Check(empty.Charts().empty(), "Adding a chart does not mutate committed input");
    const auto original = committed.Charts()[0];
    Check(original.Id() == "imported:sha256:" + original.Revision().substr(7),
        "Initial identity includes normalized content/material, not path/menu order");
    Check(original.ContentRevision() == 1, "New stable asset starts at content revision1");
    Check(original.Custom() && !original.Generated(), "Personal PNG is custom and never icon-generated");
    Reject(BuildCandidate(committed, AddChart{"personal", second}), ErrorCode::DuplicateName,
        "Duplicate name does not implicitly replace committed chart");
    Reject(BuildCandidate(committed, AddChart{"copy", first}), ErrorCode::DuplicateIdentity,
        "Duplicate content identity is not an independent menu-index chart");
    Reject(BuildCandidate(committed, AddChart{"bad/name", second}), ErrorCode::InvalidName, "Path-shaped chart name rejected");
    Reject(BuildCandidate(committed, ReplaceChart{"missing", second, {}}), ErrorCode::UnknownChart,
        "Unknown replacement identity rejected");
    Reject(BuildCandidate(committed, RemoveChart{"missing"}), ErrorCode::UnknownChart, "Unknown remove identity rejected");
    auto replacement = Value(BuildCandidate(committed, ReplaceChart{original.Id(), second, {}}));
    Check(replacement.Charts()[0].Id() == original.Id(), "Replacement retains stable imported identity");
    Check(replacement.Charts()[0].Revision() != original.Revision(), "Replacement changes content revision for cache invalidation");
    Check(replacement.Charts()[0].ContentRevision() == 2, "Changed normalized content advances native resolver/renderer revision");
    Check(committed.Charts()[0].Revision() == original.Revision() &&
        committed.Charts()[0].Data().Cells()[0] == 0xff123456, "Replacement only returns a candidate; old content remains intact");
    const auto repeated = Value(BuildCandidate(replacement,
        ReplaceChart{original.Id(), second, {}}));
    Check(repeated.Charts()[0].Revision() == replacement.Charts()[0].Revision(),
        "Repeated same-content replacement is revision-idempotent");
    Check(repeated.Charts()[0].ContentRevision() == 2, "Same-content retry does not increment native content revision");
    const auto rename = Value(BuildCandidate(committed,
        ReplaceChart{original.Id(), first, std::string("renamed")}));
    Check(rename.Charts()[0].Id() == original.Id() && rename.Charts()[0].Revision() == original.Revision(),
        "Presentation rename does not change content identity/revision");
    Check(rename.Charts()[0].ContentRevision() == 1, "Presentation rename retains native content revision");
    Check(Value(BuildCandidate(committed, RemoveChart{original.Id()})).Charts().empty(),
        "Explicit remove returns an empty candidate");
    const auto with_rules = Value(BuildCandidate(committed,
        SetRules{Value(ParseRules("Claude = #123456 personal\n"))}));
    Reject(BuildCandidate(with_rules, RemoveChart{original.Id()}), ErrorCode::UnknownChart,
        "Removing a referenced chart fails without changing committed rules or chart");
    Reject(BuildCandidate(committed, SetRules{Value(ParseRules("Claude = #123456 unknown\n"))}),
        ErrorCode::UnknownChart, "Unknown semantic chart references are reported before commit");
    Check(with_rules.PersonalRules().Rules()[0].base_argb == 0xff123456 &&
        with_rules.Charts()[0].Data().Cells()[0] == 0xff123456, "Personal colors do not recolor chart cells");
    const auto shadow = Value(BuildCandidate(empty, AddChart{"atelier-messages", first}));
    const auto* messages = designs::ws_design_chart_by_name("atelier-messages");
    Check(shadow.Charts()[0].SourceMaterial().flags == messages->flags &&
        (messages->flags & designs::ChartRoundDots), "Known-chart replacement retains round-dot and yarn flags");
    Check(shadow.Charts()[0].Id() != original.Id(),
        "Same literal cells with different source material have distinct initial identities for safe cache keys");
    const auto source_and_custom = Value(BuildCandidate(committed, AddChart{"atelier-messages", first}));
    Check(source_and_custom.Charts().size() == 2,
        "Different retained material can share literal pixels without being treated as duplicate content identity");
    const auto cuff = Value(BuildCandidate(empty, AddChart{"atelier-notes", first}));
    const auto* notes = designs::ws_design_chart_by_name("atelier-notes");
    const auto replaced_cuff = Value(BuildCandidate(cuff,
        ReplaceChart{cuff.Charts()[0].Id(), second, {}}));
    Check(replaced_cuff.Charts()[0].SourceMaterial() == Material{notes->flags, notes->corner_argb, notes->cuff_argb},
        "Replacement retains cuff, corner, fitted/sculpted/defined source metadata");
    Check(replaced_cuff.Charts()[0].Data().Cells()[0] == 0xffabcdef,
        "Source material retention does not rewrite personal PNG pixels");
    const auto shadow_rules = Value(BuildCandidate(shadow,
        SetRules{Value(ParseRules("Messages = #ABCDEF atelier-messages\n"))}));
    const auto unshadowed = Value(BuildCandidate(shadow_rules, RemoveChart{shadow.Charts()[0].Id()}));
    Check(unshadowed.Charts().empty() && unshadowed.PersonalRules().Rules().size() == 1,
        "Removing a built-in shadow restores immutable original semantic chart");
    auto records = Value(StoreCatalogue(replacement));
    const auto restored = Value(RestoreCatalogue(records));
    Check(restored.Charts()[0].Id() == original.Id() &&
        restored.Charts()[0].Revision() == replacement.Charts()[0].Revision() &&
        restored.Charts()[0].Data().Cells()[0] == 0xffabcdef && restored.Charts()[0].ContentRevision() == 2,
        "Owned storage values rehydrate stable identity and changed content without external paths");
    records.charts[0].revision.back() = records.charts[0].revision.back() == '0' ? '1' : '0';
    Reject(RestoreCatalogue(records), ErrorCode::RevisionMismatch, "Stale or forged content revision is rejected");
    records.charts[0] = Store(replacement.Charts()[0]); records.charts[0].encoded_png.pop_back();
    Reject(RestoreCatalogue(records), ErrorCode::InvalidPng, "Corrupt persisted PNG rejected without losing committed chart");
    const StoredCatalogue duplicates{{}, {Store(original), Store(original)}, 1};
    Reject(RestoreCatalogue(duplicates), ErrorCode::DuplicateName, "Duplicate stored chart names rejected");
    auto duplicate_id = duplicates;
    duplicate_id.charts[1].name = "other-name";
    Reject(RestoreCatalogue(duplicate_id), ErrorCode::DuplicateIdentity, "Duplicate stored identities rejected independently of names");
    records.charts[0] = Store(original); records.charts[0].id = "C:\\external\\chart.png";
    Reject(RestoreCatalogue(records), ErrorCode::InvalidStoredChart, "Stored identity cannot be an external file reference");
    records.charts[0] = Store(original); records.charts[0].material.flags = 1u << 31;
    Reject(RestoreCatalogue(records), ErrorCode::InvalidStoredChart, "Unknown stored renderer flags rejected");
    records.charts[0] = Store(original); records.charts[0].material.corner_argb = 0x7f123456;
    Reject(RestoreCatalogue(records), ErrorCode::InvalidStoredChart, "Partial-alpha material yarn rejected");
    records.charts[0] = Store(original); records.charts[0].content_revision = 0;
    Reject(RestoreCatalogue(records), ErrorCode::InvalidStoredChart, "Zero native content revision rejected");
    records.charts[0].content_revision = (std::numeric_limits<std::uint64_t>::max)();
    records.revision_high_water = (std::numeric_limits<std::uint64_t>::max)();
    const auto exhausted = Value(RestoreCatalogue(records));
    Reject(BuildCandidate(exhausted, ReplaceChart{original.Id(), second, {}}), ErrorCode::RevisionOverflow,
        "Native content revision overflow rejects replacement without wrapping or mutating committed chart");
    Check(exhausted.Charts()[0].ContentRevision() == (std::numeric_limits<std::uint64_t>::max)() &&
        exhausted.Charts()[0].Data().Cells()[0] == 0xff123456, "Failed revision overflow preserves old asset");
    const auto both = Value(BuildCandidate(committed, AddChart{"other", second}));
    Reject(BuildCandidate(both, ReplaceChart{original.Id(), first, std::string("other")}),
        ErrorCode::DuplicateName, "Rename cannot overwrite another chart by presentation name");
    const auto moved_source = Value(DecodePng(Solid(0xff987654)));
    auto movable = moved_source;
    auto moved = std::move(movable);
    Check(moved.Cells()[0] == 0xff987654, "Moved decoded value remains valid at destination");
    Reject(BuildCandidate(empty, AddChart{"moved-from", movable}), ErrorCode::InvalidStoredChart,
        "Moved-from decoded value cannot construct an invalid candidate");
}

void ReimportRevisionTest() {
    const auto a = Value(DecodePng(Solid(0xff112233)));
    const auto b = Value(DecodePng(Solid(0xff445566)));
    const auto c = Value(DecodePng(Solid(0xff778899)));
    for (const bool restart : {false, true}) {
        auto catalogue = Value(BuildCandidate({}, AddChart{"asset", a}));
        const auto identity = catalogue.Charts()[0].Id();
        const colorways::PersonalChart first_a{identity, catalogue.Charts()[0].ContentRevision()};
        Check(first_a.revision == 1, "First incarnation gets revision1");
        catalogue = Value(BuildCandidate(catalogue, ReplaceChart{identity, b, {}}));
        const colorways::PersonalChart old_b{identity, catalogue.Charts()[0].ContentRevision()};
        Check(old_b.revision == 2, "B gets revision2 without changing its original asset identity");
        catalogue = Value(BuildCandidate(catalogue, RemoveChart{identity}));
        Check(catalogue.Charts().empty() && catalogue.RevisionHighWater() == 2,
            "Removing the last asset retains a bounded scalar revision high-water mark");
        if (restart) {
            const auto stored = Value(StoreCatalogue(catalogue));
            Check(stored.charts.empty() && stored.revision_high_water == 2,
                "Empty stored catalogue retains revision history without tombstone records");
            catalogue = Value(RestoreCatalogue(stored));
            Check(catalogue.RevisionHighWater() == 2 && catalogue.Charts().empty(),
                "Restart between remove/reimport restores the durable high-water mark");
        }
        catalogue = Value(BuildCandidate(catalogue, AddChart{"asset", a}));
        const colorways::PersonalChart reimported_a{
            catalogue.Charts()[0].Id(), catalogue.Charts()[0].ContentRevision()};
        Check(reimported_a.id == identity && reimported_a.revision == 3 && reimported_a != first_a,
            "Reimport retains content identity but receives a fresh incarnation revision3");
        catalogue = Value(BuildCandidate(catalogue, ReplaceChart{identity, c, {}}));
        const colorways::PersonalChart new_c{identity, catalogue.Charts()[0].ContentRevision()};
        Check(new_c.revision == 4 && old_b != new_c,
            "Remove/reimport cannot reuse old B consumer key for different C content");
        const std::array cache_keys{first_a, old_b, reimported_a, new_c};
        for (std::size_t i = 0; i != cache_keys.size(); ++i)
            for (std::size_t j = i + 1; j != cache_keys.size(); ++j)
                Check(cache_keys[i] != cache_keys[j], "Every accepted incarnation has a unique production consumer key");
        const std::array cache{
            std::pair{old_b, 0xff445566u}, std::pair{new_c, catalogue.Charts()[0].Data().Cells()[0]}};
        const auto found = std::find_if(cache.begin(), cache.end(),
            [&](const auto& entry) { return entry.first == new_c; });
        Check(found != cache.end() && found->second == 0xff778899,
            "Consumer key lookup selects C and cannot return cached B after reimport");
        const auto roundtrip = Value(RestoreCatalogue(Value(StoreCatalogue(catalogue))));
        Check(roundtrip.Charts()[0].ContentRevision() == 4 && roundtrip.RevisionHighWater() == 4,
            "Post-reimport restore preserves native consumer revision and high-water mark");
    }

    const auto original = Value(BuildCandidate({}, AddChart{"asset", a}));
    auto catalogue = Value(BuildCandidate(original, AddChart{"other", b}));
    const auto identity = original.Charts()[0].Id();
    Check(catalogue.Charts()[1].ContentRevision() == 2,
        "A second identity receives a catalogue-global revision rather than resetting to1");
    catalogue = Value(BuildCandidate(catalogue, ReplaceChart{identity, c, {}}));
    Check(catalogue.FindById(identity)->ContentRevision() == 3 && catalogue.RevisionHighWater() == 3,
        "Replacement allocates above every asset revision, not just its per-ID counter");
    const auto stored = Value(StoreCatalogue(catalogue));
    auto missing = stored; missing.revision_high_water.reset();
    Reject(RestoreCatalogue(missing), ErrorCode::InvalidStoredChart, "Missing durable high-water mark cannot be inferred from live assets");
    Reject(RestoreCatalogue(StoredCatalogue{}), ErrorCode::InvalidStoredChart, "Missing high-water mark is rejected even for empty storage");
    auto reset = stored; reset.revision_high_water = 0;
    Reject(RestoreCatalogue(reset), ErrorCode::InvalidStoredChart, "Reset high-water mark cannot cover live revisions");
    auto behind = stored; behind.revision_high_water = 2;
    Reject(RestoreCatalogue(behind), ErrorCode::InvalidStoredChart, "Stored high-water mark must cover every live numeric revision");
    auto duplicate = stored; duplicate.charts[1].content_revision = duplicate.charts[0].content_revision;
    Reject(RestoreCatalogue(duplicate), ErrorCode::InvalidStoredChart, "Different live identities cannot share a globally allocated revision");
    const auto failed = BuildCandidate(catalogue, ReplaceChart{identity, a, std::string("other")});
    Reject(failed, ErrorCode::DuplicateName, "Failed replacement does not consume committed high-water mark");
    Check(catalogue.RevisionHighWater() == 3, "Invalid candidate leaves committed revision allocation intact");
    const auto valid = Value(BuildCandidate(catalogue, ReplaceChart{identity, a, {}}));
    Check(valid.FindById(identity)->ContentRevision() == 4, "Next valid replacement consumes exactly the next durable revision");
    auto exhausted_store = stored;
    exhausted_store.revision_high_water = (std::numeric_limits<std::uint64_t>::max)();
    const auto exhausted = Value(RestoreCatalogue(exhausted_store));
    Reject(BuildCandidate(exhausted, AddChart{"new", Value(DecodePng(Solid(0xffaabbcc)))}), ErrorCode::RevisionOverflow,
        "Catalogue-global revision exhaustion rejects new identities without wrapping");
    const auto removed = Value(BuildCandidate(
        Value(BuildCandidate(exhausted, RemoveChart{identity})),
        RemoveChart{exhausted.Charts()[1].Id()}));
    const auto restored_empty = Value(RestoreCatalogue(Value(StoreCatalogue(removed))));
    Reject(BuildCandidate(restored_empty, AddChart{"asset", a}), ErrorCode::RevisionOverflow,
        "Removing/restoring every asset cannot reset an exhausted high-water mark");
    Check(removed.Charts().empty() && restored_empty.RevisionHighWater() ==
        (std::numeric_limits<std::uint64_t>::max)(), "Revision history uses one scalar after arbitrary removals");
    const auto unchanged = Value(BuildCandidate(exhausted, ReplaceChart{identity, c, std::string("renamed")}));
    Check(unchanged.FindById(identity)->ContentRevision() == 3 && unchanged.RevisionHighWater() ==
        (std::numeric_limits<std::uint64_t>::max)(), "Same-content rename remains valid without allocating a revision at exhaustion");
}

void ResolverPrecedenceTest() {
    const auto data = Value(DecodePng(Encode(3, 1,
        std::array{0xffabcdefu, 0x00000000u, 0xff123456u})));
    const auto catalogue = Value(BuildCandidate({}, AddChart{"personal", data}));
    const auto& asset = catalogue.Charts()[0];
    const std::array metadata{Metadata(L"C:\\Verified\\Editor.exe")};
    const auto plan = Value(PlanRuleImport(Value(ParseRules(
        "Editor = #112233\nEditor.exe = #654321 personal\n")), metadata));
    Check(plan.bindings.size() == 1, "Precedence fixture binds one longest personal rule");
    const auto& binding = plan.bindings[0];
    const auto* referenced = catalogue.FindByName(*binding.rule.chart_name);
    Check(referenced == &asset, "Semantic rule chart resolves through validated imported asset registry");
    const std::array personal{colorways::PersonalRule{
        binding.application, {binding.rule.base_argb, {}},
        colorways::PersonalChart{asset.Id(), asset.ContentRevision()}}};
    const colorways::AutomaticAppearance sample{0xff998877, 0xff556677, {}};
    const auto appearance = [](const colorways::ResolutionResult& result) {
        Check(std::holds_alternative<colorways::ResolvedAppearance>(result), "Imported bound rule is accepted by production resolver");
        return std::get<colorways::ResolvedAppearance>(result);
    };
    const auto automatic = appearance(colorways::ResolveAppearance(&metadata[0].Identity(), {}, {}, {}, sample));
    Check(automatic.color_source == colorways::ColorSource::AutomaticSample && automatic.base_argb == 0xff998877,
        "Deterministic automatic proposal actually supplies baseline color");
    const auto result = appearance(colorways::ResolveAppearance(&metadata[0].Identity(), {}, personal, {}, sample));
    Check(result.base_argb == 0xff654321 && result.color_source == colorways::ColorSource::Personal,
        "Imported personal base outranks a deterministic automatic-icon proposal");
    Check(result.chart_colors == colorways::ChartColorPolicy::PreservePersonalPixels && !result.contrast_argb,
        "Personal PNG is not recolored by icon-derived contrast");
    Check(std::get<colorways::PersonalChart>(result.pattern) ==
        colorways::PersonalChart{asset.Id(), 1}, "Production resolver preserves stable imported ID and native content revision");
    Check(std::ranges::equal(asset.Data().Cells(), std::array{0xffabcdefu, 0u, 0xff123456u}),
        "Personal palette remains literal after precedence resolution");
    auto plain = personal;
    plain[0].pattern = colorways::PlainPattern{};
    const auto omitted = appearance(colorways::ResolveAppearance(&metadata[0].Identity(), {}, plain, {}, sample));
    Check(std::holds_alternative<colorways::PlainPattern>(omitted.pattern) &&
        omitted.pattern_source == colorways::PatternSource::Personal,
        "Omitted imported chart translates to explicit By App plain, not a fall-through override");
    colorways::ResolverSettings global;
    global.global_pattern = colorways::AuthoredChart{"atelier-notes"};
    const auto selected = appearance(colorways::ResolveAppearance(&metadata[0].Identity(), global, personal, {}, sample));
    Check(std::get<colorways::AuthoredChart>(selected.pattern).name == "atelier-notes" &&
        selected.pattern_source == colorways::PatternSource::GlobalSelection && selected.base_argb == 0xff654321,
        "Global explicit pattern overrides personal pattern, while personal base still wins");
    colorways::ResolverSettings manual;
    manual.design = colorways::SelectedTheme{"claude"};
    const auto selected_theme = appearance(colorways::ResolveAppearance(&metadata[0].Identity(), manual, personal, {}, sample));
    Check(selected_theme.base_argb == 0xff654321 &&
        selected_theme.color_source == colorways::ColorSource::Personal &&
        selected_theme.pattern_source == colorways::PatternSource::SelectedTheme,
        "Imported personal base outranks manual authored theme without changing its global pattern semantics");
}

void CatalogueBoundsTest() {
    ImportCatalogue candidate;
    const auto import_capacity = maximum_effective_charts - designs::ws_design_chart_count();
    for (std::size_t i = 0; i != import_capacity; ++i)
        candidate = Value(BuildCandidate(candidate,
            AddChart{"chart-" + std::to_string(i), Value(DecodePng(Solid(0xff010100u + static_cast<std::uint32_t>(i))))}));
    Check(candidate.Charts().size() == import_capacity, "Immutable built-ins plus imports reach the effective chart limit");
    Reject(BuildCandidate(candidate, AddChart{"overflow", Value(DecodePng(Solid(0xff998877)))}),
        ErrorCode::CatalogueLimit, "Effective128-chart source bound enforced");
    Check(candidate.Charts().size() == import_capacity, "Failed catalogue overflow leaves committed data unchanged");
    ImportCatalogue padded_catalogue;
    for (unsigned i = 0; i != 8; ++i) {
        const auto fixture_png = Solid(0xff112200u + i);
        const auto encoded = ExtraChunk(fixture_png, "vpAg",
            std::vector<std::uint8_t>(maximum_png_bytes - fixture_png.size() - 12));
        const auto decoded = Value(DecodePng(encoded));
        Check(decoded.EncodedPng().size() == maximum_png_bytes, "Exact encoded2MiB boundary accepted");
        const auto result = BuildCandidate(padded_catalogue, AddChart{"padded-" + std::to_string(i), decoded});
        if (i == 7)
            Reject(result, ErrorCode::CatalogueLimit, "Aggregate16MiB includes decoded cell memory, not encoded bytes alone");
        else padded_catalogue = Value(result);
    }
    Check(padded_catalogue.Charts().size() == 7, "Failed aggregate overflow does not mutate committed catalogue");
}

void WriteFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "Create local isolated PNG test file");
    DWORD written = 0;
    const bool success = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    Check(success && written == bytes.size() && closed, "Write complete isolated fixture bytes");
}
void Junction(const std::filesystem::path& junction, const std::filesystem::path& target) {
    struct MountPoint {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        wchar_t names[4096];
    } reparse{};
    const auto substitute = L"\\??\\" + target.native();
    const auto print = target.native();
    Check(substitute.size() + print.size() + 2 <= std::size(reparse.names), "Local junction metadata stays bounded");
    reparse.tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    reparse.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    reparse.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    reparse.data_length = static_cast<WORD>(8 + reparse.print_offset + reparse.print_length + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), reparse.names);
    std::copy(print.begin(), print.end(), reparse.names + substitute.size() + 1);
    Check(CreateDirectoryW(junction.c_str(), nullptr) != FALSE, "Create isolated local junction directory");
    const auto directory = CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Check(directory != INVALID_HANDLE_VALUE, "Open isolated junction metadata handle");
    DWORD returned = 0;
    const bool set = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &reparse, reparse.data_length + 8,
        nullptr, 0, &returned, nullptr) != FALSE;
    const auto native = GetLastError();
    const bool closed = CloseHandle(directory) != FALSE;
    if (!set) std::cerr << "Junction setup error " << native << '\n';
    Check(set && closed, "Create local reparse fixture without elevation");
}
void FileTest() {
    const auto folder = std::filesystem::absolute(L"tests\\Imports\\outputs");
    std::filesystem::create_directories(folder);
    const auto first = folder / L"first.png";
    const auto second = folder / L"second.png";
    const auto bytes = Solid(0xff123456);
    WriteFile(first, bytes); WriteFile(second, bytes);
    const auto imported = Value(ImportPngFile(first.native()));
    const auto same = Value(ImportPngFile(first.native()));
    const auto other_path = Value(ImportPngFile(second.native()));
    Check(imported.source_identity == same.source_identity, "Same stable ordinary file identity is observed twice");
    Check(imported.source_identity != other_path.source_identity &&
        imported.chart.ContentHash() == other_path.chart.ContentHash(),
        "Content identity is independent of file identity/path");
    const auto hardlink = folder / L"hardlink.png";
    Check(CreateHardLinkW(hardlink.c_str(), second.c_str(), nullptr) != FALSE, "Create ordinary local hard-link fixture");
    Check(Value(ImportPngFile(hardlink.native())).source_identity == other_path.source_identity,
        "Stable file identity recognizes ordinary hard links without using their paths as chart IDs");
    const auto junction = folder / L"junction";
    Junction(junction, folder);
    Reject(ImportPngFile((junction / L"second.png").native()), ErrorCode::ReparsePoint,
        "Local junction traversal is rejected before following its child path");
    Check(RemoveDirectoryW(junction.c_str()) != FALSE, "Remove only isolated junction, not its target");
    std::filesystem::remove(first);
    Check(imported.chart.Cells()[0] == 0xff123456 && !imported.chart.EncodedPng().empty(),
        "Imported content is owned after source file removal");
    Reject(ImportPngFile(first.native()), ErrorCode::FileIo, "Missing file returns checked error");
    Reject(ImportPngFile(L"relative.png"), ErrorCode::InvalidPath, "Relative external reference is rejected");
    Reject(ImportPngFile(L"\\\\never-contact-server\\share\\chart.png"), ErrorCode::RemotePath,
        "Network UNC rejected before filesystem access");
    Reject(ImportPngFile(L"\\\\?\\C:\\chart.png"), ErrorCode::RemotePath, "Device-prefixed path rejected");
    Reject(ImportPngFile(second.native() + L":stream"), ErrorCode::InvalidPath, "Alternate data stream rejected");
    Reject(ImportPngFile((folder / L"..\\second.png").native()), ErrorCode::InvalidPath, "Parent traversal rejected");
    Reject(ImportPngFile((folder / L"CON.png").native()), ErrorCode::InvalidPath, "DOS device alias rejected");
    Reject(ImportPngFile(folder.native()), ErrorCode::NotOrdinaryFile, "Directories cannot be imported");
    const auto busy = CreateFileW(second.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(busy != INVALID_HANDLE_VALUE, "Open controlled writer to exercise sharing safety");
    const auto rejected_busy = ImportPngFile(second.native());
    Check(CloseHandle(busy) != FALSE, "Close controlled writer");
    Reject(rejected_busy, ErrorCode::FileIo, "Active writers cannot race a stable PNG read");
    WriteFile(second, Solid(0xff654321));
    Check(Value(ImportPngFile(second.native())).chart.ContentHash() != imported.chart.ContentHash() &&
        imported.chart.Cells()[0] == 0xff123456, "Changed file content has a new revision without changing already owned content");
    const auto oversized = folder / L"oversized.png";
    WriteFile(oversized, std::vector<std::uint8_t>(maximum_png_bytes + 1));
    Reject(ImportPngFile(oversized.native()), ErrorCode::EncodedLimit, "Oversized disk file rejected before decode/read allocation");
    const auto truncated = folder / L"truncated.png";
    WriteFile(truncated, std::span(bytes).first(bytes.size() - 1));
    Reject(ImportPngFile(truncated.native()), ErrorCode::InvalidPng, "Truncated disk PNG rejected");
    std::filesystem::remove(second);
    std::filesystem::remove(hardlink);
    std::filesystem::remove(oversized);
    std::filesystem::remove(truncated);
}
} // namespace

int main() {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) return 2;
    int result = 0;
    try {
        RulesTest();
        ResolutionTest();
        PngTest();
        DeflateTest();
        CandidateTest();
        ReimportRevisionTest();
        ResolverPrecedenceTest();
        CatalogueBoundsTest();
        FileTest();
        std::cout << checks << " native import checks passed (WIC/local-file/model; no host/UI integration).\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    CoUninitialize();
    return result;
}
