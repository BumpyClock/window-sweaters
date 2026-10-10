#include "..\..\src\Applications\EligibilityPreferences.h"
#include "..\..\src\Applications\ProcessMetadata.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <objbase.h>
#include <stdexcept>
#include <type_traits>

using namespace window_sweaters::applications;
using namespace std::chrono_literals;

namespace {
int checks{};
void Check(bool value, const char* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
AppIdentity Path(std::wstring_view value) {
    auto result = AppIdentity::FromExecutablePath(value);
    Check(result.has_value(), "path fixture validates");
    return *result;
}
AppIdentity Package(std::wstring_view value = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App") {
    auto result = AppIdentity::FromPackagedAumid(value);
    Check(result.has_value(), "packaged fixture validates without installed app");
    return *result;
}
ProcessInstance Process(std::uint32_t pid = 20, std::uint64_t start = 100) {
    auto result = ProcessInstance::FromValues(pid, start);
    Check(result.has_value(), "process fixture validates");
    return *result;
}
TargetToken Token(std::uintptr_t hwnd = 10, std::uint32_t pid = 20, std::uint64_t start = 100,
                  std::uint64_t generation = 1, std::uint64_t revision = 0) {
    auto result = TargetToken::FromTarget({hwnd, pid, start, generation}, revision);
    Check(result.has_value(), "target fixture validates");
    return *result;
}
MetadataResult Metadata(std::wstring path = L"C:\\One\\Editor.exe") {
    return ResolveMetadata(NoPackagedIdentity{}, ExecutablePath{std::move(path)});
}
WindowLifetimeResult StableLifetime(const TargetToken& token) {
    return WindowOwnerLifetime{token.Process(), 42, 7};
}
std::wstring FrameHostPath() {
    std::array<wchar_t, MAX_PATH> directory{};
    const auto size = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    Check(size != 0 && size < directory.size(), "fixture system path available");
    return std::wstring(directory.data(), size) + L"\\ApplicationFrameHost.exe";
}
MetadataResult HostedMetadata(std::wstring_view aumid) {
    return ResolveWindowOwnerMetadata(PackagedAumid{std::wstring(aumid)}, Metadata(FrameHostPath()));
}
ImmutableMetadata Shared(MetadataResult result = Metadata()) {
    return std::make_shared<const MetadataResult>(std::move(result));
}
EligibilityPreferences Apply(const EligibilityPreferences& preferences, EligibilityMutation mutation) {
    auto result = preferences.Apply(mutation);
    Check(std::holds_alternative<EligibilityPreferences>(result), "valid preference mutation");
    return std::get<EligibilityPreferences>(std::move(result));
}
MetadataRequest Request(CompletionGate& gate, const TargetToken& token) {
    auto result = gate.Begin(token);
    Check(result.has_value(), "request registered");
    return *result;
}
template<class Predicate> void Wait(Predicate predicate, const char* label) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(label);
        std::this_thread::sleep_for(1ms);
    }
}
MetadataCompletion Complete(MetadataWorker& worker, const MetadataRequest& request) {
    Check(worker.Submit(request) == SubmitResult::Accepted, "worker accepts request");
    Wait([&] { return worker.Statistics().completed != 0; }, "metadata query timed out");
    auto results = worker.DrainCompletions();
    Check(results.size() == 1 && results.front().request == request, "completion retains immutable token");
    return std::move(results.front());
}

