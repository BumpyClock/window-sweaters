#include "..\..\src\IconYarn\IconMotif.h"
#include "..\..\src\IconYarn\WindowsIcon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace window_sweaters::icon_yarn;
using namespace std::chrono_literals;

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Failure(const PaletteResult& result, FallbackReason reason) {
    const auto failed = std::get_if<Fallback>(&result);
    Require(failed && failed->reason == reason, "wrong explicit fallback outcome");
}

SampledPalette Palette(const PaletteResult& result) {
    const auto palette = std::get_if<SampledPalette>(&result);
    Require(palette != nullptr, "expected a sampled palette");
    return *palette;
}

IconPixels Solid(PremultipliedRgba8 pixel) {
    IconPixels pixels{};
    pixels.fill(pixel);
    return pixels;
}

double Lightness(std::uint32_t color) {
    const unsigned r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
    return (std::max({r, g, b}) + std::min({r, g, b})) / 510.0;
}

void Sampling() {
    Failure(SamplePalette({}), FallbackReason::InvalidPixels);
    Failure(SamplePalette(Solid({255, 0, 0, 217})), FallbackReason::InvalidPixels);
    Failure(SamplePalette(Solid({})), FallbackReason::InsufficientColorCoverage);
    Failure(SamplePalette(Solid({136, 136, 136, 255})), FallbackReason::InsufficientColorCoverage);
    Failure(SamplePalette(Solid({40, 40, 60, 255})), FallbackReason::InsufficientColorCoverage);
    Failure(SamplePalette(Solid({58, 58, 42, 255})), FallbackReason::InsufficientColorCoverage);
    Failure(SamplePalette(Solid({24, 0, 0, 255})), FallbackReason::InsufficientColorCoverage);
    Failure(SamplePalette(Solid({216, 0, 0, 216})), FallbackReason::InsufficientColorCoverage);
    Require(Palette(SamplePalette(Solid({217, 0, 0, 217}))) ==
            SampledPalette{0xffc04b4bu, 0xfff6f0deu}, "alpha217 unpremultiplication");
    Require(Palette(SamplePalette(Solid({255, 0, 0, 255}))) ==
            SampledPalette{0xffc04b4bu, 0xfff6f0deu}, "literal softened red palette");
    Require(Palette(SamplePalette(Solid({238, 0, 0, 238}))).baseArgb == 0xffc04b4bu,
            "premultiplied red has same histogram bin");
    Require(Palette(SamplePalette(Solid({51, 119, 221, 255}))) ==
            SampledPalette{0xff6685b4u, 0xfff6f0deu}, "literal softened blue palette");

    auto coverage = Solid({80, 80, 80, 255});
    std::fill_n(coverage.begin(), 37, PremultipliedRgba8{255, 149, 0, 255});
    Failure(SamplePalette(coverage), FallbackReason::InsufficientColorCoverage);
    coverage[37] = {255, 149, 0, 255};
    Require(Palette(SamplePalette(coverage)).baseArgb == 0xffc0894bu,
            "38 orange pixels commit color;4096bin floor quantization");

    Require(Soften(0xffff0000u) == 0xffc04b4bu, "literal softening");
    Require(Soften(0xffaaffaau) == 0xff95d995u, "lightness .72 clamp");
    Require(Soften(0xff000000u) == 0xff5d3b3bu, "saturation .22/lightness .30 clamps");
    Require(ZigzagContrast(0xff9d9d9du) == 0xfff6f0deu, "Zigzag below .62");
    Require(ZigzagContrast(0xff9f9f9fu) == 0xff5c5c5cu, "Zigzag above .62");
    Require(ZigzagContrast(0xffffffffu) == 0xffbcbcbcu, "white Zigzag contrast");
    const auto pastel = Palette(SamplePalette(Solid({170, 255, 170, 255})));
    Require(pastel.baseArgb == 0xff95d995u &&
            Lightness(pastel.baseArgb) - Lightness(pastel.contrastArgb) >= .20,
            "light pastel needs deeper contrast, not cream");

    auto colors = Solid({255, 0, 0, 255});
    std::fill_n(colors.begin() + 1700, 604, PremultipliedRgba8{0, 0, 68, 255});
    Require(Palette(SamplePalette(colors)) == SampledPalette{0xffc04b4bu, 0xff272771u},
            "raw lightness-separated accent survives softening");
    std::fill_n(colors.begin() + 1700, 604, PremultipliedRgba8{0, 0, 136, 255});
    Require(Palette(SamplePalette(colors)).contrastArgb == 0xfff6f0deu,
            "contrast rechecked after softening compresses raw difference");
    auto tie = Solid({255, 0, 0, 255});
    std::fill_n(tie.begin(), 1152, PremultipliedRgba8{0, 255, 0, 255});
    Require(Palette(SamplePalette(tie)) == SampledPalette{0xff4bc04bu, 0xfff6f0deu},
            "equal counts choose ascending histogram bin deterministically");
    auto wrap = Solid({255, 0, 0, 255});
    std::fill_n(wrap.begin() + 1400, 500, PremultipliedRgba8{255, 0, 17, 255});
    std::fill_n(wrap.begin() + 1900, 300, PremultipliedRgba8{0, 255, 0, 255});
    std::fill_n(wrap.begin() + 2200, 104, PremultipliedRgba8{0, 0, 68, 255});
    Require(Palette(SamplePalette(wrap)) == SampledPalette{0xffc04b4bu, 0xff272771u},
            "circular hue separation reserves third candidate for real contrast");

    for (unsigned r = 17; r < 256; r += 34)
        for (unsigned g = 17; g < 256; g += 34)
            for (unsigned b = 17; b < 256; b += 34) {
                auto grid = Solid({static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                   static_cast<std::uint8_t>(b), 255});
                std::fill_n(grid.begin(), 500, PremultipliedRgba8{17, 51, 119, 255});
                const auto result = SamplePalette(grid);
                if (const auto palette = std::get_if<SampledPalette>(&result))
                    Require(std::abs(Lightness(palette->baseArgb) -
                                     Lightness(palette->contrastArgb)) >= .195,
                            "final byte-quantized contrast grid");
            }
    Require(StableMotif("exe:Example") == Motif::Zigzag, "persistent identity literal motif");
    Require(StableMotif("exe:example") == Motif::Picnic, "motif FNV is case-sensitive");
    Require(MotifName(Motif::Zigzag) == "zigzag" && MotifName(Motif::Twinkle) == "twinkle",
            "authored motif names");
    std::cout << "PASS portable sampling, alpha, coverage, faint-gray, contrast and motif\n";
}

