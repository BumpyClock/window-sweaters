// SPDX-License-Identifier: GPL-3.0-only
// Alpha/material semantics: pinned src/chart.c and src/knit.c; see README.md.
#include "Internal.h"
#include <Windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <limits>
#include <set>

namespace window_sweaters::imports {
namespace {
using Microsoft::WRL::ComPtr;

std::uint32_t BigEndian(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}
std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit != 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}
struct Dimensions {
    std::uint32_t width = 0, height = 0;
    std::uint8_t depth = 0, color_type = 0;
    std::uint32_t palette_count = 0;
    bool interlaced = false;
};

Result<Dimensions> Envelope(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() > maximum_png_bytes)
        return Error{ErrorCode::EncodedLimit, "PNG exceeds 2 MiB encoded limit"};
    if (bytes.size() < 33 || !std::equal(signature.begin(), signature.end(), bytes.begin()))
        return Error{ErrorCode::InvalidPng, "Missing PNG signature or IHDR"};
    Dimensions dimensions{};
    bool header = false, palette = false, data = false, ended_data = false, transparency = false;
    std::uint8_t color_type = 0;
    std::array<std::uint8_t, 2> zlib_header{};
    std::size_t data_bytes = 0;
    std::size_t offset = 8;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 12)
            return Error{ErrorCode::InvalidPng, "Truncated PNG chunk header"};
        const auto length = BigEndian(bytes.subspan(offset, 4));
        if (length > bytes.size() - offset - 12)
            return Error{ErrorCode::InvalidPng, "PNG chunk length exceeds encoded bytes"};
        const auto type_bytes = bytes.subspan(offset + 4, 4);
        if (!std::all_of(type_bytes.begin(), type_bytes.end(), [](std::uint8_t c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }) || (type_bytes[2] & 32) != 0)
            return Error{ErrorCode::InvalidPng, "Invalid PNG chunk type"};
        const std::string_view type(reinterpret_cast<const char*>(type_bytes.data()), 4);
        const auto payload = bytes.subspan(offset + 8, length);
        if (Crc32(bytes.subspan(offset + 4, static_cast<std::size_t>(length) + 4)) !=
            BigEndian(bytes.subspan(offset + 8 + length, 4)))
            return Error{ErrorCode::InvalidPng, "PNG chunk CRC mismatch"};
        if (!header && type != "IHDR")
            return Error{ErrorCode::InvalidPng, "IHDR must be first"};
        if (type == "IHDR") {
            if (header || length != 13)
                return Error{ErrorCode::InvalidPng, "Duplicate or malformed IHDR"};
            header = true;
            dimensions = {BigEndian(payload.first(4)), BigEndian(payload.subspan(4, 4))};
            if (!dimensions.width || !dimensions.height ||
                dimensions.width > maximum_chart_axis || dimensions.height > maximum_chart_axis)
                return Error{ErrorCode::Dimensions, "PNG dimensions must be 1..256 on each axis"};
            const auto depth = payload[8];
            color_type = payload[9];
            dimensions.depth = depth;
            dimensions.color_type = color_type;
            dimensions.interlaced = payload[12] != 0;
            const bool depth_valid =
                (color_type == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                ((color_type == 2 || color_type == 4 || color_type == 6) && (depth == 8 || depth == 16)) ||
                (color_type == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8));
            if (!depth_valid || payload[10] != 0 || payload[11] != 0 || payload[12] > 1)
                return Error{ErrorCode::InvalidPng, "Unsupported or invalid PNG header fields"};
            if (depth == 16)
                return Error{ErrorCode::InvalidPng, "16-bit PNG channels cannot be preserved in an 8-bit yarn chart"};
        } else if (type == "PLTE") {
            if (palette || data || length == 0 || length > 768 || length % 3 != 0 ||
                color_type == 0 || color_type == 4)
                return Error{ErrorCode::InvalidPng, "Invalid PNG palette"};
            palette = true;
            dimensions.palette_count = length / 3;
            if (color_type == 3 && dimensions.palette_count > (1u << dimensions.depth))
                return Error{ErrorCode::InvalidPng, "PNG palette exceeds indexed bit depth"};
        } else if (type == "IDAT") {
            if (ended_data || (color_type == 3 && !palette))
                return Error{ErrorCode::InvalidPng, "Invalid PNG image-data order or missing palette"};
            data = true;
            for (std::size_t i = 0; i != payload.size() && data_bytes + i < zlib_header.size(); ++i)
                zlib_header[data_bytes + i] = payload[i];
            data_bytes += payload.size();
        } else if (type == "tRNS") {
            if (transparency || data || (color_type == 0 && length != 2) ||
                (color_type == 2 && length != 6) ||
                (color_type == 3 && (!palette || !length || length > dimensions.palette_count)) ||
                color_type == 4 || color_type == 6)
                return Error{ErrorCode::InvalidPng, "Invalid PNG transparency chunk"};
            if (color_type == 0 || color_type == 2) {
                for (std::size_t i = 0; i != payload.size(); i += 2)
                    if (((static_cast<unsigned>(payload[i]) << 8) | payload[i + 1]) >=
                        (1u << dimensions.depth))
                        return Error{ErrorCode::InvalidPng, "PNG transparency sample exceeds channel depth"};
            }
            transparency = true;
        } else if (type == "IEND") {
            if (length != 0 || !data || offset + 12 != bytes.size())
                return Error{ErrorCode::InvalidPng, "Invalid IEND or trailing PNG bytes"};
            const auto zlib_word = (static_cast<unsigned>(zlib_header[0]) << 8) | zlib_header[1];
            if (data_bytes < 6 || (zlib_header[0] & 15) != 8 || (zlib_header[0] >> 4) > 7 ||
                (zlib_header[1] & 32) != 0 || zlib_word % 31 != 0)
                return Error{ErrorCode::InvalidPng, "PNG image data has an invalid zlib header"};
            return dimensions;
        } else {
            if (type == "acTL" || type == "fcTL" || type == "fdAT")
                return Error{ErrorCode::InvalidPng, "Animated PNG charts are not supported"};
            if ((type_bytes[0] & 32) == 0)
                return Error{ErrorCode::InvalidPng, "Unknown critical PNG chunk"};
        }
        if (data && type != "IDAT") ended_data = true;
        offset += static_cast<std::size_t>(length) + 12;
    }
    return Error{ErrorCode::InvalidPng, "PNG is truncated before IEND"};
}