void IdentityTests() {
    static_assert(!std::is_default_constructible_v<AppIdentity>);
    static_assert(!std::is_default_constructible_v<TargetToken>);
    static_assert(!std::is_default_constructible_v<ProcessInstance>);
    const auto first = Path(L"c:/One/Editor.exe");
    const auto same = Path(L"\\\\?\\C:\\ONE\\editor.EXE");
    const auto different = Path(L"C:\\Two\\Editor.exe");
    Check(first == same, "ordinal-case/separator/extended DOS normalization");
    Check(first.Key() == L"C:\\One\\Editor.exe", "full normalized UTF16 key retained");
    Check(!(first < same) && !(same < first), "map comparison matches equality");
    Check(first != different, "same basename different directories remains separate");
    Check(Path(L"\\\\?\\UNC\\server\\share\\App.exe") ==
        Path(L"\\\\SERVER\\SHARE\\app.EXE"), "UNC path normalization");
    Check(Path(L"C:\\\u00c4\\app.exe") == Path(L"C:\\\u00e4\\APP.EXE"),
        "Windows ordinal Unicode case comparison, not locale collation");
    for (const auto* invalid : {L"Editor.exe", L"C:Editor.exe", L"\\Editor.exe",
        L"C:\\", L"C:\\One\\..\\Editor.exe", L"C:\\One\\.\\Editor.exe",
        L"C:\\One\\\\Editor.exe", L"C:\\One\\Editor.exe ", L"C:\\One\\Editor.exe.",
        L"C:\\One\\Editor.exe:stream", L"\\\\.\\pipe\\Editor.exe",
        L"\\\\server\\share", L"C:\\One\\Editor.exe\\", L"\\\\?\\Volume{1}\\Editor.exe",
        L"C:\\One\\con.exe", L"\\\\?\\C:\\One\\NUL.exe", L"C:\\COM1\\App.exe",
        L"C:\\One\\LPT\u00b9.exe"}) {
        Check(!AppIdentity::FromExecutablePath(invalid), "ambiguous/unsupported executable path rejected");
    }
    Check(!AppIdentity::FromExecutablePath(std::wstring(L"C:\\Bad\0Path\\App.exe", 19)),
        "embedded NUL rejected");
    Check(!AppIdentity::FromExecutablePath(std::wstring(L"C:\\") + wchar_t(0xD800) + L"\\App.exe"),
        "unpaired UTF16 surrogate rejected");
    Check(!AppIdentity::FromExecutablePath(L"C:\\" + std::wstring(32768, L'a')),
        "oversized path rejected before normalization");
    Check(!AppIdentity::FromPackagedAumid(L"Not an AUMID"), "unpackaged shell alias is not packaged identity");
    Check(!AppIdentity::FromPackagedAumid(L""), "empty packaged identity rejected");
    const auto packaged = Package();
    const auto traditional = Path(L"C:\\WindowsApps\\Calculator.exe");
    Check(packaged.Kind() == IdentityKind::PackagedAumid && packaged != traditional,
        "identity kind participates in persistence");
    const auto version1 = ResolveMetadata(PackagedAumid{packaged.Key()},
        ExecutablePath{L"C:\\WindowsApps\\Calculator_1.0_x64\\Calculator.exe"});
    const auto version2 = ResolveMetadata(PackagedAumid{packaged.Key()},
        ExecutablePath{L"C:\\WindowsApps\\Calculator_2.0_x64\\Calculator.exe"});
    Check(std::get<AppMetadata>(version1).Identity() == std::get<AppMetadata>(version2).Identity(),
        "package update path does not change AUMID identity");
    Check(Package(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App") !=
        Package(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!Other"),
        "package relative application identifier separates apps");
    Check(!ProcessInstance::FromValues(0, 100) && !ProcessInstance::FromValues(1, 0),
        "unknown process creation time cannot form cache key");
    Check(!TargetToken::FromTarget({0, 2, 3, 4}, 0) &&
        !TargetToken::FromTarget({1, 2, 0, 4}, 0) &&
        !TargetToken::FromTarget({1, 2, 3, 0}, 0), "invalid tracking lifetime cannot form token");
}

void MetadataTests() {
    const auto result = Metadata();
    const auto& value = std::get<AppMetadata>(result);
    Check(value.Identity() == Path(L"C:\\One\\Editor.exe"), "verified fullpath determines identity");
    Check(value.DisplayName() == L"Editor.exe" && value.ExecutableAlias() == L"Editor.exe",
        "display/alias are independently derived presentation metadata");
    Check(value.Provenance() == MetadataProvenance::ProcessImageName &&
        value.DisplayProvenance() == DisplayNameProvenance::ExecutableBasename,
        "executable provenance explicit");
    const MetadataUnavailable denied{UnavailableReason::AccessDenied, QueryStage::ApplicationId, ERROR_ACCESS_DENIED};
    Check(std::get<MetadataUnavailable>(ResolveMetadata(denied,
        ExecutablePath{L"C:\\One\\Editor.exe"})) == denied,
        "failed AUMID query does not silently downgrade to potentially versioned path");
    const MetadataUnavailable imageDenied{UnavailableReason::AccessDenied, QueryStage::ExecutablePath, ERROR_ACCESS_DENIED};
    Check(std::get<MetadataUnavailable>(ResolveMetadata(NoPackagedIdentity{}, imageDenied)) == imageDenied,
        "missing executable metadata remains unavailable");
    const auto packaged = ResolveMetadata(PackagedAumid{Package().Key()}, imageDenied);
    const auto& packageValue = std::get<AppMetadata>(packaged);
    Check(packageValue.DisplayName() == L"App" && !packageValue.ExecutableAlias() &&
        packageValue.DisplayProvenance() == DisplayNameProvenance::PackageRelativeApplicationId,
        "packaged identity usable without invented executable alias");
    Check(packageValue.Provenance() == MetadataProvenance::ProcessApplicationId,
        "packaged provenance explicit");
    Check(std::holds_alternative<MetadataUnavailable>(ResolveMetadata(PackagedAumid{L"bad"},
        ExecutablePath{L"C:\\One\\Editor.exe"})), "invalid packaged identity never falls back to path");
    Check(std::holds_alternative<MetadataUnavailable>(Metadata(L"Editor.exe")),
        "basename-only fact is not persistent metadata");
}

class FixtureOwnerSource final : public WindowOwnerSource {
public:
    explicit FixtureOwnerSource(const TargetToken& token)
        : before(StableLifetime(token)), after(before), root(Metadata(FrameHostPath())) {}
    WindowLifetimeResult before;
    WindowLifetimeResult after;
    PackagedIdentityQuery application{PackagedAumid{L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"}};
    MetadataResult root;
    unsigned lifetimeReads{};
    unsigned applicationReads{};
    unsigned processReads{};
    WindowLifetimeResult ReadLifetime(const TargetToken&) override {
        return lifetimeReads++ == 0 ? before : after;
    }
    PackagedIdentityQuery ReadApplicationId(const TargetToken&) override {
        ++applicationReads;
        return application;
    }
    MetadataResult ReadProcessMetadata(const ProcessInstance&) override {
        ++processReads;
        return root;
    }
};

void WindowOwnerTests() {
    const auto token = Token();
    FixtureOwnerSource first(token);
    const auto calculator = QueryWindowOwnerMetadata(token, first);
    const auto& one = std::get<AppMetadata>(calculator);
    Check(one.Identity() == Package() && one.Provenance() == MetadataProvenance::WindowApplicationId,
        "root window property has explicit supported per-window identity provenance");
    Check(one.DisplayName() == L"App" && !one.ExecutableAlias() &&
        one.DisplayProvenance() == DisplayNameProvenance::PackageRelativeApplicationId,
        "frame host executable is not fabricated hosted-app display or alias");
    Check(first.lifetimeReads == 2 && first.applicationReads == 1 && first.processReads == 1,
        "root lifetime validation brackets property and process lookup");

    FixtureOwnerSource second(token);
    second.application = PackagedAumid{L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!Other"};
    const auto other = QueryWindowOwnerMetadata(token, second);
    Check(std::get<AppMetadata>(other).Identity() != one.Identity(),
        "same host process can identify two unrelated window owners");
    const auto exactCase = HostedMetadata(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!app");
    Check(std::get<AppMetadata>(exactCase).Identity() != one.Identity(),
        "window packaged identity retains exact ordinal case instead of path case-folding");
    Check(std::get<AppMetadata>(HostedMetadata(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App")).Identity() ==
        one.Identity(), "window identity remains stable independently of lookup source");
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(
        PackagedAumid{one.Identity().Key()}, calculator)).reason == UnavailableReason::InvalidMetadata,
        "window-scoped fact cannot masquerade as root process provenance");

    const auto unavailable = ResolveWindowOwnerMetadata(NoPackagedIdentity{}, first.root);
    Check(std::get<MetadataUnavailable>(unavailable).reason == UnavailableReason::OwnerUnverified,
        "frame host without verified window AUMID never becomes system-host identity");
    Check(std::get<AppMetadata>(ResolveWindowOwnerMetadata(NoPackagedIdentity{},
        Metadata(L"C:\\Other\\ApplicationFrameHost.exe"))).Identity() ==
        Path(L"C:\\Other\\ApplicationFrameHost.exe"), "basename alone never classifies a process host");
    const MetadataUnavailable denied{UnavailableReason::AccessDenied, QueryStage::WindowApplicationId,
        static_cast<std::uint32_t>(E_ACCESSDENIED)};
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(denied, Metadata())) == denied,
        "property failure does not downgrade to root process executable");
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(PackagedAumid{L"bad"}, first.root)).reason ==
        UnavailableReason::OwnerUnverified, "non-package shell property on a frame host fails closed");
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(PackagedAumid{std::wstring(131, L'a')},
        first.root)).reason == UnavailableReason::OwnerUnverified, "oversized property identity is bounded");
    const auto browser = Metadata(L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe");
    const auto explicitAumid = ResolveWindowOwnerMetadata(PackagedAumid{L"MSEdge.udd.Default"}, browser);
    Check(std::get<AppMetadata>(explicitAumid).Identity() == std::get<AppMetadata>(browser).Identity() &&
        std::get<AppMetadata>(explicitAumid).Provenance() == std::get<AppMetadata>(browser).Provenance(),
        "unpackaged explicit window AUMID falls back to the verified root executable");
    const auto packagedRoot = ResolveMetadata(PackagedAumid{Package().Key()},
        ExecutablePath{L"C:\\Packages\\Calculator.exe"});
    const auto agreed = ResolveWindowOwnerMetadata(PackagedAumid{Package().Key()}, packagedRoot);
    Check(std::get<AppMetadata>(agreed).ExecutableAlias() == L"Calculator.exe",
        "matching packaged process retains genuinely known executable alias");
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(
        PackagedAumid{L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!Other"}, packagedRoot)).reason ==
        UnavailableReason::ConflictingOwner, "conflicting packaged process/window facts remain ambiguous");
    const MetadataUnavailable processDenied{UnavailableReason::AccessDenied, QueryStage::ApplicationId, ERROR_ACCESS_DENIED};
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(
        PackagedAumid{Package().Key()}, processDenied)) == processDenied,
        "failed process lookup does not assert a verified matching owner");
    Check(std::get<AppMetadata>(ResolveWindowOwnerMetadata(NoPackagedIdentity{}, packagedRoot)).Provenance() ==
        MetadataProvenance::ProcessApplicationId, "standalone packaged process still uses supported process fact");

    for (const auto& changed : {WindowOwnerLifetime{Process(20, 101), 42, 7},
        WindowOwnerLifetime{Process(21, 100), 42, 7}, WindowOwnerLifetime{token.Process(), 43, 7},
        WindowOwnerLifetime{token.Process(), 42, 8}}) {
        FixtureOwnerSource reused(token);
        reused.after = changed;
        Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(token, reused)).reason ==
            UnavailableReason::LifetimeMismatch, "PID/birth/thread/class changes during lookup cannot publish owner");
    }
    FixtureOwnerSource initiallyReused(token);
    initiallyReused.before = WindowOwnerLifetime{Process(21, 100), 42, 7};
    Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(token, initiallyReused)).reason ==
        UnavailableReason::LifetimeMismatch && initiallyReused.applicationReads == 0,
        "mismatching initial owner prevents metadata lookup");
    FixtureOwnerSource destroyed(token);
    destroyed.after = MetadataUnavailable{UnavailableReason::ProcessUnavailable, QueryStage::WindowLifetime};
    Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(token, destroyed)).reason ==
        UnavailableReason::ProcessUnavailable, "destroyed owner after lookup cannot return stale success");
    FixtureOwnerSource failedAndReused(token);
    failedAndReused.application = denied;
    failedAndReused.after = WindowOwnerLifetime{Process(21, 100), 42, 7};
    Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(token, failedAndReused)).reason ==
        UnavailableReason::LifetimeMismatch, "failed lookup also performs final lifetime validation");

    const auto unicodeRoot = Metadata(L"C:\\\u00c4pp\\\u7f16\u8f91.exe");
    const auto unicode = ResolveWindowOwnerMetadata(NoPackagedIdentity{}, unicodeRoot);
    Check(std::get<AppMetadata>(unicode).Identity() == Path(L"c:\\\u00e4pp\\\u7f16\u8f91.EXE") &&
        std::get<AppMetadata>(unicode).DisplayName() == L"\u7f16\u8f91.exe",
        "Unicode window-owner path identity and display retain ordinal normalization");
    const std::wstring invalidUnicode = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!" +
        std::wstring(1, wchar_t(0xD800));
    Check(std::get<MetadataUnavailable>(ResolveWindowOwnerMetadata(PackagedAumid{invalidUnicode}, first.root)).reason ==
        UnavailableReason::OwnerUnverified, "invalid UTF16 property never forms a persistent package key");

    const auto disabled = Apply(EligibilityPreferences{},
        {{}, false, {SetApplicationPreference{one.Identity(), false}}});
    const std::vector<RunningApplicationWindow> windows{
        {Token(1), Shared(calculator)}, {Token(2), Shared(other)}, {Token(3), Shared(calculator)},
        {Token(4), Shared(unavailable)}, {Token(1), Shared(calculator)}};
    const auto snapshot = std::get<ApplicationSnapshot>(BuildApplicationSnapshot(disabled, 0, windows));
    Check(snapshot.applications.size() == 2 && snapshot.applications[0].windowCount == 2 &&
        snapshot.applications[0].processCount == 1 && !snapshot.applications[0].enabled &&
        snapshot.applications[1].windowCount == 1 && snapshot.applications[1].processCount == 1 &&
        snapshot.applications[1].enabled, "hosted identities group windows independently with shared root process counts");
    Check(snapshot.unavailableOwners.size() == 1 && snapshot.unavailableOwners[0].windowCount == 1,
        "verified and unavailable window owners may coexist in one host process");
    const std::vector<RunningApplicationWindow> duplicateConflict{
        {Token(1), Shared(calculator)}, {Token(1), Shared(other)}};
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(disabled, 0, duplicateConflict)) ==
        SnapshotError::ConflictingMetadata, "distinct identities cannot conceal conflict on the same window lifetime");
}