bool Isolated(const MotifRows& rows) {
    const auto width = static_cast<int>(rows[0].size());
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < width; ++x) {
            if (rows[y][x] == '.') continue;
            bool neighbour = false;
            for (int yy = std::max(0, y - 1); yy < std::min(6, y + 2); ++yy)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((yy != y || dx) && rows[yy][(x + dx + width) % width] != '.') neighbour = true;
            if (!neighbour) return true;
        }
    return false;
}

void Motifs() {
    struct Golden { std::uint32_t seed; std::string_view glyph; MotifRows rows; };
    const Golden goldens[]{
        {0, "", {"aa..aa..a", "..a.a....", "aababaa..", "..aaa....", "aababaa..", "..a.a...."}},
        {1, "", {"..aa........aa..", ".aaaa......aaaa.", "aabbaa....aabbaa", "aabbaabbbbaabbaa", ".aaaa.bbbb.aaaa.", "..aa........aa.."}},
        {2, "", {"............", "..a.....a...", ".aba...aba..", "aabaa.aabaa.", ".aba...aba..", "..a.....a..."}},
        {3, "", {"..ab.ba...", "aabbabbaa.", "..ababa...", "aabbabbaa.", "..ab.ba...", "aa..aa..aa"}},
        {4, "", {"..a..aaa", ".aba....", "aabaabbb", ".aba.bbb", "..a.....", "........"}},
        {0x9e3779b9u, "", {".......", ".a.a...", ".bab...", ".aaa...", ".bab...", ".a.a..."}},
        {0xdeadbeefu, "", {"..aa......aa....", ".aaaa....aaaa...", "aabbaa..aabbaa..", "aabbaa..aabbaa..", ".aaaa....aaaa...", "..aa......aa...."}},
        {0xffffffffu, "", {"..a.a...", "aababaa.", "..aaa...", "aababaa.", "..a.a...", "aa..aa.."}},
        {1234567, ".aaa.aabaa..b...aaa.a...a", {".aaa....", "aabaabbb", "..b..bbb", ".aaa....", "a...a...", "........"}},
        {7654321, ".aaa.aabaa..b...aaa.a...a", {".aaa........aaa.", "aabaabbbbbbaabaa", "..b..bbbbbb..b..", ".aaa........aaa.", "a...a......a...a", "................"}},
        {7654322, ".aaa.aabaa..b...aaa.a...a", {".aaa...a...a..", "aabaa...aaa...", "..b......b....", ".aaa...aabaa..", "a...a...aaa...", ".............."}},
        {99, "aa..aaab..baab..baab..ba..aa..aa..aa", {"aa..a..aa.", "ab..b..ba.", "aa..a..aa.", "ab..b..ba.", "aa..a..aa.", "aa..aa..aa"}},
        {0xe2acd194u, "..aa....aab.aaaaaaaaaaaa..aa....aa..", {"..aa......aa....", "..aa......aa....", "aaaaaa..aaaaaa..", "aaaaaa..aaaaaa..", "..aa......aa....", "..aa......aa...."}},
        {0xe38465f3u, "bbbbbbbbbbbbaaaaaaaaaaaa............", {"bbbbbbbbbbbbbbbbbb", "bbbbbbbbbbbbbbbbbb", "aaaaaa......aaaaaa", "aaaaaa......aaaaaa", "..................", ".................."}},
    };
    for (const auto& golden : goldens)
        Require(GenerateMotif(golden.seed, golden.glyph) == golden.rows, "motif matches icongen.py golden rows");
    const std::string_view glyphs[]{"", ".aaa.aabaa..b...aaa.a...a", "aa..aaab..baab..baab..ba..aa..aa..aa", DiamondGlyph(6)};
    for (std::uint32_t seed = 0; seed < 3000; ++seed)
        for (const auto glyph : glyphs) {
            const auto rows = GenerateMotif(seed * 2654435761u, glyph);
            Require(rows == GenerateMotif(seed * 2654435761u, glyph), "motif is deterministic");
            const auto width = rows[0].size();
            Require(width >= 4 && width <= 24, "motif width stays within 4..24");
            for (const auto& row : rows)
                Require(row.size() == width && row.find_first_not_of(".ab") == std::string::npos, "motif rows are uniform '.ab'");
            Require(!Isolated(rows), "motif has no isolated stitches");
        }
    bool threw = false;
    try { (void)GenerateMotif(1, "abc"); } catch (const std::invalid_argument&) { threw = true; }
    Require(threw, "invalid glyph is rejected");
    std::cout << "PASS icon motif generators match icongen.py and keep 6-row '.ab' charts\n";
}

