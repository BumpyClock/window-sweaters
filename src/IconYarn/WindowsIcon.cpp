#include "WindowsIcon.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace window_sweaters::icon_yarn {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::size_t MaximumExecutableBytes = 64 * 1024 * 1024;
constexpr std::size_t MaximumDirectoryBytes = 1024 * 1024;
constexpr std::size_t MaximumImageBytes = 256 * 1024;
constexpr unsigned MaximumIconSide = 256;

class Handle final {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE Get() const noexcept { return value_; }
    HANDLE Release() noexcept { return std::exchange(value_, nullptr); }
private:
    HANDLE value_;
};
class OwnedIcon final {
public:
    explicit OwnedIcon(HICON value) : value_(value) {}
    ~OwnedIcon() { if (value_) DestroyIcon(value_); }
    HICON Get() const noexcept { return value_; }
private:
    HICON value_;
};
class IconBitmaps final {
public:
    ICONINFO info{};
    ~IconBitmaps() {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
};
class Apartment final {
public:
    Apartment() : status_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Apartment() { if (SUCCEEDED(status_)) CoUninitialize(); }
    HRESULT Status() const noexcept { return status_; }
private:
    HRESULT status_;
};

std::optional<Fallback> Interrupted(std::stop_token stop,
                                   std::chrono::steady_clock::time_point deadline) noexcept {
    if (stop.stop_requested()) return Fallback{FallbackReason::Cancelled};
    if (std::chrono::steady_clock::now() >= deadline)
        return Fallback{FallbackReason::DeadlineExceeded};
    return std::nullopt;
}

bool LocalPath(std::wstring_view path) noexcept {
    if (path.size() < 4 || path.size() > 4096 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return false;
    if (path.find_first_of(L"/:*?\"<>|", 2) != std::wstring::npos ||
        path.find(L'\0') != std::wstring::npos) return false;
    std::size_t start = 3;
    while (start < path.size()) {
        const std::size_t end = path.find(L'\\', start);
        const auto component = path.substr(start, end == std::wstring::npos ? end : end - start);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ') return false;
        const auto base = component.substr(0, component.find(L'.'));
        const auto matches = [&](std::wstring_view reserved) {
            return CompareStringOrdinal(base.data(), static_cast<int>(base.size()),
                                        reserved.data(), static_cast<int>(reserved.size()), TRUE) ==
                   CSTR_EQUAL;
        };
        if (matches(L"CON") || matches(L"PRN") || matches(L"AUX") || matches(L"NUL") ||
            matches(L"CLOCK$")) return false;
        if (base.size() == 4 && base[3] >= L'1' && base[3] <= L'9') {
            const auto prefix = base.substr(0, 3);
            if (CompareStringOrdinal(prefix.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
                CompareStringOrdinal(prefix.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL)
                return false;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return path.back() != L'\\';
}

thread_local std::uint64_t bytesRead = 0;

// Bounded range reads from the validated handle; no whole-file buffer.
class Source final {
public:
    Source(HANDLE file, HANDLE event, std::uint64_t size, std::stop_token stop,
           std::chrono::steady_clock::time_point deadline)
        : file_(file), event_(event), size_(size), stop_(stop), deadline_(deadline) {}
    std::uint64_t Size() const noexcept { return size_; }
    const std::optional<Fallback>& Failure() const noexcept { return failure_; }
    std::optional<std::vector<std::uint8_t>> Read(std::uint64_t offset, std::uint64_t length) {
        if (failure_ || offset > size_ || length > size_ - offset || length > MaximumDirectoryBytes)
            return std::nullopt;
        if (auto interrupted = Interrupted(stop_, deadline_)) { failure_ = *interrupted; return std::nullopt; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (bytes.empty()) return bytes;
        ResetEvent(event_.Get());
        OVERLAPPED operation{};
        operation.hEvent = event_.Get();
        operation.Offset = static_cast<DWORD>(offset);
        operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!ReadFile(file_.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, &operation) &&
            GetLastError() != ERROR_IO_PENDING) {
            failure_ = Fallback{FallbackReason::IconUnavailable, GetLastError()};
            return std::nullopt;
        }
        for (;;) {
            if (GetOverlappedResult(file_.Get(), &operation, &read, FALSE)) break;
            const DWORD error = GetLastError();
            if (error != ERROR_IO_INCOMPLETE) {
                failure_ = Fallback{FallbackReason::IconUnavailable, error};
                return std::nullopt;
            }
            if (auto interrupted = Interrupted(stop_, deadline_)) {
                CancelIoEx(file_.Get(), &operation);
                // OVERLAPPED and its buffer must outlive kernel cancellation completion.
                GetOverlappedResult(file_.Get(), &operation, &read, TRUE);
                failure_ = *interrupted;
                return std::nullopt;
            }
            WaitForSingleObject(event_.Get(), 5);
        }
        bytesRead += read;
        if (read != bytes.size()) return std::nullopt;
        return bytes;
    }
private:
    Handle file_, event_;
    std::uint64_t size_;
    std::stop_token stop_;
    std::chrono::steady_clock::time_point deadline_;
    std::optional<Fallback> failure_;
};

using FileResult = std::variant<std::unique_ptr<Source>, Fallback>;
FileResult OpenExecutable(const std::wstring& path, std::stop_token stop,
                          std::chrono::steady_clock::time_point deadline) {
    if (path.empty()) return Fallback{FallbackReason::IconUnavailable};
    if (!LocalPath(path)) return Fallback{FallbackReason::UnsupportedSource};
    const wchar_t root[]{path[0], L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED)
        return Fallback{FallbackReason::UnsupportedSource};
    // Reject intermediate junctions/symlinks before opening, not just the final file.
    for (std::size_t end = path.find(L'\\', 3); end != std::wstring::npos;
         end = path.find(L'\\', end + 1)) {
        const DWORD attributes = GetFileAttributesW(path.substr(0, end).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return Fallback{FallbackReason::IconUnavailable, GetLastError()};
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return Fallback{FallbackReason::UnsupportedSource};
    }
    if (auto interrupted = Interrupted(stop, deadline)) return *interrupted;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE)
        return Fallback{FallbackReason::IconUnavailable, GetLastError()};
    if (GetFileType(file.Get()) != FILE_TYPE_DISK)
        return Fallback{FallbackReason::UnsupportedSource};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file.Get(), FileAttributeTagInfo,
                                     &attributes, sizeof attributes))
        return Fallback{FallbackReason::IconUnavailable, GetLastError()};
    if (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        return Fallback{FallbackReason::UnsupportedSource};
    // Parent attributes can change between query/open. Verify the opened handle's final location.
    std::array<wchar_t, 4097> finalPath{};
    const DWORD characters = GetFinalPathNameByHandleW(file.Get(), finalPath.data(),
        static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (characters < 7 || characters >= finalPath.size() ||
        std::wstring_view(finalPath.data(), 4) != L"\\\\?\\" ||
        !LocalPath(std::wstring_view(finalPath.data() + 4, characters - 4)) ||
        CompareStringOrdinal(finalPath.data() + 4, static_cast<int>(characters - 4),
                             path.c_str(), static_cast<int>(path.size()), TRUE) != CSTR_EQUAL)
        return Fallback{FallbackReason::UnsupportedSource};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size))
        return Fallback{FallbackReason::IconUnavailable, GetLastError()};
    if (size.QuadPart <= 0) return Fallback{FallbackReason::InvalidResource};
    if (size.QuadPart > MaximumExecutableBytes)
        return Fallback{FallbackReason::SourceTooLarge};
    if (auto interrupted = Interrupted(stop, deadline)) return *interrupted;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.Get()) return Fallback{FallbackReason::ResourceLimit, GetLastError()};
    return std::make_unique<Source>(file.Release(), event.Release(),
                                    static_cast<std::uint64_t>(size.QuadPart), stop, deadline);
}

template<typename T>
std::optional<T> Read(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return std::nullopt;
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof value);
    return value;
}

class Resources final {
public:
    explicit Resources(Source& file) : file_(file) {}
    bool Initialize() {
        constexpr std::size_t MaximumHeaders = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                                               sizeof(IMAGE_OPTIONAL_HEADER64);
        const auto dosBytes = file_.Read(0, std::min<std::uint64_t>(sizeof(IMAGE_DOS_HEADER), file_.Size()));
        if (!dosBytes) return false;
        const auto dos = Read<IMAGE_DOS_HEADER>(*dosBytes, 0);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
        const auto nt = static_cast<std::uint64_t>(dos->e_lfanew);
        if (nt > file_.Size()) return false;
        const auto headers = file_.Read(nt, std::min<std::uint64_t>(MaximumHeaders, file_.Size() - nt));
        if (!headers) return false;
        const auto signature = Read<DWORD>(*headers, 0);
        const auto header = Read<IMAGE_FILE_HEADER>(*headers, sizeof(DWORD));
        if (!signature || *signature != IMAGE_NT_SIGNATURE || !header ||
            !header->NumberOfSections || header->NumberOfSections > 96) return false;
        const std::size_t optionalOffset = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        const auto magic = Read<WORD>(*headers, optionalOffset);
        IMAGE_DATA_DIRECTORY directory{};
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            const auto optional = Read<IMAGE_OPTIONAL_HEADER64>(*headers, optionalOffset);
            if (!optional || header->SizeOfOptionalHeader < sizeof(*optional) ||
                optional->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return false;
            directory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            const auto optional = Read<IMAGE_OPTIONAL_HEADER32>(*headers, optionalOffset);
            if (!optional || header->SizeOfOptionalHeader < sizeof(*optional) ||
                optional->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE) return false;
            directory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        } else return false;
        const auto sections = nt + optionalOffset + header->SizeOfOptionalHeader;
        if (sections > file_.Size()) return false;
        const auto available = std::min<std::uint64_t>(
            header->NumberOfSections * sizeof(IMAGE_SECTION_HEADER), file_.Size() - sections);
        const auto table = file_.Read(sections, available);
        if (!table) return false;
        sections_ = std::move(*table);
        sectionCount_ = header->NumberOfSections;
        if (!directory.Size || directory.Size > MaximumDirectoryBytes) return false;
        auto data = AtRva(directory.VirtualAddress, directory.Size);
        if (!data) return false;
        directory_ = std::move(*data);
        return true;
    }

    std::optional<std::vector<std::uint8_t>> Resource(WORD type, std::optional<WORD> id) {
        const auto typeEntry = Entry(0, type);
        if (!typeEntry || !typeEntry->DataIsDirectory) return std::nullopt;
        const auto name = Entry(typeEntry->OffsetToDirectory, id);
        if (!name || !name->DataIsDirectory) return std::nullopt;
        const auto language = Entry(name->OffsetToDirectory, std::nullopt);
        if (!language || language->DataIsDirectory) return std::nullopt;
        const auto data = Read<IMAGE_RESOURCE_DATA_ENTRY>(directory_, language->OffsetToData);
        if (!data || !data->Size || data->Size > MaximumImageBytes) return std::nullopt;
        return AtRva(data->OffsetToData, data->Size);
    }
private:
    std::optional<std::vector<std::uint8_t>> AtRva(DWORD rva, DWORD length) {
        for (unsigned i = 0; i < sectionCount_; ++i) {
            const auto section = Read<IMAGE_SECTION_HEADER>(sections_, i * sizeof(IMAGE_SECTION_HEADER));
            if (!section) return std::nullopt;
            if (rva < section->VirtualAddress) continue;
            const auto relative = static_cast<std::uint64_t>(rva) - section->VirtualAddress;
            if (relative > section->SizeOfRawData ||
                length > section->SizeOfRawData - relative) continue;
            const auto offset = static_cast<std::uint64_t>(section->PointerToRawData) + relative;
            return file_.Read(offset, length);
        }
        return std::nullopt;
    }

    std::optional<IMAGE_RESOURCE_DIRECTORY_ENTRY> Entry(
        DWORD offset, std::optional<WORD> id) const noexcept {
        const auto table = Read<IMAGE_RESOURCE_DIRECTORY>(directory_, offset);
        if (!table) return std::nullopt;
        const unsigned count = table->NumberOfNamedEntries + table->NumberOfIdEntries;
        if (!count || count > 1024) return std::nullopt;
        for (unsigned i = 0; i < count; ++i) {
            const auto entry = Read<IMAGE_RESOURCE_DIRECTORY_ENTRY>(
                directory_, static_cast<std::size_t>(offset) + sizeof(*table) +
                            i * sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY));
            if (!entry) return std::nullopt;
            if (!id || (!entry->NameIsString && entry->Id == *id)) return entry;
        }
        return std::nullopt;
    }
    Source& file_;
    std::vector<std::uint8_t> sections_, directory_;
    unsigned sectionCount_{};
};

bool BoundedImage(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::uint8_t png[]{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() >= 24 && std::equal(std::begin(png), std::end(png), bytes.begin())) {
        if (bytes[8] || bytes[9] || bytes[10] || bytes[11] != 13 ||
            std::memcmp(bytes.data() + 12, "IHDR", 4)) return false;
        const auto dimension = [&](std::size_t offset) {
            return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                   (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
        };
        const auto width = dimension(16), height = dimension(20);
        return width && width <= MaximumIconSide && height && height <= MaximumIconSide;
    }
    const auto dib = Read<BITMAPINFOHEADER>(bytes, 0);
    if (!dib || (dib->biSize != sizeof(BITMAPINFOHEADER) &&
                 dib->biSize != sizeof(BITMAPV4HEADER) && dib->biSize != sizeof(BITMAPV5HEADER)) ||
        dib->biSize > bytes.size() || dib->biWidth <= 0 || dib->biWidth > MaximumIconSide ||
        dib->biHeight <= 0 || dib->biHeight > MaximumIconSide * 2 ||
        dib->biHeight % 2 || dib->biPlanes != 1 ||
        (dib->biBitCount != 1 && dib->biBitCount != 4 && dib->biBitCount != 8 &&
         dib->biBitCount != 16 && dib->biBitCount != 24 && dib->biBitCount != 32) ||
        (dib->biCompression != BI_RGB && dib->biCompression != BI_BITFIELDS)) return false;
    if (dib->biCompression == BI_BITFIELDS && dib->biBitCount != 16 && dib->biBitCount != 32)
        return false;
    std::size_t colorEntries = 0;
    if (dib->biBitCount <= 8) {
        const std::size_t maximum = std::size_t{1} << dib->biBitCount;
        if (dib->biClrUsed > maximum) return false;
        colorEntries = dib->biClrUsed ? dib->biClrUsed : maximum;
    } else if (dib->biClrUsed) return false;
    const auto width = static_cast<std::size_t>(dib->biWidth);
    const auto height = static_cast<std::size_t>(dib->biHeight / 2);
    const auto colorStride = ((width * dib->biBitCount + 31) / 32) * 4;
    const auto maskStride = ((width + 31) / 32) * 4;
    const auto bitfields = dib->biSize == sizeof(BITMAPINFOHEADER) &&
                           dib->biCompression == BI_BITFIELDS ? 12 : 0;
    const auto minimum = dib->biSize + colorEntries * sizeof(RGBQUAD) + bitfields +
                         (colorStride + maskStride) * height;
    return bytes.size() >= minimum;
}

} // namespace

PixelResult PixelsFromOwnedIcon(HICON icon) {
    if (!icon) return Fallback{FallbackReason::IconUnavailable};
    IconBitmaps bitmaps;
    if (!GetIconInfo(icon, &bitmaps.info))
        return Fallback{FallbackReason::DecodeFailed, GetLastError()};
    BITMAP bitmap{};
    const auto source = bitmaps.info.hbmColor ? bitmaps.info.hbmColor : bitmaps.info.hbmMask;
    if (!source || !GetObjectW(source, sizeof bitmap, &bitmap))
        return Fallback{FallbackReason::DecodeFailed, GetLastError()};
    const LONG height = bitmaps.info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
    if (bitmap.bmWidth <= 0 || bitmap.bmWidth > MaximumIconSide ||
        height <= 0 || height > MaximumIconSide)
        return Fallback{FallbackReason::SourceTooLarge};

    Apartment apartment;
    if (FAILED(apartment.Status()) && apartment.Status() != RPC_E_CHANGED_MODE)
        return Fallback{FallbackReason::DecodeFailed, static_cast<std::uint32_t>(apartment.Status())};
    ComPtr<IWICImagingFactory> factory;
    HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmap> image;
    if (SUCCEEDED(status)) status = factory->CreateBitmapFromHICON(icon, &image);
    UINT width = 0, imageHeight = 0;
    WICPixelFormatGUID format{};
    if (SUCCEEDED(status)) status = image->GetSize(&width, &imageHeight);
    if (SUCCEEDED(status)) status = image->GetPixelFormat(&format);
    if (SUCCEEDED(status) && (!width || width > MaximumIconSide ||
        !imageHeight || imageHeight > MaximumIconSide ||
        (format != GUID_WICPixelFormat32bppBGRA && format != GUID_WICPixelFormat32bppPBGRA)))
        status = E_INVALIDARG;
    std::vector<BYTE> raw;
    ComPtr<IWICBitmap> premultiplied;
    if (SUCCEEDED(status)) {
        raw.resize(static_cast<std::size_t>(width) * imageHeight * 4);
        status = image->CopyPixels(nullptr, width * 4, static_cast<UINT>(raw.size()), raw.data());
    }
    // WIC's HICON bridge labels premultiplied GDI icon bytes as BGRA. Reinterpret
    // those bytes as PBGRA before scaling/conversion, or alpha is multiplied twice.
    if (SUCCEEDED(status)) status = factory->CreateBitmapFromMemory(width, imageHeight,
        GUID_WICPixelFormat32bppPBGRA, width * 4, static_cast<UINT>(raw.size()),
        raw.data(), &premultiplied);
    ComPtr<IWICBitmapScaler> scaler;
    if (SUCCEEDED(status)) status = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(status)) status = scaler->Initialize(premultiplied.Get(), static_cast<UINT>(IconSide),
        static_cast<UINT>(IconSide), WICBitmapInterpolationModeFant);
    if (SUCCEEDED(status)) status = scaler->GetPixelFormat(&format);
    if (SUCCEEDED(status) && format != GUID_WICPixelFormat32bppPBGRA) status = E_INVALIDARG;
    IconPixels pixels{};
    static_assert(sizeof(PremultipliedRgba8) == 4);
    if (SUCCEEDED(status)) status = scaler->CopyPixels(nullptr, static_cast<UINT>(IconSide * 4),
        static_cast<UINT>(sizeof pixels), reinterpret_cast<BYTE*>(pixels.data()));
    if (FAILED(status))
        return Fallback{FallbackReason::DecodeFailed, static_cast<std::uint32_t>(status)};
    // A byte swap preserves exact premultiplication; WIC's RGBA converter can
    // unpremultiply/re-premultiply and lose one byte at partial-alpha boundaries.
    for (auto& pixel : pixels) std::swap(pixel.r, pixel.b);
    return pixels;
}