void PreferenceTests() {
    EligibilityPreferences defaults;
    const auto first = Path(L"C:\\One\\Editor.exe");
    const auto second = Path(L"C:\\Two\\Editor.exe");
    Check(defaults.DefaultEnabled() && defaults.Allows(first), "initial default allows known future applications");
    Check(!defaults.Allows(MetadataUnavailable{UnavailableReason::AccessDenied, QueryStage::OpenProcess}),
        "inaccessible owner remains conservatively suppressed with no global fallback key");
    const auto excluded = Apply(defaults, {{}, false, {SetApplicationPreference{first, false}}});
    Check(!excluded.Allows(first) && excluded.Allows(second), "single-app mutation does not affect same-name app");
    Check(defaults.Allows(first) && defaults.Exceptions().empty(), "preference mutations are value-atomic");
    const auto allOff = Apply(excluded, {false, true, {}});
    Check(!allOff.DefaultEnabled() && allOff.Exceptions().empty(), "all-off replaces default and clears overrides atomically");
    Check(!allOff.Allows(second) && !allOff.Allows(Path(L"D:\\New\\Future.exe")),
        "all-off applies to apps first discovered later");
    const auto allowed = Apply(allOff, {{}, false, {SetApplicationPreference{first, true}}});
    Check(allowed.Allows(first) && !allowed.Allows(second), "explicit enable overrides all-off");
    const auto defaultOn = Apply(allowed, {true, false, {SetApplicationPreference{second, false}}});
    Check(defaultOn.Allows(first) && !defaultOn.Allows(second),
        "atomic default+exception mutation preserves explicit choices");
    const auto removed = Apply(defaultOn, {{}, false, {RemoveApplicationPreference{second}}});
    Check(removed.Allows(second), "remove exception returns app to default");
    auto invalid = removed.Apply({false, true, {SetApplicationPreference{first, false},
        SetApplicationPreference{Path(L"C:\\ONE\\EDITOR.EXE"), true}}});
    Check(std::get<PreferenceMutationError>(invalid) == PreferenceMutationError::DuplicateIdentityEdit &&
        removed.DefaultEnabled() && removed.Allows(first), "duplicate semantic mutation rejects entire transaction");

    const auto relaunch = Metadata();
    Check(!excluded.Allows(relaunch), "persistent exclusion survives new process metadata");
    CompletionGate gate;
    const auto old = Token(10, 20, 100, 1);
    const auto relaunched = Token(11, 21, 200, 2);
    Check(old.Target() != relaunched.Target() && old.Process() != relaunched.Process(),
        "relaunch has different ephemeral target/process identity");
    Check(!excluded.Allows(Metadata()), "relaunch retains persistent app exclusion independently of target token");
    Check(excluded.Allows(Metadata(L"C:\\Two\\Editor.exe")), "reused PID for another app cannot inherit exclusion");

    EligibilityMutation oversized;
    oversized.edits.reserve(EligibilityPreferences::MaximumExceptions + 1);
    for (std::size_t i = 0; i <= EligibilityPreferences::MaximumExceptions; ++i)
        oversized.edits.push_back(SetApplicationPreference{first, true});
    Check(std::get<PreferenceMutationError>(defaults.Apply(oversized)) == PreferenceMutationError::TooManyExceptions,
        "exception mutations are bounded");
}