WorkToken Token(std::uint64_t target, std::uint64_t revision = 1) {
    return {target, 100, 1, revision, 1};
}
IconInput Input(std::string identity = "exe:Example", std::uint64_t revision = 1) {
    return {std::move(identity), revision, L"C:\\Fixture\\Own.exe"};
}
WorkRequest Request(CompletionGate& gate, WorkToken token, IconInput input = Input()) {
    auto request = gate.Begin(token, std::move(input));
    Require(request.has_value(), "valid request must be admitted by gate");
    return *request;
}

class QueryBarrier final {
public:
    PaletteResult Query(const IconInput& input, std::stop_token stop,
                        std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        ++started_;
        changed_.notify_all();
        changed_.wait_until(lock, stop, deadline, [&] { return open_ || input.iconRevision == 2; });
        if (stop.stop_requested()) return Fallback{FallbackReason::Cancelled};
        if (!open_ && input.iconRevision != 2) return Fallback{FallbackReason::DeadlineExceeded};
        return input.iconRevision == 2 ? PaletteResult{SampledPalette{0xff6685b4u, 0xfff6f0deu}}
                                       : PaletteResult{SampledPalette{0xffc44646u, 0xfff6f0deu}};
    }
    void Wait(std::size_t count) {
        std::unique_lock lock(mutex_);
        Require(changed_.wait_for(lock, 3s, [&] { return started_ >= count; }), "worker start deadline");
    }
    void Open() {
        std::lock_guard lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }
    std::size_t Started() {
        std::lock_guard lock(mutex_);
        return started_;
    }
private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::size_t started_{};
    bool open_{};
};

std::vector<Completion> WaitCompletions(PreparationWorker& worker, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::vector<Completion> results;
    while (results.size() < count && std::chrono::steady_clock::now() < deadline) {
        auto ready = worker.Drain();
        for (auto& completion : ready) results.push_back(std::move(completion));
        if (results.size() < count) std::this_thread::sleep_for(1ms);
    }
    Require(results.size() == count, "completion count/deadline");
    return results;
}