std::uint64_t ExecutableBytesRead() noexcept { return bytesRead; }

PaletteResult SampleExecutableIcon(const IconInput& input, std::stop_token stop,
                                  std::chrono::steady_clock::time_point deadline) {
    bytesRead = 0;
    if (auto interrupted = Interrupted(stop, deadline)) return *interrupted;
    auto opened = OpenExecutable(input.executablePath, stop, deadline);
    if (const auto failure = std::get_if<Fallback>(&opened)) return *failure;
    Source& file = *std::get<std::unique_ptr<Source>>(opened);
    const auto failed = [&](FallbackReason otherwise) -> Fallback {
        return file.Failure() ? *file.Failure() : Fallback{otherwise};
    };
    Resources resources(file);
    if (!resources.Initialize()) return failed(FallbackReason::InvalidResource);
    const auto group = resources.Resource(14, std::nullopt);
    if (!group) return failed(FallbackReason::IconUnavailable);
    const auto reserved = Read<WORD>(*group, 0), type = Read<WORD>(*group, 2),
               count = Read<WORD>(*group, 4);
    if (!reserved || *reserved || type != 1 || !count || !*count || *count > 256 ||
        group->size() < 6 + static_cast<std::size_t>(*count) * 14)
        return Fallback{FallbackReason::InvalidResource};
    std::optional<WORD> best;
    unsigned bestDistance = std::numeric_limits<unsigned>::max();
    for (unsigned i = 0; i < *count; ++i) {
        const std::size_t offset = 6 + i * 14;
        const unsigned width = (*group)[offset] ? (*group)[offset] : 256;
        const unsigned height = (*group)[offset + 1] ? (*group)[offset + 1] : 256;
        const auto id = Read<WORD>(*group, offset + 12);
        const unsigned distance = static_cast<unsigned>(std::abs(static_cast<int>(width) - 48) +
                                                        std::abs(static_cast<int>(height) - 48));
        if (id && distance < bestDistance) { best = id; bestDistance = distance; }
    }
    if (!best) return Fallback{FallbackReason::InvalidResource};
    const auto image = resources.Resource(3, best);
    if (!image) return failed(FallbackReason::InvalidResource);
    if (!BoundedImage(*image)) return Fallback{FallbackReason::InvalidResource};
    if (auto interrupted = Interrupted(stop, deadline)) return *interrupted;
    OwnedIcon icon(CreateIconFromResourceEx(const_cast<PBYTE>(image->data()),
        static_cast<DWORD>(image->size()), TRUE, 0x00030000,
        static_cast<int>(IconSide), static_cast<int>(IconSide), LR_DEFAULTCOLOR));
    if (!icon.Get()) return Fallback{FallbackReason::DecodeFailed, GetLastError()};
    auto pixels = PixelsFromOwnedIcon(icon.Get());
    if (const auto failure = std::get_if<Fallback>(&pixels)) return *failure;
    if (auto interrupted = Interrupted(stop, deadline)) return *interrupted;
    return SamplePalette(std::get<IconPixels>(pixels));
}

} // namespace window_sweaters::icon_yarn