void SnapshotTests() {
    const auto runningIdentity = Path(L"C:\\One\\Editor.exe");
    const auto savedIdentity = Path(L"D:\\NotRunning\\Other.exe");
    const auto prefs = Apply(EligibilityPreferences{},
        {{}, false, {SetApplicationPreference{runningIdentity, false}, SetApplicationPreference{savedIdentity, false}}});
    const auto one = Shared();
    const auto failure = Shared(MetadataUnavailable{UnavailableReason::AccessDenied, QueryStage::OpenProcess});
    std::vector<RunningApplicationWindow> windows{
        {Token(1, 20, 100, 1, 5), one}, {Token(2, 20, 100, 2, 5), one},
        {Token(3, 21, 200, 3, 5), one}, {Token(4, 30, 300, 4, 5), failure},
        {Token(5, 30, 300, 5, 5), failure}, {Token(1, 20, 100, 1, 5), one}};
    auto result = BuildApplicationSnapshot(prefs, 5, windows);
    Check(std::holds_alternative<ApplicationSnapshot>(result), "supported owners join into app snapshot");
    const auto& snapshot = std::get<ApplicationSnapshot>(result);
    Check(snapshot.configurationRevision == 5 && snapshot.applications.size() == 2,
        "running owners and saved nonrunning exceptions joined");
    const auto& row = snapshot.applications[0];
    Check(row.identity == runningIdentity && row.presence == ApplicationPresence::Running &&
        !row.enabled && row.explicitPreference && row.windowCount == 3 && row.processCount == 2 &&
        row.metadata, "disabled running owner stays visible; multiwindow/lifetime duplicates grouped correctly");
    const auto& saved = snapshot.applications[1];
    Check(saved.identity == savedIdentity && saved.presence == ApplicationPresence::SavedNotRunning &&
        saved.windowCount == 0 && saved.processCount == 0 && !saved.metadata,
        "saved nonrunning exception never gets fabricated live metadata");
    Check(snapshot.unavailableOwners.size() == 1 && snapshot.unavailableOwners[0].windowCount == 2,
        "inaccessible owners represented without persistent guessed binding");
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(prefs, 6, windows)) == SnapshotError::StaleRevision,
        "stale app snapshot revision rejected");
    windows.push_back({Token(1, 99, 900, 6, 5), one});
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(prefs, 5, windows)) ==
        SnapshotError::ConflictingWindowLifetime, "HWND reuse cannot mix lifetimes in one snapshot");
    const std::vector<RunningApplicationWindow> missing{{Token(1), {}}};
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(prefs, 0, missing)) == SnapshotError::MissingMetadata,
        "unresolved work is not authoritative unavailable metadata");
    const std::vector<RunningApplicationWindow> contradictory{
        {Token(1), one}, {Token(2), Shared(Metadata(L"C:\\Two\\Editor.exe"))}};
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(prefs, 0, contradictory)) ==
        SnapshotError::ConflictingMetadata, "contradictory identities for one process fail closed");
    const std::vector<RunningApplicationWindow> uncertain{{Token(1), one}, {Token(1), failure}};
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(prefs, 0, uncertain)) ==
        SnapshotError::ConflictingMetadata, "duplicate window cannot conceal conflicting availability");
    const std::vector<RunningApplicationWindow> empty;
    const auto nonrunning = std::get<ApplicationSnapshot>(BuildApplicationSnapshot(prefs, 5, empty));
    Check(nonrunning.applications.size() == 2 &&
        nonrunning.applications[0].presence == ApplicationPresence::SavedNotRunning,
        "all saved choices remain available after application restart/close");
}

void CompletionTests() {
    CompletionGate gate(7, 2);
    const auto original = Token(10, 20, 100, 1, 7);
    const auto old = Request(gate, original);
    const auto latest = Request(gate, original);
    Check(!gate.Retire(old) && gate.Size() == 1,
        "request-specific cancellation cannot retire a newer same-window request");
    Check(!gate.Accept({old, Shared()}, original), "out-of-order old completion rejected even with same target/revision");
    Check(gate.Accept({latest, Shared()}, original) && !gate.Accept({latest, Shared()}, original),
        "latest completion accepted at most once");
    const auto pending = Request(gate, original);
    gate.Forget(10);
    Check(!gate.Accept({pending, Shared()}, std::nullopt), "completion after destruction cannot resurrect target");
    const auto replacement = Request(gate, Token(10, 20, 200, 2, 7));
    Check(!gate.Accept({pending, Shared()}, replacement.Token()), "PID+HWND reuse rejects previous process creation/generation");
    Check(gate.Accept({replacement, Shared()}, replacement.Token()), "replacement lifetime accepted independently");
    const auto beforeRevision = Request(gate, original);
    gate.SetConfigurationRevision(8);
    Check(!gate.Accept({beforeRevision, Shared()}, original) && !gate.Begin(original),
        "configuration mutation invalidates old completion and submissions");
    const auto current = Request(gate, Token(10, 20, 100, 1, 8));
    Check(!gate.Accept({current, {}}, current.Token()), "null completion does not consume pending request");
    Check(!gate.Accept({current, Shared()}, Token(10, 20, 100, 2, 8)),
        "owner-tracker current lifetime required even if Forget was missed");
    Check(!gate.Accept({current, Shared()}, std::nullopt), "absent current target cannot accept pending metadata");
    Check(gate.Accept({current, Shared()}, current.Token()), "current revision can resolve metadata after stale completion");
    Request(gate, Token(1, 20, 100, 1, 8));
    Request(gate, Token(2, 20, 100, 2, 8));
    Check(!gate.Begin(Token(3, 20, 100, 3, 8)) && gate.Size() == 2, "pending lifetime gate is bounded");
}