void Lifetimes() {
    CompletionGate gate;
    const auto old = Request(gate, Token(1));
    const auto newer = Request(gate, Token(1, 2));
    const auto palette = std::make_shared<const PaletteResult>(
        SampledPalette{0xffc44646u, 0xfff6f0deu});
    Require(!gate.Accept({old, palette}, Token(1, 2)), "appearance revision rejects old result");
    auto wrong = Token(1, 2);
    wrong.deviceGeneration = 2;
    Require(!gate.Accept({newer, palette}, wrong), "device generation rejects old result");
    wrong = Token(1, 2);
    ++wrong.targetLifetime;
    Require(!gate.Accept({newer, palette}, wrong), "target lifetime rejects reused target");
    wrong = Token(1, 2);
    ++wrong.processBirth;
    Require(!gate.Accept({newer, palette}, wrong), "process birth rejects restarted PID");
    Require(gate.Accept({newer, palette}, Token(1, 2)), "matching lifetime result accepted once");
    Require(!gate.Accept({newer, palette}, Token(1, 2)), "duplicate completion rejected");
    const auto closed = Request(gate, Token(1, 3));
    gate.Forget(1);
    Require(!gate.Accept({closed, palette}, Token(1, 3)), "closed/excluded target cannot resurrect");
    const auto cleared = Request(gate, Token(2));
    gate.Clear();
    Require(!gate.Accept({cleared, palette}, Token(2)), "mode change clear drops work");
    Require(!gate.Begin(Token(2), Input("")), "identity missing is not a PID fallback");
    CompletionGate tiny(1);
    Request(tiny, Token(1));
    Require(!tiny.Begin(Token(2), Input()), "gate target count bounded");
    std::cout << "PASS immutable completion tokens and closed/reused targets\n";
}

void Workers() {
    QueryBarrier barrier;
    PreparationLimits limits;
    limits.workerCount = 2;
    limits.jobDeadline = 3s;
    PreparationWorker worker([&](const auto& input, auto stop, auto deadline) {
        return barrier.Query(input, stop, deadline);
    }, limits);
    CompletionGate gate;
    const auto first = Request(gate, Token(1));
    Require(worker.Submit(first) == SubmitStatus::Accepted, "queue accepts first");
    barrier.Wait(1);
    Require(worker.Submit(first) == SubmitStatus::Deduplicated, "same request pending dedup");
    const auto second = Request(gate, Token(2));
    Require(worker.Submit(second) == SubmitStatus::Deduplicated, "same identity decoded once");
    const auto revised = Request(gate, Token(1, 2), Input("exe:Example", 2));
    Require(worker.Submit(revised) == SubmitStatus::Accepted, "new icon revision decoded separately");
    barrier.Wait(2);
    auto early = WaitCompletions(worker, 1);
    Require(early[0].request.Sequence() == revised.Sequence() &&
            Palette(*early[0].palette).baseArgb == 0xff6685b4u &&
            gate.Accept(early[0], Token(1, 2)), "out-of-order new result is authoritative");
    barrier.Open();
    auto late = WaitCompletions(worker, 1);
    Require(late[0].request.Token().targetId == 2 &&
            gate.Accept(late[0], Token(2)), "superseded waiter dropped, other target retained");
    Require(barrier.Started() == 2, "one query per immutable input");

    auto restart = Token(3);
    restart.processBirth = 999;
    const auto restarted = Request(gate, restart);
    Require(worker.Submit(restarted) == SubmitStatus::Cached, "same AppIdentity restart cache hit");
    auto reused = WaitCompletions(worker, 1);
    Require(Palette(*reused[0].palette).baseArgb == 0xffc44646u &&
            gate.Accept(reused[0], restart), "restart uses stable palette not PID");
    const auto held = reused[0].palette;
    worker.Stop();
    Require(Palette(*held).baseArgb == 0xffc44646u, "immutable result survives cache shutdown");
    Require(worker.Submit(restarted) == SubmitStatus::Stopped, "shutdown rejects requests");
    std::cout << "PASS delayed/out-of-order jobs, dedup, revision changes and identity restart\n";
}