std::vector<std::uint8_t> PixelOnlyPng(std::span<const std::uint8_t> encoded) {
    std::vector<std::uint8_t> bytes(encoded.begin(), encoded.begin() + 8);
    for (std::size_t offset = 8; offset < encoded.size();) {
        const auto length = BigEndian(encoded.subspan(offset, 4));
        const std::string_view type(reinterpret_cast<const char*>(encoded.data() + offset + 4), 4);
        // Keep only image chunks; WIC metadata handlers would otherwise parse text, profile and EXIF data.
        if (type == "IHDR" || type == "PLTE" || type == "IDAT" || type == "IEND" || type == "tRNS")
            bytes.insert(bytes.end(), encoded.begin() + offset, encoded.begin() + offset + length + 12);
        offset += static_cast<std::size_t>(length) + 12;
    }
    return bytes;
}

class Apartment {
public:
    Apartment() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    HRESULT result;
};
Error DecodeError(HRESULT result, std::string_view reason) {
    return {ErrorCode::DecodeFailed, reason, 0, static_cast<std::uint32_t>(result)};
}
class Algorithm {
public:
    ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    BCRYPT_ALG_HANDLE value = nullptr;
};
class Hash {
public:
    ~Hash() { if (value) BCryptDestroyHash(value); }
    BCRYPT_HASH_HANDLE value = nullptr;
};
class File {
public:
    explicit File(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~File() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    File(File&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    File& operator=(File&&) = delete;
    File(const File&) = delete;
    HANDLE Get() const noexcept { return value_; }
private:
    HANDLE value_;
};
Error FileError(std::string_view reason) {
    return {ErrorCode::FileIo, reason, 0, GetLastError()};
}
bool LocalDrive(wchar_t letter) {
    const wchar_t drive[]{letter, L':', L'\0'};
    std::array<wchar_t, 32768> target{};
    if (!QueryDosDeviceW(drive, target.data(), static_cast<DWORD>(target.size()))) return false;
    // Reject DOS-device mappings (SUBST) before opening; drive type alone does not catch them.
    constexpr std::wstring_view prefix = L"\\Device\\HarddiskVolume";
    const std::wstring_view mapping(target.data());
    if (!mapping.starts_with(prefix)) return false;
    const auto number = mapping.substr(prefix.size());
    if (number.empty() || !std::all_of(number.begin(), number.end(),
        [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return false;
    const wchar_t root[]{letter, L':', L'\\', L'\0'};
    const auto type = GetDriveTypeW(root);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE;
}
bool DrivePath(std::wstring_view path) {
    return path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\';
}
bool SafeComponent(std::wstring_view name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') return false;
    if (std::any_of(name.begin(), name.end(), [](wchar_t c) {
        return c < 32 || c == L':' || c == L'/' || c == L'?' || c == L'*' ||
            c == L'<' || c == L'>' || c == L'|' || c == L'"';
    })) return false;
    auto base = name.substr(0, name.find(L'.'));
    constexpr std::array reserved{L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    for (const auto* device : reserved)
        if (CompareStringOrdinal(base.data(), static_cast<int>(base.size()), device, -1, TRUE) == CSTR_EQUAL)
            return false;
    if (base.size() == 4 && ((base[3] >= L'0' && base[3] <= L'9') ||
        base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3')) {
        if (CompareStringOrdinal(base.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(base.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL)
            return false;
    }
    return true;
}
bool ValidUtf16(std::wstring_view value) {
    for (std::size_t i = 0; i != value.size(); ++i) {
        const auto unit = static_cast<std::uint16_t>(value[i]);
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (++i == value.size()) return false;
            const auto low = static_cast<std::uint16_t>(value[i]);
            if (low < 0xdc00 || low > 0xdfff) return false;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) return false;
    }
    return true;
}
struct Snapshot {
    FILE_ID_INFO identity{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
};
bool SnapshotFile(HANDLE file, Snapshot& snapshot) {
    return GetFileInformationByHandleEx(file, FileIdInfo, &snapshot.identity, sizeof snapshot.identity) &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &snapshot.standard, sizeof snapshot.standard) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &snapshot.basic, sizeof snapshot.basic);
}
bool SameFile(const Snapshot& a, const Snapshot& b) {
    return a.identity.VolumeSerialNumber == b.identity.VolumeSerialNumber &&
        std::equal(std::begin(a.identity.FileId.Identifier), std::end(a.identity.FileId.Identifier),
            std::begin(b.identity.FileId.Identifier)) &&
        a.standard.EndOfFile.QuadPart == b.standard.EndOfFile.QuadPart &&
        a.basic.LastWriteTime.QuadPart == b.basic.LastWriteTime.QuadPart &&
        a.basic.ChangeTime.QuadPart == b.basic.ChangeTime.QuadPart;
}
}

namespace detail {
Result<std::string> HashContent(std::uint32_t width, std::uint32_t height,
    std::span<const std::uint32_t> cells, Material material) {
    Algorithm algorithm;
    auto status = BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    auto failure = [](NTSTATUS code) -> Error {
        return {ErrorCode::HashFailed, "BCrypt SHA-256 failed", 0, static_cast<std::uint32_t>(code)};
    };
    if (status < 0) return failure(status);
    ULONG object_size = 0, returned = 0;
    status = BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size), sizeof object_size, &returned, 0);
    if (status < 0) return failure(status);
    if (returned != sizeof object_size || object_size > 64 * 1024)
        return Error{ErrorCode::HashFailed, "Invalid BCrypt hash object size"};
    std::vector<std::uint8_t> object(object_size);
    Hash hash;
    status = BCryptCreateHash(algorithm.value, &hash.value, object.data(), object_size, nullptr, 0, 0);
    if (status < 0) return failure(status);
    std::vector<std::uint8_t> canonical;
    canonical.reserve(24 + cells.size() * 4);
    const auto append = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            canonical.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    append(1);
    append(width); append(height);
    append(material.flags); append(material.corner_argb); append(material.cuff_argb);
    for (const auto cell : cells) append(cell);
    status = BCryptHashData(hash.value, canonical.data(), static_cast<ULONG>(canonical.size()), 0);
    if (status < 0) return failure(status);
    std::array<std::uint8_t, 32> digest{};
    status = BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (status < 0) return failure(status);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
}

Result<DecodedChart> DecodePng(std::span<const std::uint8_t> encoded) {
    return detail::CheckedAllocation<DecodedChart>([&]() -> Result<DecodedChart> {
        const auto envelope = Envelope(encoded);
        if (const auto* error = std::get_if<Error>(&envelope)) return *error;
        const auto dimensions = std::get<Dimensions>(envelope);
        const auto deflate = detail::ValidatePngDeflate(encoded, dimensions.width, dimensions.height,
            dimensions.depth, dimensions.color_type, dimensions.interlaced);
        if (const auto* error = std::get_if<Error>(&deflate)) return *error;
        const auto count = static_cast<std::uint64_t>(dimensions.width) * dimensions.height;
        if (count > maximum_chart_axis * maximum_chart_axis ||
            count * 4 > (std::numeric_limits<UINT>::max)())
            return Error{ErrorCode::Dimensions, "Decoded PNG allocation would overflow"};
        Apartment apartment;
        if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
            return DecodeError(apartment.result, "COM apartment initialization failed");
        ComPtr<IWICImagingFactory> factory;
        auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return DecodeError(hr, "WIC factory creation failed");
        DecodedChart result;
        result.encoded_.assign(encoded.begin(), encoded.end());
        auto codec_bytes = PixelOnlyPng(encoded);
        ComPtr<IWICStream> stream;
        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) return DecodeError(hr, "WIC memory stream creation failed");
        hr = stream->InitializeFromMemory(codec_bytes.data(), static_cast<DWORD>(codec_bytes.size()));
        if (FAILED(hr)) return DecodeError(hr, "WIC memory stream initialization failed");
        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) return DecodeError(hr, "WIC rejected PNG data");
        GUID format{};
        hr = decoder->GetContainerFormat(&format);
        if (FAILED(hr)) return DecodeError(hr, "WIC container query failed");
        if (format != GUID_ContainerFormatPng)
            return Error{ErrorCode::InvalidPng, "WIC container is not PNG"};
        UINT frames = 0;
        hr = decoder->GetFrameCount(&frames);
        if (FAILED(hr)) return DecodeError(hr, "WIC frame-count query failed");
        if (frames != 1) return Error{ErrorCode::InvalidPng, "PNG must contain exactly one frame"};
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return DecodeError(hr, "WIC PNG frame query failed");
        UINT width = 0, height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr)) return DecodeError(hr, "WIC PNG dimensions query failed");
        if (width != dimensions.width || height != dimensions.height)
            return Error{ErrorCode::Dimensions, "PNG decoder and header dimensions disagree"};
        if (dimensions.color_type == 3) {
            GUID pixel_format{};
            hr = frame->GetPixelFormat(&pixel_format);
            if (FAILED(hr)) return DecodeError(hr, "WIC indexed pixel-format query failed");
            unsigned bits = 0;
            if (pixel_format == GUID_WICPixelFormat1bppIndexed) bits = 1;
            else if (pixel_format == GUID_WICPixelFormat2bppIndexed) bits = 2;
            else if (pixel_format == GUID_WICPixelFormat4bppIndexed) bits = 4;
            else if (pixel_format == GUID_WICPixelFormat8bppIndexed) bits = 8;
            else return Error{ErrorCode::InvalidPng, "WIC cannot expose indexed PNG cells for validation"};
            const UINT stride = (width * bits + 7) / 8;
            std::vector<std::uint8_t> indices(static_cast<std::size_t>(stride) * height);
            hr = frame->CopyPixels(nullptr, stride, static_cast<UINT>(indices.size()), indices.data());
            if (FAILED(hr)) return DecodeError(hr, "WIC indexed PNG cells are malformed");
            for (UINT y = 0; y != height; ++y) for (UINT x = 0; x != width; ++x) {
                const auto bit = x * bits;
                const auto index = (indices[static_cast<std::size_t>(y) * stride + bit / 8] >>
                    (8 - bits - bit % 8)) & ((1u << bits) - 1);
                if (index >= dimensions.palette_count)
                    return Error{ErrorCode::InvalidPng, "PNG cell refers outside its encoded palette"};
            }
        }
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return DecodeError(hr, "WIC pixel converter creation failed");
        // Straight RGBA: WIC dither, premultiply or profile conversion would alter user RGB.
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return DecodeError(hr, "WIC RGBA conversion failed");
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(count * 4));
        hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data());
        if (FAILED(hr)) return DecodeError(hr, "WIC PNG pixels are truncated or malformed");
        result.width_ = width; result.height_ = height;
        result.cells_.reserve(static_cast<std::size_t>(count));
        std::set<std::uint32_t> yarns;
        for (std::size_t i = 0; i != rgba.size(); i += 4) {
            const std::uint32_t cell = rgba[i + 3] < 128 ? 0 :
                0xff000000u | (static_cast<std::uint32_t>(rgba[i]) << 16) |
                (static_cast<std::uint32_t>(rgba[i + 1]) << 8) | rgba[i + 2];
            yarns.insert(cell);
            if (yarns.size() > maximum_yarn_slots)
                return Error{ErrorCode::UnsupportedYarnCount,
                    "PNG needs more than sixteen yarn groups; no colors were dropped or recolored"};
            result.cells_.push_back(cell);
        }
        auto hash = detail::HashContent(width, height, result.cells_, {});
        if (const auto* error = std::get_if<Error>(&hash)) return *error;
        result.content_hash_ = std::get<std::string>(std::move(hash));
        return result;
    });
}