void CacheTests() {
    std::atomic<unsigned> queries{};
    WorkerLimits limits;
    limits.cacheEntries = 2;
    CompletionGate gate;
    MetadataWorker worker(limits, [&](const TargetToken& token) {
        ++queries;
        return Metadata(token.Process().CreationTime() == 200 ? L"C:\\Two\\Editor.exe" : L"C:\\One\\Editor.exe");
    }, StableLifetime);
    const auto first = Complete(worker, Request(gate, Token(1)));
    const auto second = Complete(worker, Request(gate, Token(1)));
    Check(first.metadata == second.metadata && queries == 1 && worker.Statistics().cacheHits == 1,
        "same verified target lifetime shares immutable metadata cache");
    const auto oneEntryBytes = worker.Statistics().cacheBytes;
    const auto recycled = Complete(worker, Request(gate, Token(1, 20, 200, 2)));
    Check(queries == 2 && std::get<AppMetadata>(*recycled.metadata).Identity() == Path(L"C:\\Two\\Editor.exe"),
        "PID reuse forces metadata lookup for different creation time");
    Complete(worker, Request(gate, Token(3, 21, 300, 3)));
    Check(worker.Statistics().cached == 2 && worker.Statistics().cacheBytes <= limits.cacheBytes,
        "LRU entry/byte limits held");
    Check(std::get<AppMetadata>(*first.metadata).Identity() == Path(L"C:\\One\\Editor.exe"),
        "cache eviction does not invalidate outstanding immutable metadata");
    Complete(worker, Request(gate, Token(4)));
    Check(queries == 4, "least recently used target lifetime was evicted");
    Check(worker.Stop().empty(), "stop with drained completions has no unreported requests");
    const auto afterStop = Request(gate, Token(9));
    Check(worker.Statistics().cached == 0 && worker.Statistics().cacheBytes == 0 &&
        worker.Submit(afterStop) == SubmitResult::Stopped && gate.Retire(afterStop),
        "stopped submission is explicitly rejected and its exact gate request retired");
    Check(worker.Stop().empty(), "repeated stop returns no duplicate cancellations");

    WorkerLimits tiny;
    tiny.cacheBytes = 1;
    MetadataWorker noCache(tiny, [](const TargetToken&) { return Metadata(); }, StableLifetime);
    Complete(noCache, Request(gate, Token(11)));
    Complete(noCache, Request(gate, Token(12)));
    Check(noCache.Statistics().cached == 0 && noCache.Statistics().queries == 2,
        "single oversized metadata entry is not cached");

    WorkerLimits byteLimited;
    byteLimited.cacheBytes = oneEntryBytes;
    MetadataWorker exactBudget(byteLimited, [](const TargetToken&) { return Metadata(); }, StableLifetime);
    Complete(exactBudget, Request(gate, Token(21)));
    Complete(exactBudget, Request(gate, Token(22, 22, 200)));
    Check(exactBudget.Statistics().cached == 1 && exactBudget.Statistics().cacheBytes == oneEntryBytes,
        "byte budget evicts even when cache entry-count limit has space");
    Complete(exactBudget, Request(gate, Token(23)));
    Check(exactBudget.Statistics().queries == 3, "byte-evicted target must resolve again");

    WorkerLimits expires;
    expires.cacheLifetime = 1ms;
    MetadataWorker expiring(expires, [](const TargetToken&) { return Metadata(); }, StableLifetime);
    Complete(expiring, Request(gate, Token(13)));
    std::this_thread::sleep_for(5ms);
    Complete(expiring, Request(gate, Token(13)));
    Check(expiring.Statistics().queries == 2, "cache TTL permits refresh without lifetime guessing");

    MetadataWorker unavailable({}, [](const TargetToken&) -> MetadataResult {
        return MetadataUnavailable{UnavailableReason::AccessDenied, QueryStage::OpenProcess};
    }, StableLifetime);
    Complete(unavailable, Request(gate, Token(15)));
    Complete(unavailable, Request(gate, Token(16)));
    Check(unavailable.Statistics().cached == 0 && unavailable.Statistics().unavailableQueries == 2,
        "transient unavailable metadata is not cached as authoritative identity");
    MetadataWorker throwing({}, [](const TargetToken&) -> MetadataResult { throw std::runtime_error("injected"); });
    const auto failed = Complete(throwing, Request(gate, Token(17)));
    Check(std::get<MetadataUnavailable>(*failed.metadata).reason == UnavailableReason::QueryException,
        "backend exception produces explicit failure rather than worker escape");
    bool rejected = false;
    try {
        WorkerLimits bad;
        bad.queuedRequests = 0;
        MetadataWorker invalid(bad);
    } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "invalid worker budgets rejected before starting thread");
}