void Pressure() {
    QueryBarrier barrier;
    PreparationLimits limits;
    limits.workerCount = 1;
    limits.pendingJobs = 2;
    limits.subscribers = 2;
    limits.cacheEntries = 1;
    limits.cacheBytes = 4096;
    limits.pendingBytes = 4096;
    limits.jobDeadline = 3s;
    PreparationWorker worker([&](const auto& input, auto stop, auto deadline) {
        return barrier.Query(input, stop, deadline);
    }, limits);
    CompletionGate gate;
    const auto first = Request(gate, Token(1), Input("exe:First"));
    const auto second = Request(gate, Token(2), Input("exe:Second"));
    const auto refused = Request(gate, Token(3), Input("exe:Third"));
    Require(worker.Submit(first) == SubmitStatus::Accepted, "first pressure job");
    barrier.Wait(1);
    Require(worker.Submit(second) == SubmitStatus::Accepted, "second pressure job");
    Require(worker.Submit(refused) == SubmitStatus::Full, "bounded pending/subscriber pressure");
    barrier.Open();
    auto done = WaitCompletions(worker, 2);
    const auto inUse = done[0].palette;
    const auto stats = worker.Statistics();
    Require(stats.cacheEntries == 1 && stats.cacheBytes <= limits.cacheBytes &&
            stats.pendingBytes == 0 && stats.subscribers == 0 && stats.pendingJobs == 0,
            "completion drains bytes and cache eviction bound");
    Require(Palette(*inUse).baseArgb == 0xffc44646u, "eviction leaves in-use palette valid");
    Require(barrier.Started() == 2, "cache pressure does not redraw/requeue all targets");
    Require(worker.Submit(refused) == SubmitStatus::Accepted, "one explicit targeted retry");
    WaitCompletions(worker, 1);
    Require(barrier.Started() == 3, "targeted retry is one query");
    worker.Stop();

    PreparationLimits bytes;
    bytes.pendingBytes = 64;
    PreparationWorker limited([](const auto&, auto, auto) -> PaletteResult {
        throw std::runtime_error("should not decode");
    }, bytes);
    Require(limited.Submit(first) == SubmitStatus::Full, "pending byte cap independent of count");
    Require(limited.Statistics().queries == 0, "oversize admission performs no query");
    limited.Stop();
    bytes = {};
    bytes.cacheBytes = 64;
    std::atomic<unsigned> calls{};
    PreparationWorker noCache([&](const auto&, auto, auto) -> PaletteResult {
        ++calls;
        return Fallback{FallbackReason::IconUnavailable, ERROR_ACCESS_DENIED};
    }, bytes);
    const auto negative = Request(gate, Token(4), Input("exe:NoIcon"));
    noCache.Submit(negative);
    auto missing = WaitCompletions(noCache, 1);
    Require(std::get<Fallback>(*missing[0].palette) ==
            Fallback{FallbackReason::IconUnavailable, ERROR_ACCESS_DENIED},
            "unavailable icons are explicit, not silent empty colors");
    Require(noCache.Statistics().cacheEntries == 0, "cache bytes independent of entry cap");
    noCache.Stop();
    std::cout << "PASS count/byte pressure, explicit fallbacks and no eviction feedback\n";
}

void Cancellation() {
    QueryBarrier barrier;
    PreparationLimits limits;
    limits.jobDeadline = 15ms;
    PreparationWorker worker([&](const auto& input, auto stop, auto deadline) {
        return barrier.Query(input, stop, deadline);
    }, limits);
    CompletionGate gate;
    const auto first = Request(gate, Token(1));
    worker.Submit(first);
    auto timeout = WaitCompletions(worker, 1);
    Failure(*timeout[0].palette, FallbackReason::DeadlineExceeded);
    worker.Stop();
    QueryBarrier pending;
    PreparationWorker closing([&](const auto& input, auto stop, auto deadline) {
        return pending.Query(input, stop, deadline);
    });
    closing.Submit(Request(gate, Token(2)));
    pending.Wait(1);
    closing.Forget(2);
    gate.Forget(2);
    closing.Stop();
    Require(closing.Drain().empty() && closing.Statistics().subscribers == 0,
            "cooperative shutdown drains ownership without callback");
    PreparationWorker throwing([](const auto&, auto, auto) -> PaletteResult {
        throw std::runtime_error("local decode failure");
    });
    throwing.Submit(Request(gate, Token(3)));
    Failure(*WaitCompletions(throwing, 1)[0].palette, FallbackReason::QueryFailed);
    throwing.Stop();
    limits.jobDeadline = 5ms;
    PreparationWorker late([](const auto&, auto, auto) -> PaletteResult {
        std::this_thread::sleep_for(20ms);
        return SampledPalette{0xffc04b4bu, 0xfff6f0deu};
    }, limits);
    late.Submit(Request(gate, Token(4)));
    Failure(*WaitCompletions(late, 1)[0].palette, FallbackReason::DeadlineExceeded);
    late.Stop();
    std::cout << "PASS cooperative timeout, cancellation and query exception\n";
}

HICON MemoryIcon(bool legacy) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 48;
    info.bmiHeader.biHeight = -48;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* data = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    Require(color && data, "owned fixture DIB");
    auto* bgra = static_cast<std::uint32_t*>(data);
    std::fill_n(bgra, IconPixelCount, legacy ? 0x00ff0000u : 0xd9ff0000u);
    std::array<std::uint8_t, 48 * 6> mask{};
    // For legacy icons the upper-left 8x8 block is transparent solely through AND bits.
    if (legacy) for (unsigned y = 0; y < 8; ++y) mask[y * 6] = 255;
    else bgra[0] = 0;
    const HBITMAP andMask = CreateBitmap(48, 48, 1, 1, mask.data());
    if (!andMask) {
        DeleteObject(color);
        throw std::runtime_error("owned fixture AND mask");
    }
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = andMask;
    const HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(color);
    DeleteObject(andMask);
    Require(icon != nullptr, "owned alpha/legacy icon");
    return icon;
}