Result<FileImport> ImportPngFile(std::wstring_view path) {
    return detail::CheckedAllocation<FileImport>([&]() -> Result<FileImport> {
        if (path.starts_with(L"\\\\"))
            return Error{ErrorCode::RemotePath, "UNC and device paths are not accepted"};
        if (!DrivePath(path) || path.size() <= 3 || path.size() > 32760 ||
            path.find(L'\0') != std::wstring_view::npos || !ValidUtf16(path))
            return Error{ErrorCode::InvalidPath, "Import needs an absolute local drive file path"};
        if (!LocalDrive(path[0]))
            return Error{ErrorCode::RemotePath, "Import drive is not a fixed or removable local disk"};
        std::vector<std::size_t> ends{3};
        for (std::size_t start = 3; start < path.size();) {
            auto end = path.find(L'\\', start);
            if (end == std::wstring_view::npos) end = path.size();
            if (!SafeComponent(path.substr(start, end - start)))
                return Error{ErrorCode::InvalidPath, "Unsafe, device, or noncanonical path component"};
            ends.push_back(end);
            if (end == path.size()) break;
            start = end + 1;
            if (start == path.size())
                return Error{ErrorCode::InvalidPath, "Import path must name a file, not a directory"};
        }
        std::vector<File> pinned;
        pinned.reserve(ends.size());
        Snapshot before;
        for (std::size_t i = 0; i < ends.size(); ++i) {
            const bool last = i + 1 == ends.size();
            const auto component = std::wstring(path.substr(0, ends[i]));
            File file(CreateFileW(component.c_str(), last ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (file.Get() == INVALID_HANDLE_VALUE) return FileError("Cannot open stable local import path");
            if (GetFileType(file.Get()) != FILE_TYPE_DISK)
                return Error{ErrorCode::NotOrdinaryFile, "Import is not a disk file"};
            FILE_BASIC_INFO basic{};
            if (!GetFileInformationByHandleEx(file.Get(), FileBasicInfo, &basic, sizeof basic))
                return FileError("Cannot inspect import path attributes");
            if (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                return Error{ErrorCode::ReparsePoint, "Reparse traversal is not allowed for imports"};
            if (basic.FileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
                return Error{ErrorCode::NotOrdinaryFile, "Offline or remotely recalled import files are not accepted"};
            if ((!last && !(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ||
                (last && (basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))))
                return Error{ErrorCode::NotOrdinaryFile, "Import path is not an ordinary file in ordinary directories"};
            if (last && !SnapshotFile(file.Get(), before))
                return FileError("Cannot establish import file identity");
            pinned.push_back(std::move(file));
        }
        if (before.standard.DeletePending || before.standard.Directory)
            return Error{ErrorCode::NotOrdinaryFile, "Import file is being deleted or is a directory"};
        if (before.standard.EndOfFile.QuadPart <= 0 ||
            before.standard.EndOfFile.QuadPart > static_cast<LONGLONG>(maximum_png_bytes))
            return Error{ErrorCode::EncodedLimit, "Import file must contain 1 byte..2 MiB"};
        auto& file = pinned.back();
        std::vector<wchar_t> final_path(32768);
        const auto final_length = GetFinalPathNameByHandleW(file.Get(), final_path.data(),
            static_cast<DWORD>(final_path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!final_length) return FileError("Cannot verify final local file location");
        if (final_length >= final_path.size())
            return Error{ErrorCode::InvalidPath, "Final import path exceeds supported bound"};
        std::wstring_view final(final_path.data(), final_length);
        if (!final.starts_with(L"\\\\?\\") || !DrivePath(final.substr(4)) ||
            !LocalDrive(final[4]))
            return Error{ErrorCode::RemotePath, "Final import location is not a local drive"};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(before.standard.EndOfFile.QuadPart));
        std::size_t read = 0;
        while (read < bytes.size()) {
            DWORD received = 0;
            if (!ReadFile(file.Get(), bytes.data() + read, static_cast<DWORD>(bytes.size() - read), &received, nullptr))
                return FileError("Local PNG read failed");
            if (!received) return Error{ErrorCode::FileChanged, "Import file became shorter during read"};
            read += received;
        }
        std::uint8_t extra = 0;
        DWORD received = 0;
        if (!ReadFile(file.Get(), &extra, 1, &received, nullptr))
            return FileError("Cannot check import file end");
        Snapshot after;
        if (!SnapshotFile(file.Get(), after)) return FileError("Cannot revalidate import file identity");
        if (received || !SameFile(before, after))
            return Error{ErrorCode::FileChanged, "Import file identity or content metadata changed during read"};
        auto decoded = DecodePng(bytes);
        if (const auto* error = std::get_if<Error>(&decoded)) return *error;
        LocalFileIdentity identity{before.identity.VolumeSerialNumber, {}};
        std::copy(std::begin(before.identity.FileId.Identifier), std::end(before.identity.FileId.Identifier),
            identity.file_id.begin());
        return FileImport{std::get<DecodedChart>(std::move(decoded)), identity};
    });
}

}