void OwnerWorkerTests() {
    const auto root = Metadata(FrameHostPath());
    const std::wstring firstId = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";
    const std::wstring secondId = L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!Other";
    std::atomic<unsigned> queries{};
    std::atomic<bool> mta{true};
    std::atomic<unsigned> lifetimeMode{};
    std::atomic<unsigned> lifetimeReads{};
    WorkerLimits limits;
    limits.cacheEntries = 2;
    CompletionGate gate;
    MetadataWorker worker(limits, [&](const TargetToken& token) {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (FAILED(CoGetApartmentType(&type, &qualifier)) || type != APTTYPE_MTA) mta = false;
        ++queries;
        return ResolveWindowOwnerMetadata(PackagedAumid{token.Target().hwnd == 1 ? firstId : secondId}, root);
    }, [&](const TargetToken& token) -> WindowLifetimeResult {
        ++lifetimeReads;
        if (lifetimeMode == 1)
            return MetadataUnavailable{UnavailableReason::LifetimeMismatch, QueryStage::WindowLifetime};
        if (lifetimeMode == 2 && lifetimeReads % 2 == 0)
            return WindowOwnerLifetime{token.Process(), 43, 7};
        if (lifetimeMode == 3) return WindowOwnerLifetime{token.Process(), 43, 7};
        if (lifetimeMode == 4) throw std::runtime_error("injected lifetime failure");
        return StableLifetime(token);
    });
    const auto first = Complete(worker, Request(gate, Token(1)));
    Check(gate.Accept(first, first.request.Token()), "first verified owner completion accepted");
    const auto second = Complete(worker, Request(gate, Token(2)));
    Check(gate.Accept(second, second.request.Token()) && queries == 2 && worker.Statistics().cacheHits == 0 &&
        std::get<AppMetadata>(*first.metadata).Identity() == Package(firstId) &&
        std::get<AppMetadata>(*second.metadata).Identity() == Package(secondId),
        "worker queries different windows in one host process independently");
    const auto cached = Complete(worker, Request(gate, Token(1)));
    Check(gate.Accept(cached, cached.request.Token()) && cached.metadata == first.metadata && queries == 2 &&
        worker.Statistics().cacheHits == 1 && lifetimeReads == 6,
        "cache hit repeats before/after root lifetime validation on MTA worker");
    Check(mta, "all application-property work executes on the one initialized MTA");
    for (unsigned mode = 1; mode <= 4; ++mode) {
        lifetimeMode = mode;
        lifetimeReads = 0;
        const auto invalid = Complete(worker, Request(gate, Token(1)));
        Check(gate.Accept(invalid, invalid.request.Token()) &&
            std::holds_alternative<MetadataUnavailable>(*invalid.metadata) && queries == 2 &&
            worker.Statistics().cacheHits == 1, "failed/changed lifetime cannot publish a cached owner identity");
        Check(worker.Statistics().cached <= limits.cacheEntries &&
            worker.Statistics().cacheBytes <= limits.cacheBytes, "cache validation failure preserves count/byte bounds");
    }
    lifetimeMode = 0;
    const auto generation = Complete(worker, Request(gate, Token(1, 20, 100, 2)));
    Check(gate.Accept(generation, generation.request.Token()) && queries == 3,
        "recycled HWND tracker generation never shares previous window-owner cache");
    const auto recycledPid = Complete(worker, Request(gate, Token(1, 20, 200, 3)));
    Check(gate.Accept(recycledPid, recycledPid.request.Token()) && queries == 4,
        "process birth change never shares previous window-owner cache");
    gate.SetConfigurationRevision(1);
    const auto revision = Complete(worker, Request(gate, Token(1, 20, 200, 3, 1)));
    Check(gate.Accept(revision, revision.request.Token()) && queries == 5,
        "configuration revision cannot reuse old target authority");
    Check(worker.Stop().empty() && worker.Statistics().cacheBytes == 0, "owner worker shutdown frees validated cache");

    MetadataWorker unvalidated(limits, [](const TargetToken&) { return Metadata(); });
    gate.SetConfigurationRevision(0);
    Complete(unvalidated, Request(gate, Token(9)));
    Complete(unvalidated, Request(gate, Token(9)));
    Check(unvalidated.Statistics().queries == 2 && unvalidated.Statistics().cached == 0,
        "injected query without lifetime validator cannot create owner cache authority");

    WorkerLimits pressureLimits;
    pressureLimits.completedRequests = 1;
    pressureLimits.queuedRequests = 1;
    pressureLimits.cacheEntries = 2;
    CompletionGate pressureGate(0, 2);
    MetadataWorker pressure(pressureLimits, [&](const TargetToken& token) {
        return ResolveWindowOwnerMetadata(PackagedAumid{token.Target().hwnd == 1 ? firstId : secondId}, root);
    }, StableLifetime);
    const auto pendingFirst = Request(pressureGate, Token(1));
    Check(pressure.Submit(pendingFirst) == SubmitResult::Accepted, "hosted pressure first window accepted");
    Wait([&] { return pressure.Statistics().completed == 1; }, "hosted first completion timed out");
    const auto pendingSecond = Request(pressureGate, Token(2));
    Check(pressure.Submit(pendingSecond) == SubmitResult::Accepted &&
        pressure.Statistics().completionBackpressure && pressure.Statistics().queries == 1 &&
        pressure.Statistics().coalesced == 0, "host process sharing never coalesces separate window-owner requests");
    const auto deliveredFirst = pressure.DrainCompletions();
    Check(deliveredFirst.size() == 1 && pressureGate.Accept(deliveredFirst[0], pendingFirst.Token()),
        "completion pressure delivers first hosted owner exactly");
    Wait([&] { return pressure.Statistics().completed == 1; }, "hosted second completion timed out");
    const auto deliveredSecond = pressure.DrainCompletions();
    Check(deliveredSecond.size() == 1 && pressureGate.Accept(deliveredSecond[0], pendingSecond.Token()) &&
        pressureGate.Size() == 0 && pressure.Statistics().queries == 2,
        "completion pressure delivers second distinct hosted owner and recovers exact gate capacity");
    const std::vector<RunningApplicationWindow> windows{
        {pendingFirst.Token(), deliveredFirst[0].metadata}, {pendingSecond.Token(), deliveredSecond[0].metadata}};
    const auto snapshot = std::get<ApplicationSnapshot>(BuildApplicationSnapshot(EligibilityPreferences{}, 0, windows));
    Check(snapshot.applications.size() == 2 && snapshot.applications[0].windowCount == 1 &&
        snapshot.applications[1].windowCount == 1 && snapshot.applications[0].processCount == 1 &&
        snapshot.applications[1].processCount == 1, "worker completions build distinct hosted application snapshot rows");
    std::vector<RunningApplicationWindow> tooMany(4097, windows.front());
    Check(std::get<SnapshotError>(BuildApplicationSnapshot(EligibilityPreferences{}, 0, tooMany)) ==
        SnapshotError::TooManyWindows, "window-owner snapshot count remains bounded before grouping duplicates");
}