class TempFile final {
public:
    TempFile() {
        std::array<wchar_t, MAX_PATH> dir{}, shortPath{}, longPath{};
        Require(GetTempPathW(static_cast<DWORD>(dir.size()), dir.data()) &&
                GetTempFileNameW(dir.data(), L"icn", 0, shortPath.data()) &&
                GetLongPathNameW(shortPath.data(), longPath.data(), static_cast<DWORD>(longPath.size())),
                "temp executable fixture path");
        path_ = longPath.data();
    }
    ~TempFile() { DeleteFileW(path_.c_str()); }
    const std::wstring& Path() const noexcept { return path_; }
    void Write(const std::vector<char>& bytes) const {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Require(file.good(), "write temp executable fixture");
    }
private:
    std::wstring path_;
};

constexpr unsigned char RedLogoPng[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,
    0x30,0x00,0x00,0x00,0x30,0x08,0x06,0x00,0x00,0x00,0x57,0x02,0xf9,0x87,0x00,0x00,0x00,0x45,0x49,
    0x44,0x41,0x54,0x78,0xda,0xed,0xcf,0x31,0x0d,0x00,0x00,0x08,0x03,0x30,0xa4,0xcc,0xbf,0x0a,0x9c,
    0x81,0x05,0x5e,0x92,0x1e,0x15,0xd0,0xea,0x64,0x3e,0x2b,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x81,0x6b,0x60,0x01,0x06,0x63,0xf8,0x00,0x4c,0xf2,0x04,0xf1,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
};
constexpr unsigned char BlueLogoPng[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,
    0x30,0x00,0x00,0x00,0x30,0x08,0x06,0x00,0x00,0x00,0x57,0x02,0xf9,0x87,0x00,0x00,0x00,0x45,0x49,
    0x44,0x41,0x54,0x78,0xda,0xed,0xcf,0x31,0x0d,0x00,0x00,0x08,0x03,0x30,0xa4,0xcc,0xbf,0x0a,0x9c,
    0x81,0x05,0x5e,0x92,0x1e,0x15,0xd0,0x4a,0x7a,0x3e,0x2b,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x81,0x6b,0x60,0x01,0xc5,0xaf,0xf8,0x00,0x42,0x02,0x77,0xf1,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
};

class TempDirectory final {
public:
    TempDirectory() {
        TempFile reserved;
        path_ = reserved.Path() + L".d";
        Require(CreateDirectoryW(path_.c_str(), nullptr) &&
                CreateDirectoryW((path_ + L"\\Assets").c_str(), nullptr), "temp package fixture directory");
    }
    ~TempDirectory() {
        for (const auto& file : written_) DeleteFileW(file.c_str());
        RemoveDirectoryW((path_ + L"\\Assets").c_str());
        RemoveDirectoryW(path_.c_str());
    }
    const std::wstring& Path() const noexcept { return path_; }
    void Write(const std::wstring& relative, std::string_view bytes) {
        const auto file = path_ + L"\\" + relative;
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Require(output.good(), "write temp package fixture");
        written_.push_back(file);
    }
private:
    std::wstring path_;
    std::vector<std::wstring> written_;
};

void PackageLogo() {
    const auto bytes = [](const unsigned char* data, std::size_t size) {
        return std::string_view(reinterpret_cast<const char*>(data), size);
    };
    TempDirectory package;
    package.Write(L"App.exe", "not a PE image");
    package.Write(L"Other.exe", "not a PE image");
    package.Write(L"AppxManifest.xml",
        "<Package><Applications>"
        "<Application Id=\"Other\" Executable=\"Other.exe\"><uap:VisualElements Square44x44Logo=\"Assets\\Other.png\"/></Application>"
        "<Application Id=\"App\" Executable=\"App.exe\"><uap:VisualElements Square44x44Logo=\"Assets\\Logo.png\"/></Application>"
        "</Applications></Package>");
    package.Write(L"Assets\\Logo.targetsize-48_altform-unplated.png", bytes(RedLogoPng, sizeof RedLogoPng));
    package.Write(L"Assets\\Logo.altform-lightunplated_targetsize-48.png", bytes(BlueLogoPng, sizeof BlueLogoPng));
    package.Write(L"Assets\\Logo.targetsize-48_altform-unplated_contrast-white.png", bytes(BlueLogoPng, sizeof BlueLogoPng));
    package.Write(L"Assets\\Logo.scale-200.png", bytes(BlueLogoPng, sizeof BlueLogoPng));
    package.Write(L"Assets\\Other.png", bytes(BlueLogoPng, sizeof BlueLogoPng));
    const auto deadline = [] { return std::chrono::steady_clock::now() + 10s; };
    const auto red = Palette(SamplePalette(Solid({192, 32, 32, 255})));
    Require(Palette(SampleExecutableIcon({"aumid:Fixture!App", 1, package.Path() + L"\\App.exe"}, {}, deadline())) == red,
            "packaged app without an exe icon samples its own unplated manifest logo");
    std::stop_source stop;
    stop.request_stop();
    Failure(SampleExecutableIcon({"aumid:Fixture!App", 1, package.Path() + L"\\App.exe"}, stop.get_token(), deadline()),
            FallbackReason::Cancelled);
    TempDirectory unpackaged;
    unpackaged.Write(L"App.exe", "not a PE image");
    Failure(SampleExecutableIcon({"exe:Fixture", 1, unpackaged.Path() + L"\\App.exe"}, {}, deadline()),
            FallbackReason::InvalidResource);
    std::cout << "PASS packaged manifest logo fallback prefers unplated art and rejects light/contrast variants\n";
}