void QueueTests() {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool released = false;
    WorkerLimits limits;
    limits.queuedRequests = 1;
    limits.completedRequests = 1;
    limits.cacheEntries = 0;
    CompletionGate gate(0, 3);
    MetadataWorker worker(limits, [&](const TargetToken&) {
        std::unique_lock lock(mutex);
        entered = true;
        wake.notify_all();
        if (!wake.wait_for(lock, 5s, [&] { return released; })) throw std::runtime_error("query barrier timed out");
        return Metadata();
    });
    const auto inFlight = Request(gate, Token(1));
    Check(worker.Submit(inFlight) == SubmitResult::Accepted, "barrier request submitted");
    {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock, 5s, [&] { return entered; }), "worker reached deterministic query barrier");
    }
    const auto pending = Request(gate, Token(2));
    Check(worker.Submit(pending) == SubmitResult::Accepted, "one bounded pending slot available");
    const auto newest = Request(gate, Token(2, 20, 100, 2));
    Check(worker.Submit(newest) == SubmitResult::Coalesced, "queued obsolete lifetime replaced not appended");
    Check(worker.Submit(pending) == SubmitResult::Obsolete &&
        worker.Submit(inFlight) == SubmitResult::Obsolete, "older/duplicate request never replaces newer work");
    const auto rejected = Request(gate, Token(3));
    Check(worker.Submit(rejected) == SubmitResult::Full && gate.Retire(rejected) && gate.Size() == 2,
        "full queue rejects request explicitly and request-specific retirement frees its gate slot");
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    wake.notify_all();
    Wait([&] { return worker.Statistics().completed == 1 && !worker.Statistics().inFlight; },
        "first bounded query timed out");
    const auto stats = worker.Statistics();
    const auto results = worker.DrainCompletions();
    Check(stats.completed == 1 && stats.queued == 1 && stats.completionBackpressure &&
        stats.queries == 1 && stats.coalesced == 1 && results.size() == 1 && results[0].request == inFlight,
        "completion pressure must deliver first accepted current request without loss");
    Check(gate.Accept(results[0], inFlight.Token()), "first accepted request retires its gate entry");
    Wait([&] { return worker.Statistics().completed == 1; }, "second bounded query timed out");
    const auto remaining = worker.DrainCompletions();
    Check(remaining.size() == 1 && remaining[0].request == newest &&
        gate.Accept(remaining[0], newest.Token()) && gate.Size() == 0,
        "every accepted current request delivered and all gate capacity recovered under pressure");
    const auto recovered = Request(gate, Token(3));
    const auto recoveredCompletion = Complete(worker, recovered);
    Check(gate.Accept(recoveredCompletion, recovered.Token()) && gate.Size() == 0,
        "previously capacity-bound gate accepts new target after pressure drains");
    for (unsigned round = 0; round < 32; ++round) {
        const auto first = Request(gate, Token(100 + 2 * round));
        Check(worker.Submit(first) == SubmitResult::Accepted, "pressure round accepts first current request");
        Wait([&] { return worker.Statistics().completed == 1; }, "pressure round first completion timed out");
        const auto second = Request(gate, Token(101 + 2 * round));
        Check(worker.Submit(second) == SubmitResult::Accepted, "pressure round accepts second current request");
        const auto pressure = worker.Statistics();
        Check(pressure.queued == 1 && pressure.completed == 1 && pressure.cached == 0 &&
            pressure.cacheBytes == 0 && pressure.completionBackpressure,
            "repeated completion pressure preserves queue/cache count and byte bounds");
        const auto firstResult = worker.DrainCompletions();
        Check(firstResult.size() == 1 && firstResult[0].request == first &&
            gate.Accept(firstResult[0], first.Token()), "pressure round delivers first request exactly");
        Wait([&] { return worker.Statistics().completed == 1; }, "pressure round second completion timed out");
        const auto secondResult = worker.DrainCompletions();
        Check(secondResult.size() == 1 && secondResult[0].request == second &&
            gate.Accept(secondResult[0], second.Token()) && gate.Size() == 0,
            "all accepted current requests recover gate capacity on every pressure round");
    }
}

void ShutdownTests() {
    WorkerLimits limits;
    limits.queuedRequests = 1;
    limits.completedRequests = 1;
    limits.cacheEntries = 0;
    CompletionGate gate(0, 2);
    MetadataWorker worker(limits, [](const TargetToken&) { return Metadata(); });
    const auto completed = Request(gate, Token(1));
    Check(worker.Submit(completed) == SubmitResult::Accepted, "shutdown fixture accepts first request");
    Wait([&] { return worker.Statistics().completed == 1; }, "shutdown completion timed out");
    const auto queued = Request(gate, Token(2));
    Check(worker.Submit(queued) == SubmitResult::Accepted, "shutdown fixture accepts queued request");
    Check(worker.Statistics().completionBackpressure && gate.Size() == 2,
        "shutdown begins with full completion queue and full gate");
    const auto cancelled = worker.Stop();
    Check(cancelled.size() == 2 && worker.DrainCompletions().empty() &&
        worker.Statistics().queued == 0 && !worker.Statistics().inFlight,
        "shutdown returns bounded exact cancellations for completed and queued requests");
    for (const auto& request : cancelled)
        Check(gate.Retire(request), "each accepted current shutdown request is explicitly retired");
    Check(gate.Size() == 0 && worker.Stop().empty(),
        "shutdown recovers all gate capacity without duplicate cancellation");
    MetadataWorker replacement(limits, [](const TargetToken&) { return Metadata(); });
    const auto retry = Request(gate, Token(3));
    const auto retried = Complete(replacement, retry);
    Check(gate.Accept(retried, retry.Token()) && gate.Size() == 0,
        "same gate can resolve new target after worker shutdown/replacement");

    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool released = false;
    MetadataWorker inFlight(limits, [&](const TargetToken&) {
        std::unique_lock lock(mutex);
        entered = true;
        wake.notify_all();
        if (!wake.wait_for(lock, 5s, [&] { return released; })) throw std::runtime_error("stop query barrier timed out");
        return Metadata();
    });
    const auto active = Request(gate, Token(4));
    Check(inFlight.Submit(active) == SubmitResult::Accepted, "shutdown in-flight request accepted");
    {
        std::unique_lock lock(mutex);
        Check(wake.wait_for(lock, 5s, [&] { return entered; }), "shutdown reaches query barrier");
    }
    const auto pending = Request(gate, Token(5));
    Check(inFlight.Submit(pending) == SubmitResult::Accepted, "shutdown pending request accepted");
    std::vector<MetadataRequest> stopped;
    std::jthread stopper([&] { stopped = inFlight.Stop(); });
    Wait([&] { return inFlight.Statistics().queued == 0; }, "shutdown cancellation capture timed out");
    {
        std::lock_guard lock(mutex);
        released = true;
    }
    wake.notify_all();
    stopper.join();
    Check(stopped.size() == 2 && inFlight.DrainCompletions().empty(),
        "in-flight query result cannot publish false success after shutdown");
    for (const auto& request : stopped)
        Check(gate.Retire(request), "in-flight/queued cancellations retire matching current requests");
    Check(gate.Size() == 0, "shutdown during query recovers gate capacity");
}