void PaddedExecutable(const std::wstring& ownPath) {
    std::ifstream source(ownPath, std::ios::binary);
    std::vector<char> image((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    Require(image.size() > 1024 && image.size() < 8u * 1024 * 1024, "own executable is a small PE");
    const auto deadline = [] { return std::chrono::steady_clock::now() + 10s; };
    const auto sample = [&](const TempFile& file) {
        return SampleExecutableIcon({"exe:Padded", 1, file.Path()}, {}, deadline());
    };
    TempFile plain;
    plain.Write(image);
    Require(Palette(sample(plain)) == SampledPalette{0xffc04b4bu, 0xfff6f0deu}, "unpadded PE copy extraction");
    const auto baseline = ExecutableBytesRead();
    Require(baseline > 0 && baseline < 4u * 1024 * 1024, "extraction reads bounded ranges");

    auto padded = image;
    padded.resize(56u * 1024 * 1024, '\x5a');
    TempFile large;
    large.Write(padded);
    Require(Palette(sample(large)) == SampledPalette{0xffc04b4bu, 0xfff6f0deu}, "heavily padded PE extraction");
    Require(ExecutableBytesRead() == baseline, "bytes read do not scale with file size");

    padded.resize(64u * 1024 * 1024 + 1, '\x5a');
    large.Write(padded);
    Failure(sample(large), FallbackReason::SourceTooLarge);
    Require(ExecutableBytesRead() == 0, "oversize file rejected before any read");

    auto malformed = image;
    const std::int32_t beyond = 0x7ffffff0;
    std::memcpy(malformed.data() + offsetof(IMAGE_DOS_HEADER, e_lfanew), &beyond, sizeof beyond);
    TempFile bad;
    bad.Write(malformed);
    Failure(sample(bad), FallbackReason::InvalidResource);
    malformed = image;
    malformed.resize(1024);
    bad.Write(malformed);
    Failure(sample(bad), FallbackReason::InvalidResource);

    std::stop_source stop;
    stop.request_stop();
    Failure(SampleExecutableIcon({"exe:Padded", 1, large.Path()}, stop.get_token(), deadline()),
            FallbackReason::Cancelled);
}

void Native() {    Require(std::get<Fallback>(PixelsFromOwnedIcon(nullptr)).reason == FallbackReason::IconUnavailable,
            "null HICON explicit fallback");
    const HICON icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR));
    Require(icon != nullptr, "own compiled icon resource");
    auto result = PixelsFromOwnedIcon(icon);
    DestroyIcon(icon);
    const auto pixels = std::get_if<IconPixels>(&result);
    Require(pixels != nullptr, "WIC own icon conversion");
    Require(std::any_of(pixels->begin(), pixels->end(), [](const auto& p) { return p.a == 0; }) &&
            std::any_of(pixels->begin(), pixels->end(), [](const auto& p) { return p.a == 255; }),
            "WIC preserves transparent corner and opaque yarn");
    const auto nativePalette = Palette(SamplePalette(*pixels));
    Require(nativePalette.baseArgb == 0xffc04b4bu,
            "native icon outputs portable premultiplied red");
    for (const bool legacy : {false, true}) {
        const HICON memory = MemoryIcon(legacy);
        const auto copied = PixelsFromOwnedIcon(memory);
        DestroyIcon(memory);
        const auto bitmap = std::get_if<IconPixels>(&copied);
        Require(bitmap != nullptr, "owned legacy/alpha conversion");
        Require(Palette(SamplePalette(*bitmap)).baseArgb == 0xffc04b4bu,
                "native partial alpha or legacy mask must not darken yarn");
        const auto transparent = std::count_if(bitmap->begin(), bitmap->end(),
                                               [](const auto& p) { return p.a == 0; });
        Require(transparent == (legacy ? 64 : 1), "native alpha/AND mask transparency count");
        Require((*bitmap)[0].a == 0 && (*bitmap)[47 * 48 + 47].a == (legacy ? 255 : 217),
                "native transparency remains in the correct corner");
        if (!legacy) Require((*bitmap)[24 * 48 + 24] == PremultipliedRgba8{217, 0, 0, 217},
                             "native alpha must be multiplied exactly once");
    }
    std::array<std::uint8_t, 48 * 6> monochromeAnd{};
    std::array<std::uint8_t, 48 * 6> monochromeXor{};
    monochromeXor.fill(255);
    const HICON monochrome = CreateIcon(GetModuleHandleW(nullptr), 48, 48, 1, 1,
                                       monochromeAnd.data(), monochromeXor.data());
    Require(monochrome != nullptr, "owned monochrome icon");
    auto neutral = PixelsFromOwnedIcon(monochrome);
    DestroyIcon(monochrome);
    Require(std::holds_alternative<IconPixels>(neutral), "native monochrome conversion");
    Failure(SamplePalette(std::get<IconPixels>(neutral)), FallbackReason::InsufficientColorCoverage);
    std::array<wchar_t, 4096> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Require(length && length < path.size(), "own executable path bounded");
    const IconInput own{"exe:OwnFixture", 1, std::wstring(path.data(), length)};
    Require(Palette(SampleExecutableIcon(own, {}, std::chrono::steady_clock::now() + 3s)) ==
            SampledPalette{0xffc04b4bu, 0xfff6f0deu}, "bounded own executable PE icon extraction");
    auto missing = own;
    missing.executablePath += L".unavailable-icon-fixture";
    Failure(SampleExecutableIcon(missing, {}, std::chrono::steady_clock::now() + 1s),
            FallbackReason::IconUnavailable);
    auto unavailable = Input();
    unavailable.executablePath.clear();
    Failure(SampleExecutableIcon(unavailable, {}, std::chrono::steady_clock::now() + 1s),
            FallbackReason::IconUnavailable);
    unavailable.executablePath = L"\\\\server\\share\\app.exe";
    Failure(SampleExecutableIcon(unavailable, {}, std::chrono::steady_clock::now() + 1s),
            FallbackReason::UnsupportedSource);
    unavailable.executablePath = L"C:\\Fixture\\..\\app.exe";
    Failure(SampleExecutableIcon(unavailable, {}, std::chrono::steady_clock::now() + 1s),
            FallbackReason::UnsupportedSource);
    unavailable.executablePath = L"C:\\Fixture\\NUL.exe";
    Failure(SampleExecutableIcon(unavailable, {}, std::chrono::steady_clock::now() + 1s),
            FallbackReason::UnsupportedSource);
    Failure(SampleExecutableIcon(own, {}, std::chrono::steady_clock::now() - 1ms),
            FallbackReason::DeadlineExceeded);
    std::stop_source stop;
    stop.request_stop();
    Failure(SampleExecutableIcon(own, stop.get_token(), std::chrono::steady_clock::now() + 1s),
            FallbackReason::Cancelled);
    PaddedExecutable(own.executablePath);
    std::cout << "PASS native own-executable resource and WIC mask/alpha probe; no desktop apps\n";
}

}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--fixture-icon") {
            const IconInput fixture{"exe:KnownColorFixture", 0, argv[2]};
            Require(Palette(SampleExecutableIcon(fixture, {}, std::chrono::steady_clock::now() + 500ms)) ==
                SampledPalette{0xffc04b4bu, 0xfff6f0deu}, "ordinary fixture executable must contain the known colorful PE icon");
            std::cout << "PASS ordinary Fixture PE icon: base=0xffc04b4b contrast=0xfff6f0de; 500ms bound, no GUI\n";
            return 0;
        }
        if (argc != 1) {
            std::cerr << "Usage: IconYarnTests.exe [--fixture-icon <absolute Fixture.exe path>]\n";
            return 2;
        }
        Sampling();
        Motifs();
        Lifetimes();
        Workers();
        Pressure();
        Cancellation();
        Native();
        PackageLogo();
        std::cout << "PASS IconYarn contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