void NativeTests() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    Check(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE,
        "own-process creation time available");
    const auto start = (std::uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
    const auto process = Process(GetCurrentProcessId(), start);
    const auto handleResult = QueryProcessMetadata(GetCurrentProcess(), process);
    const auto pidResult = QueryProcessMetadata(process);
    Check(std::holds_alternative<AppMetadata>(handleResult) &&
        std::holds_alternative<AppMetadata>(pidResult), "own process resolves using public limited-information APIs");
    Check(std::get<AppMetadata>(handleResult).Identity() == std::get<AppMetadata>(pidResult).Identity(),
        "borrowed-handle and owned-handle query agree");
    std::vector<wchar_t> expected(32768);
    DWORD size = static_cast<DWORD>(expected.size());
    Check(QueryFullProcessImageNameW(GetCurrentProcess(), 0, expected.data(), &size) != FALSE,
        "own-process image retrieved for exact native assertion");
    Check(std::get<AppMetadata>(pidResult).Identity() == Path(std::wstring_view(expected.data(), size)),
        "native unpackaged identity equals own normalized executable path");
    Check(std::get<MetadataUnavailable>(QueryProcessMetadata(nullptr, process)).reason ==
        UnavailableReason::ProcessUnavailable, "invalid borrowed handle returns explicit unavailable");
    Check(std::get<MetadataUnavailable>(QueryProcessMetadata(GetCurrentProcess(),
        Process(GetCurrentProcessId(), start + 1))).reason == UnavailableReason::LifetimeMismatch,
        "native process creation-time mismatch cannot bind recycled PID");
    std::wstring command = L"\"" + std::wstring(expected.data(), size) + L"\" --owner-lifetime-fixture";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child) != FALSE, "own lifetime fixture process starts without a visible console");
    struct FixtureProcess final {
        PROCESS_INFORMATION value;
        ~FixtureProcess() { CloseHandle(value.hThread); CloseHandle(value.hProcess); }
    } childScope{child};
    Check(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0,
        "own lifetime fixture exits before process validation");
    FILETIME childCreation{};
    Check(GetProcessTimes(child.hProcess, &childCreation, &exit, &kernel, &user) != FALSE,
        "held exited fixture process retains creation-time facts");
    const auto childStart = (std::uint64_t{childCreation.dwHighDateTime} << 32) | childCreation.dwLowDateTime;
    Check(std::get<MetadataUnavailable>(QueryProcessMetadata(child.hProcess,
        Process(child.dwProcessId, childStart))).reason == UnavailableReason::ProcessUnavailable,
        "exited process with correct PID and birth cannot publish metadata");
    struct HiddenFixture final {
        HINSTANCE instance{GetModuleHandleW(nullptr)};
        ATOM atom{};
        HWND hwnd{};
        HiddenFixture() {
            WNDCLASSW type{};
            type.hInstance = instance;
            type.lpfnWndProc = DefWindowProcW;
            type.lpszClassName = L"WindowSweaters.Applications.HiddenOwnerTest";
            atom = RegisterClassW(&type);
            Check(atom != 0, "own hidden fixture class registered");
            hwnd = CreateWindowExW(0, MAKEINTATOM(atom), L"", WS_OVERLAPPEDWINDOW,
                0, 0, 100, 100, nullptr, nullptr, instance, nullptr);
            Check(hwnd != nullptr && !IsWindowVisible(hwnd), "own top-level fixture created without visibility");
        }
        ~HiddenFixture() {
            if (hwnd) DestroyWindow(hwnd);
            if (atom) UnregisterClassW(MAKEINTATOM(atom), instance);
        }
    } fixture;
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(SUCCEEDED(apartment), "native owner probe initializes its own MTA");
    struct Apartment final { ~Apartment() { CoUninitialize(); } } apartmentScope;
    const auto token = Token(reinterpret_cast<std::uintptr_t>(fixture.hwnd), process.Pid(), start);
    const auto owner = QueryWindowOwnerMetadata(token);
    const auto borrowedOwner = QueryWindowOwnerMetadata(token, GetCurrentProcess());
    Check(std::holds_alternative<AppMetadata>(owner) &&
        std::get<AppMetadata>(owner).Identity() == std::get<AppMetadata>(pidResult).Identity(),
        "hidden standalone root resolves exact own-process identity through window adapter");
    Check(std::holds_alternative<AppMetadata>(borrowedOwner) &&
        std::get<AppMetadata>(borrowedOwner).Identity() == std::get<AppMetadata>(owner).Identity(),
        "borrowed and opened process handles agree for own hidden root");
    Check(std::holds_alternative<WindowOwnerLifetime>(QueryWindowOwnerLifetime(token)),
        "public root/PID/birth/live lifetime probe accepts only own hidden root");
    Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(token, nullptr)).reason ==
        UnavailableReason::ProcessUnavailable, "window adapter invalid process handle fails closed");
    Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(
        Token(token.Target().hwnd, process.Pid(), start + 1))).reason == UnavailableReason::LifetimeMismatch,
        "window adapter refuses process-birth mismatch before property lookup");
    Check(std::get<MetadataUnavailable>(QueryWindowOwnerMetadata(
        Token(1, process.Pid(), start))).reason == UnavailableReason::LifetimeMismatch,
        "invalid root HWND cannot fall back to valid own process identity");
    CompletionGate gate;
    MetadataWorker worker;
    const auto completion = Complete(worker, Request(gate, token));
    Check(gate.Accept(completion, token) && std::holds_alternative<AppMetadata>(*completion.metadata),
        "production MTA worker queries only hidden own root and completion is accepted");
    const auto cached = Complete(worker, Request(gate, token));
    Check(gate.Accept(cached, token) && cached.metadata == completion.metadata &&
        worker.Statistics().cacheHits == 1, "native owner cache hit revalidates hidden root lifetime");
    DestroyWindow(fixture.hwnd);
    fixture.hwnd = nullptr;
    const auto destroyed = Complete(worker, Request(gate, token));
    Check(gate.Accept(destroyed, token) && std::holds_alternative<MetadataUnavailable>(*destroyed.metadata) &&
        worker.Statistics().cacheHits == 1, "destroyed hidden root cannot reuse production cached process identity");
    Check(worker.Stop().empty(), "native worker COM and query lifetime shut down with no unreported work");
}
}

int main(int argc, char* argv[]) {
    if (argc == 2 && std::strcmp(argv[1], "--owner-lifetime-fixture") == 0) return EXIT_SUCCESS;
    try {
        IdentityTests();
        MetadataTests();
        WindowOwnerTests();
        PreferenceTests();
        SnapshotTests();
        CompletionTests();
        CacheTests();
        OwnerWorkerTests();
        QueueTests();
        ShutdownTests();
        NativeTests();
        std::cout << "Applications: " << checks << " checks passed (owner model + hidden-root/own-process/invalid-handle native queries).\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Applications test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
