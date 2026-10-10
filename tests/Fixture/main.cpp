#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr wchar_t kTargetClass[] = L"WindowSweaters.TestFixture";
constexpr wchar_t kTargetTitle[] = L"WindowSweaters Fixture";
constexpr wchar_t kReceiverClass[] = L"WindowSweaters.TestFixtureReceiver";
constexpr wchar_t kReceiverTitle[] = L"WindowSweaters Fixture Receiver";
constexpr UINT_PTR kAutoExitTimer = 1;
constexpr UINT_PTR kHostPollTimer = 2;
constexpr UINT_PTR kLifecycleTimer = 3;
constexpr UINT_PTR kInputSubclassId = 1;
constexpr int kStartHostControl = 101;
constexpr int kStopHostControl = 102;
constexpr int kRunLifecycleControl = 103;
constexpr int kOpenSettingsControl = 104;
constexpr UINT kHostSettingsCommand = 1002;
constexpr COLORREF kTargetColor = RGB(37, 59, 79);
constexpr COLORREF kReceiverColor = RGB(34, 42, 54);
constexpr int kBorderWidth = 12;
constexpr int kLifecycleMoveOffset = 14;
constexpr int kLifecycleResizeWidth = 24;
constexpr int kLifecycleResizeHeight = 18;
constexpr UINT kLifecyclePollMilliseconds = 100;
constexpr ULONGLONG kLifecycleObservationMilliseconds = 1250;
constexpr ULONGLONG kLifecycleSettleMilliseconds = 450;

enum class HostState
{
    Stopped,
    Starting,
    Ready,
    Stopping,
    Failed
};

enum class LifecyclePhase
{
    Idle,
    WaitForMove,
    WaitForResize,
    Settling,
    WaitForMinimize,
    WaitForRestore,
    Resetting,
    Passed,
    Failed
};

struct HostSnapshot
{
    DWORD hostProcessId = 0;
    HWND hostWindow = nullptr;
    HWND overlayWindow = nullptr;
    HWND targetWindow = nullptr;
    DWORD targetProcessId = 0;
    uint64_t targetProcessStart = 0;
    uint64_t targetGeneration = 0;
    RECT targetBounds{};
    RECT overlayBounds{};
    bool overlayVisible = false;
    bool stopped = false;
    uint64_t trackedEntries = 0;
    uint64_t creates = 0;
    uint64_t removes = 0;
    uint64_t geometryUpdates = 0;
    uint64_t draws = 0;
    uint64_t pureMoves = 0;
    uint64_t hides = 0;
    uint64_t shows = 0;
    uint64_t reorders = 0;
    uint64_t staleActions = 0;
    uint64_t placementFailures = 0;
    uint64_t trackerCallbacks = 0;
    uint64_t trackerCoalesced = 0;
    uint64_t trackerUncertainQueries = 0;
    uint64_t trackerError = 0;
    uint64_t borderPaddingPixels = 0;
};

struct LifecycleCheck
{
    LifecyclePhase phase = LifecyclePhase::Idle;
    uint64_t runId = 0;
    ULONGLONG deadline = 0;
    RECT originalBounds{};
    RECT movedBounds{};
    RECT resizedBounds{};
    RECT movedHostBounds{};
    RECT resizedHostBounds{};
    HostSnapshot baseline{};
    HostSnapshot moveSample{};
    HostSnapshot resizeSample{};
    HostSnapshot minimizedSample{};
    HostSnapshot restoredSample{};
    HostSnapshot resetSample{};
    bool hasResetBounds = false;
    std::string failure;
};

struct Fixture
{
    HANDLE resultFile = INVALID_HANDLE_VALUE;
    HANDLE hostProcess = nullptr;
    HWND target = nullptr;
    HWND receiver = nullptr;
    HWND startHostButton = nullptr;
    HWND stopHostButton = nullptr;
    HWND runLifecycleButton = nullptr;
    HWND openSettingsButton = nullptr;
    HWND statusText = nullptr;
    HWND evidenceText = nullptr;
    HWND settingsStatusText = nullptr;
    HWND lifecycleText = nullptr;
    HWND keyboardLabel = nullptr;
    HWND scrollLabel = nullptr;
    HWND interactionHelp = nullptr;
    HWND keyboardEdit = nullptr;
    HWND scrollList = nullptr;
    HWND hostWindow = nullptr;
    HWND overlayWindow = nullptr;
    DWORD hostProcessId = 0;
    HBRUSH targetBrush = nullptr;
    HBRUSH receiverBrush = nullptr;
    UINT autoExitSeconds = 0;
    HostState hostState = HostState::Stopped;
    LifecycleCheck lifecycle;
    std::wstring settingsStatus = L"idle";
    std::wstring hostStatusDetail;
    std::wstring resultFilePath;
    std::wstring hostProbePath;
    std::wstring cachedStatusText;
    std::wstring cachedEvidenceText;
    std::wstring cachedSettingsStatusText;
    std::wstring cachedLifecycleText;
    size_t clickCount = 0;
    size_t wheelCount = 0;
    size_t keyboardCount = 0;
    size_t focusChangeCount = 0;
    size_t statusUpdateCalls = 0;
    size_t statusTextWrites = 0;
    size_t evidenceTextWrites = 0;
    size_t settingsStatusWrites = 0;
    size_t lifecycleTextWrites = 0;
};

struct Arguments
{
    std::wstring resultFile;
    UINT autoExitSeconds = 0;
};

std::string Utf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(byteCount), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), byteCount, nullptr, nullptr);
    return result;
}

std::string JsonEscape(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                result += '?';
            }
            else
            {
                result += character;
            }
            break;
        }
    }
    return result;
}

std::string JsonString(std::string_view value)
{
    return "\"" + JsonEscape(value) + "\"";
}

std::string JsonString(std::wstring_view value)
{
    return JsonString(Utf8(value));
}

std::string JsonRect(const RECT& rectangle)
{
    std::ostringstream json;
    json << '[' << rectangle.left << ',' << rectangle.top << ',' << rectangle.right << ',' << rectangle.bottom << ']';
    return json.str();
}

std::string JsonPoint(const POINT& point)
{
    std::ostringstream json;
    json << '[' << point.x << ',' << point.y << ']';
    return json.str();
}

std::string HwndHex(HWND window)
{
    std::ostringstream text;
    text << "\"0x" << std::hex << reinterpret_cast<uintptr_t>(window) << '"';
    return text.str();
}

std::wstring HwndHexWide(HWND window)
{
    std::wostringstream text;
    text << L"0x" << std::hex << reinterpret_cast<uintptr_t>(window);
    return text.str();
}

bool AppendResult(Fixture& fixture, const std::string& json)
{
    if (fixture.resultFile == INVALID_HANDLE_VALUE)
    {
        return true;
    }

    const std::string line = json + "\r\n";
    DWORD written = 0;
    return WriteFile(fixture.resultFile, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE &&
        static_cast<size_t>(written) == line.size() &&
        FlushFileBuffers(fixture.resultFile) != FALSE;
}

const char* MessageName(UINT message)
{
    switch (message)
    {
    case WM_ACTIVATE: return "WM_ACTIVATE";
    case WM_CHAR: return "WM_CHAR";
    case WM_CLOSE: return "WM_CLOSE";
    case WM_COMMAND: return "WM_COMMAND";
    case WM_CREATE: return "WM_CREATE";
    case WM_DESTROY: return "WM_DESTROY";
    case WM_DPICHANGED: return "WM_DPICHANGED";
    case WM_ENTERSIZEMOVE: return "WM_ENTERSIZEMOVE";
    case WM_EXITSIZEMOVE: return "WM_EXITSIZEMOVE";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_KILLFOCUS: return "WM_KILLFOCUS";
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP";
    case WM_MOUSEACTIVATE: return "WM_MOUSEACTIVATE";
    case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    case WM_MOVE: return "WM_MOVE";
    case WM_NCLBUTTONDOWN: return "WM_NCLBUTTONDOWN";
    case WM_NCLBUTTONUP: return "WM_NCLBUTTONUP";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
    case WM_SIZE: return "WM_SIZE";
    case WM_SIZING: return "WM_SIZING";
    case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
    default: return nullptr;
    }
}

bool IsTargetOrChild(Fixture& fixture, HWND window)
{
    if (window == fixture.target)
    {
        return true;
    }

    const HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr)
    {
        return false;
    }

    wchar_t className[128]{};
    GetClassNameW(root, className, static_cast<int>(std::size(className)));
    return root == fixture.target || std::wstring_view(className) == kTargetClass;
}

bool IsLifecycleRunning(const Fixture& fixture)
{
    switch (fixture.lifecycle.phase)
    {
    case LifecyclePhase::WaitForMove:
    case LifecyclePhase::WaitForResize:
    case LifecyclePhase::Settling:
    case LifecyclePhase::WaitForMinimize:
    case LifecyclePhase::WaitForRestore:
    case LifecyclePhase::Resetting:
        return true;
    default:
        return false;
    }
}

std::wstring_view ControlName(Fixture& fixture, HWND window)
{
    if (window == fixture.startHostButton)
    {
        return L"start-test-host";
    }
    if (window == fixture.stopHostButton)
    {
        return L"stop-test-host";
    }
    if (window == fixture.runLifecycleButton)
    {
        return L"run-lifecycle-checks";
    }
    if (window == fixture.openSettingsButton)
    {
        return L"open-native-settings";
    }
    if (window == fixture.keyboardEdit)
    {
        return L"keyboard-input";
    }
    if (window == fixture.scrollList)
    {
        return L"scroll-list";
    }
    return {};
}

const wchar_t* LifecyclePhaseName(LifecyclePhase phase)
{
    switch (phase)
    {
    case LifecyclePhase::Idle: return L"idle";
    case LifecyclePhase::WaitForMove: return L"moving";
    case LifecyclePhase::WaitForResize: return L"resizing";
    case LifecyclePhase::Settling: return L"settling";
    case LifecyclePhase::WaitForMinimize: return L"minimizing";
    case LifecyclePhase::WaitForRestore: return L"restoring";
    case LifecyclePhase::Resetting: return L"resetting";
    case LifecyclePhase::Passed: return L"passed";
    case LifecyclePhase::Failed: return L"failed";
    }
    return L"unknown";
}

void LogMessage(Fixture& fixture, HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    const char* name = MessageName(message);
    if (name == nullptr)
    {
        return;
    }

    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    const bool isTarget = IsTargetOrChild(fixture, window);
    const std::wstring_view control = ControlName(fixture, window);
    std::ostringstream json;
    json << "{\"type\":\"window.message\",\"role\":\""
         << (isTarget ? "target" : "receiver")
         << "\",\"hwnd\":" << HwndHex(window)
         << ",\"class\":" << JsonString(className)
         << ",\"control\":" << JsonString(control)
         << ",\"message\":\"" << name
         << "\",\"message_id\":" << message
         << ",\"wparam\":" << static_cast<uint64_t>(wParam)
         << ",\"lparam\":" << static_cast<int64_t>(lParam)
         << '}';
    AppendResult(fixture, json.str());
}

void UpdateStatus(Fixture& fixture)
{
    if (fixture.statusText == nullptr)
    {
        return;
    }
    ++fixture.statusUpdateCalls;

    const wchar_t* hostState = L"stopped";
    switch (fixture.hostState)
    {
    case HostState::Starting: hostState = L"starting"; break;
    case HostState::Ready: hostState = L"ready"; break;
    case HostState::Stopping: hostState = L"stopping"; break;
    case HostState::Failed: hostState = L"failed"; break;
    case HostState::Stopped: break;
    }

    std::wostringstream text;
    text << L"Host: " << hostState << L" | lifecycle: " << LifecyclePhaseName(fixture.lifecycle.phase);
    if (fixture.hostProcessId != 0)
    {
        text << L" | PID " << fixture.hostProcessId;
    }
    if (!fixture.hostStatusDetail.empty())
    {
        text << L" | " << fixture.hostStatusDetail;
    }
    const std::wstring nextStatus = text.str();
    if (nextStatus != fixture.cachedStatusText &&
        SetWindowTextW(fixture.statusText, nextStatus.c_str()))
    {
        fixture.cachedStatusText = nextStatus;
        ++fixture.statusTextWrites;
    }
    if (fixture.evidenceText != nullptr)
    {
        std::wostringstream evidence;
        evidence << L"Fixture received: clicks " << fixture.clickCount
                 << L" | wheel " << fixture.wheelCount
                 << L" | keyboard " << fixture.keyboardCount
                 << L" | focus changes " << fixture.focusChangeCount;
        const std::wstring nextEvidence = evidence.str();
        if (nextEvidence != fixture.cachedEvidenceText &&
            SetWindowTextW(fixture.evidenceText, nextEvidence.c_str()))
        {
            fixture.cachedEvidenceText = nextEvidence;
            ++fixture.evidenceTextWrites;
        }
    }
    const std::wstring nextSettings = L"Settings: " + fixture.settingsStatus;
    if (fixture.settingsStatusText != nullptr && nextSettings != fixture.cachedSettingsStatusText &&
        SetWindowTextW(fixture.settingsStatusText, nextSettings.c_str()))
    {
        fixture.cachedSettingsStatusText = nextSettings;
        ++fixture.settingsStatusWrites;
    }
    if (fixture.lifecycleText != nullptr)
    {
        std::wostringstream lifecycle;
        lifecycle << L"Lifecycle #" << fixture.lifecycle.runId << L": "
                  << LifecyclePhaseName(fixture.lifecycle.phase);
        if (!fixture.lifecycle.failure.empty())
        {
            lifecycle << L" — " << fixture.lifecycle.failure.c_str();
        }
        lifecycle << L" | Evidence: " << fixture.resultFilePath.c_str();
        const std::wstring nextLifecycle = lifecycle.str();
        if (nextLifecycle != fixture.cachedLifecycleText &&
            SetWindowTextW(fixture.lifecycleText, nextLifecycle.c_str()))
        {
            fixture.cachedLifecycleText = nextLifecycle;
            ++fixture.lifecycleTextWrites;
        }
    }
}

void RecordMessage(Fixture& fixture, HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    bool countersChanged = false;
    if (IsTargetOrChild(fixture, window))
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
            ++fixture.clickCount;
            countersChanged = true;
            break;
        case WM_MOUSEWHEEL:
            ++fixture.wheelCount;
            countersChanged = true;
            break;
        case WM_KEYDOWN:
        case WM_CHAR:
            ++fixture.keyboardCount;
            countersChanged = true;
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            ++fixture.focusChangeCount;
            countersChanged = true;
            break;
        }
    }

    LogMessage(fixture, window, message, wParam, lParam);
    if (countersChanged)
    {
        UpdateStatus(fixture);
    }
}

bool GetExecutableDirectory(std::wstring& directory)
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= static_cast<DWORD>(path.size()))
    {
        return false;
    }

    path.resize(length);
    const size_t separator = std::wstring_view(path.data(), path.size()).find_last_of(L"\\/");
    if (separator == std::wstring_view::npos)
    {
        return false;
    }
    directory.assign(path.data(), separator);
    return true;
}

bool EnsureEvidencePaths(Fixture& fixture, std::wstring_view requestedPath)
{
    if (!requestedPath.empty())
    {
        std::vector<wchar_t> absolutePath(32768);
        const DWORD length = GetFullPathNameW(
            std::wstring(requestedPath).c_str(),
            static_cast<DWORD>(absolutePath.size()),
            absolutePath.data(),
            nullptr);
        if (length == 0 || length >= absolutePath.size())
        {
            return false;
        }
        fixture.resultFilePath.assign(absolutePath.data(), length);
    }
    else
    {
        std::wstring executableDirectory;
        if (!GetExecutableDirectory(executableDirectory))
        {
            return false;
        }

        const std::wstring evidenceDirectory = executableDirectory + L"\\probe-results";
        if (!CreateDirectoryW(evidenceDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return false;
        }

        const DWORD attributes = GetFileAttributesW(evidenceDirectory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return false;
        }

        std::wostringstream name;
        name << evidenceDirectory << L"\\fixture." << GetCurrentProcessId() << L'.' << GetTickCount64() << L".ndjson";
        fixture.resultFilePath = name.str();
    }

    fixture.hostProbePath = fixture.resultFilePath + L".host.json";
    return true;
}

bool ReadSmallFile(const std::wstring& path, std::string& contents)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || length.QuadPart > 65536)
    {
        CloseHandle(file);
        return false;
    }

    contents.resize(static_cast<size_t>(length.QuadPart));
    DWORD bytesRead = 0;
    const bool read = contents.empty() ||
        (ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &bytesRead, nullptr) != FALSE &&
         static_cast<size_t>(bytesRead) == contents.size());
    CloseHandle(file);
    return read;
}

bool ReadJsonNumber(const std::string& json, std::string_view key, uint64_t& value)
{
    const std::string name = "\"" + std::string(key) + "\"";
    size_t position = json.find(name);
    if (position == std::string::npos)
    {
        return false;
    }

    position = json.find(':', position + name.size());
    if (position == std::string::npos)
    {
        return false;
    }
    ++position;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\t'))
    {
        ++position;
    }

    const char* begin = json.c_str() + position;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(begin, &end, 10);
    if (end == begin)
    {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool ReadJsonBoolean(const std::string& json, std::string_view key, bool& value)
{
    const std::string name = "\"" + std::string(key) + "\"";
    const size_t namePosition = json.find(name);
    if (namePosition == std::string::npos)
    {
        return false;
    }
    const size_t separator = json.find(':', namePosition + name.size());
    if (separator == std::string::npos)
    {
        return false;
    }

    const size_t truePosition = json.find("true", separator + 1);
    const size_t falsePosition = json.find("false", separator + 1);
    if (truePosition == separator + 1 || falsePosition == separator + 1)
    {
        return false;
    }
    if (truePosition != std::string::npos &&
        (falsePosition == std::string::npos || truePosition < falsePosition))
    {
        value = true;
        return true;
    }
    if (falsePosition != std::string::npos)
    {
        value = false;
        return true;
    }
    return false;
}

bool ReadJsonRect(const std::string& json, std::string_view key, RECT& rectangle)
{
    const std::string name = "\"" + std::string(key) + "\"";
    size_t position = json.find(name);
    if (position == std::string::npos)
    {
        return false;
    }
    position = json.find('[', position + name.size());
    if (position == std::string::npos)
    {
        return false;
    }
    ++position;

    LONG* coordinates[] = { &rectangle.left, &rectangle.top, &rectangle.right, &rectangle.bottom };
    for (size_t index = 0; index < std::size(coordinates); ++index)
    {
        while (position < json.size() && (json[position] == ' ' || json[position] == '\t'))
        {
            ++position;
        }
        const char* begin = json.c_str() + position;
        char* end = nullptr;
        const long parsed = std::strtol(begin, &end, 10);
        if (end == begin || parsed < LONG_MIN || parsed > LONG_MAX)
        {
            return false;
        }
        *coordinates[index] = static_cast<LONG>(parsed);
        position = static_cast<size_t>(end - json.c_str());
        while (position < json.size() && (json[position] == ' ' || json[position] == '\t'))
        {
            ++position;
        }
        if (index + 1 < std::size(coordinates))
        {
            if (position >= json.size() || json[position] != ',')
            {
                return false;
            }
            ++position;
        }
    }
    return position < json.size() && json[position] == ']';
}

struct OwnedHostWindowSearch
{
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindOwnedHostWindow(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<OwnedHostWindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId)
    {
        return TRUE;
    }

    wchar_t className[128]{};
    wchar_t title[256]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (std::wstring_view(className) == L"WindowSweaters.Host" &&
        std::wstring_view(title) == L"Window Sweaters")
    {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND GetOwnedHostWindow(Fixture& fixture)
{
    if (fixture.hostProcessId == 0)
    {
        return nullptr;
    }

    if (fixture.hostWindow != nullptr && IsWindow(fixture.hostWindow))
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(fixture.hostWindow, &processId);
        wchar_t className[128]{};
        GetClassNameW(fixture.hostWindow, className, static_cast<int>(std::size(className)));
        if (processId == fixture.hostProcessId && std::wstring_view(className) == L"WindowSweaters.Host")
        {
            return fixture.hostWindow;
        }
    }

    OwnedHostWindowSearch search{ fixture.hostProcessId, nullptr };
    EnumWindows(FindOwnedHostWindow, reinterpret_cast<LPARAM>(&search));
    fixture.hostWindow = search.window;
    return fixture.hostWindow;
}

bool RequestOwnedHostSettings(Fixture& fixture)
{
    if (fixture.hostProcess == nullptr ||
        WaitForSingleObject(fixture.hostProcess, 0) != WAIT_TIMEOUT ||
        GetProcessId(fixture.hostProcess) != fixture.hostProcessId)
    {
        fixture.settingsStatus = L"host unavailable";
        UpdateStatus(fixture);
        return false;
    }

    const HWND host = GetOwnedHostWindow(fixture);
    DWORD ownerProcessId = 0;
    if (host != nullptr)
    {
        GetWindowThreadProcessId(host, &ownerProcessId);
    }

    wchar_t className[128]{};
    wchar_t title[256]{};
    if (host != nullptr)
    {
        GetClassNameW(host, className, static_cast<int>(std::size(className)));
        GetWindowTextW(host, title, static_cast<int>(std::size(title)));
    }
    const bool ownedHost = host != nullptr &&
        ownerProcessId == fixture.hostProcessId &&
        ownerProcessId == GetProcessId(fixture.hostProcess) &&
        std::wstring_view(className) == L"WindowSweaters.Host" &&
        std::wstring_view(title) == L"Window Sweaters";
    const bool posted = ownedHost && PostMessageW(
        host,
        WM_COMMAND,
        MAKEWPARAM(kHostSettingsCommand, 0),
        0);
    fixture.settingsStatus = posted ? L"request posted to owned host" :
        ownedHost ? L"host rejected settings request" : L"host ownership check failed";

    std::ostringstream json;
    json << "{\"type\":\"fixture.settings_request\",\"command_id\":" << kHostSettingsCommand
         << ",\"host_hwnd\":" << HwndHex(host)
         << ",\"host_pid\":" << ownerProcessId
         << ",\"tracked_host_pid\":" << fixture.hostProcessId
         << ",\"host_class\":" << JsonString(className)
         << ",\"host_title\":" << JsonString(title)
         << ",\"posted\":" << (posted ? "true" : "false") << '}';
    AppendResult(fixture, json.str());
    UpdateStatus(fixture);
    return posted != FALSE;
}

bool ReadHostSnapshot(Fixture& fixture, HostSnapshot& snapshot)
{
    std::string json;
    if (!ReadSmallFile(fixture.hostProbePath, json))
    {
        return false;
    }

    uint64_t hostProcessId = 0;
    uint64_t hostWindow = 0;
    uint64_t overlayWindow = 0;
    uint64_t targetWindow = 0;
    uint64_t targetProcessId = 0;
    bool overlayVisible = false;
    bool stopped = false;
    if (!ReadJsonNumber(json, "host_pid", hostProcessId) ||
        !ReadJsonNumber(json, "host_hwnd", hostWindow) ||
        !ReadJsonNumber(json, "overlay_hwnd", overlayWindow) ||
        !ReadJsonNumber(json, "target_hwnd", targetWindow) ||
        !ReadJsonNumber(json, "target_pid", targetProcessId) ||
        !ReadJsonNumber(json, "target_process_start", snapshot.targetProcessStart) ||
        !ReadJsonNumber(json, "target_generation", snapshot.targetGeneration) ||
        !ReadJsonNumber(json, "tracked_entries", snapshot.trackedEntries) ||
        !ReadJsonNumber(json, "creates", snapshot.creates) ||
        !ReadJsonNumber(json, "removes", snapshot.removes) ||
        !ReadJsonNumber(json, "geometry_updates", snapshot.geometryUpdates) ||
        !ReadJsonNumber(json, "draws", snapshot.draws) ||
        !ReadJsonNumber(json, "pure_moves", snapshot.pureMoves) ||
        !ReadJsonNumber(json, "hides", snapshot.hides) ||
        !ReadJsonNumber(json, "shows", snapshot.shows) ||
        !ReadJsonNumber(json, "reorders", snapshot.reorders) ||
        !ReadJsonNumber(json, "stale_actions", snapshot.staleActions) ||
        !ReadJsonNumber(json, "placement_failures", snapshot.placementFailures) ||
        !ReadJsonNumber(json, "tracker_callbacks", snapshot.trackerCallbacks) ||
        !ReadJsonNumber(json, "tracker_coalesced", snapshot.trackerCoalesced) ||
        !ReadJsonNumber(json, "tracker_uncertain_queries", snapshot.trackerUncertainQueries) ||
        !ReadJsonNumber(json, "tracker_error", snapshot.trackerError) ||
        !ReadJsonNumber(json, "border_width_pixels", snapshot.borderPaddingPixels) ||
        !ReadJsonRect(json, "target_bounds", snapshot.targetBounds) ||
        !ReadJsonRect(json, "overlay_bounds", snapshot.overlayBounds) ||
        !ReadJsonBoolean(json, "overlay_visible", overlayVisible) ||
        !ReadJsonBoolean(json, "stopped", stopped))
    {
        return false;
    }

    snapshot.hostProcessId = static_cast<DWORD>(hostProcessId);
    snapshot.hostWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(hostWindow));
    snapshot.overlayWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(overlayWindow));
    snapshot.targetWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(targetWindow));
    snapshot.targetProcessId = static_cast<DWORD>(targetProcessId);
    snapshot.overlayVisible = overlayVisible;
    snapshot.stopped = stopped;

    DWORD actualHostPid = 0;
    DWORD actualOverlayPid = 0;
    DWORD actualTargetPid = 0;
    wchar_t hostClass[128]{};
    wchar_t overlayClass[128]{};
    wchar_t targetClass[128]{};
    if (snapshot.hostProcessId != fixture.hostProcessId ||
        snapshot.targetWindow != fixture.target ||
        snapshot.targetProcessId != GetCurrentProcessId() ||
        snapshot.targetProcessStart == 0 ||
        snapshot.targetGeneration == 0 ||
        snapshot.borderPaddingPixels == 0 ||
        snapshot.borderPaddingPixels > 512 ||
        snapshot.trackedEntries != 1 ||
        snapshot.hostWindow == nullptr ||
        snapshot.overlayWindow == nullptr ||
        !IsWindow(snapshot.hostWindow) ||
        !IsWindow(snapshot.overlayWindow) ||
        GetWindowThreadProcessId(snapshot.hostWindow, &actualHostPid) == 0 ||
        GetWindowThreadProcessId(snapshot.overlayWindow, &actualOverlayPid) == 0 ||
        GetWindowThreadProcessId(snapshot.targetWindow, &actualTargetPid) == 0 ||
        actualHostPid != fixture.hostProcessId ||
        actualOverlayPid != fixture.hostProcessId ||
        actualTargetPid != GetCurrentProcessId())
    {
        return false;
    }

    GetClassNameW(snapshot.hostWindow, hostClass, static_cast<int>(std::size(hostClass)));
    GetClassNameW(snapshot.overlayWindow, overlayClass, static_cast<int>(std::size(overlayClass)));
    GetClassNameW(snapshot.targetWindow, targetClass, static_cast<int>(std::size(targetClass)));
    if (std::wstring_view(hostClass) != L"WindowSweaters.Host" ||
        std::wstring_view(overlayClass) != L"WindowSweaters.Overlay" ||
        std::wstring_view(targetClass) != kTargetClass)
    {
        return false;
    }
    return true;
}

void AppendHostEvent(Fixture& fixture, const char* eventName, DWORD exitCode)
{
    std::ostringstream json;
    json << "{\"type\":\"host.lifecycle\",\"event\":\"" << eventName
         << "\",\"pid\":" << fixture.hostProcessId
         << ",\"code\":" << exitCode
         << ",\"host_hwnd\":" << HwndHex(fixture.hostWindow)
         << ",\"overlay_hwnd\":" << HwndHex(fixture.overlayWindow)
         << '}';
    AppendResult(fixture, json.str());
}

void CloseHostProcessHandle(Fixture& fixture)
{
    if (fixture.hostProcess != nullptr)
    {
        CloseHandle(fixture.hostProcess);
        fixture.hostProcess = nullptr;
    }
    fixture.hostProcessId = 0;
    fixture.hostWindow = nullptr;
    fixture.overlayWindow = nullptr;
}

bool VerifyHostProbe(Fixture& fixture)
{
    HostSnapshot snapshot;
    if (!ReadHostSnapshot(fixture, snapshot))
    {
        return false;
    }

    fixture.hostWindow = snapshot.hostWindow;
    fixture.overlayWindow = snapshot.overlayWindow;
    fixture.hostState = HostState::Ready;
    return true;
}

bool SameRect(const RECT& left, const RECT& right)
{
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

RECT OffsetRect(const RECT& rectangle, LONG x, LONG y)
{
    return {
        rectangle.left + x,
        rectangle.top + y,
        rectangle.right + x,
        rectangle.bottom + y
    };
}

bool ReadOwnedWindowRect(Fixture& fixture, HWND window, RECT& bounds, bool target)
{
    if (window == nullptr || window != (target ? fixture.target : fixture.receiver) || !IsWindow(window))
    {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    const std::wstring_view expectedClass = target ? kTargetClass : kReceiverClass;
    if (processId != GetCurrentProcessId() || std::wstring_view(className) != expectedClass)
    {
        return false;
    }
    return GetWindowRect(window, &bounds) != FALSE;
}

std::string JsonSnapshot(const HostSnapshot& snapshot)
{
    std::ostringstream json;
    json << "{\"host_pid\":" << snapshot.hostProcessId
         << ",\"target_identity\":{\"hwnd\":" << HwndHex(snapshot.targetWindow)
         << ",\"pid\":" << snapshot.targetProcessId
         << ",\"process_start\":" << snapshot.targetProcessStart
         << ",\"generation\":" << snapshot.targetGeneration << '}'
         << ",\"tracked_entries\":" << snapshot.trackedEntries
         << ",\"target_bounds\":" << JsonRect(snapshot.targetBounds)
         << ",\"overlay_bounds\":" << JsonRect(snapshot.overlayBounds)
         << ",\"target_visible\":" << (IsWindowVisible(snapshot.targetWindow) ? "true" : "false")
         << ",\"target_iconic\":" << (IsIconic(snapshot.targetWindow) ? "true" : "false")
         << ",\"overlay_visible\":" << (snapshot.overlayVisible ? "true" : "false")
         << ",\"stopped\":" << (snapshot.stopped ? "true" : "false")
         << ",\"counters\":{\"creates\":" << snapshot.creates
         << ",\"removes\":" << snapshot.removes
         << ",\"geometry_updates\":" << snapshot.geometryUpdates
         << ",\"draws\":" << snapshot.draws
         << ",\"pure_moves\":" << snapshot.pureMoves
         << ",\"hides\":" << snapshot.hides
         << ",\"shows\":" << snapshot.shows
         << ",\"reorders\":" << snapshot.reorders
         << ",\"stale_actions\":" << snapshot.staleActions
         << ",\"placement_failures\":" << snapshot.placementFailures
         << ",\"tracker_callbacks\":" << snapshot.trackerCallbacks
         << ",\"tracker_coalesced\":" << snapshot.trackerCoalesced
         << ",\"tracker_uncertain_queries\":" << snapshot.trackerUncertainQueries
         << ",\"tracker_error\":" << snapshot.trackerError << "}}";
    return json.str();
}

void AppendLifecycleRecord(
    Fixture& fixture,
    const char* eventName,
    const char* detail,
    const HostSnapshot* snapshot)
{
    RECT targetBounds{};
    RECT receiverBounds{};
    const bool hasTargetBounds = ReadOwnedWindowRect(fixture, fixture.target, targetBounds, true);
    const bool hasReceiverBounds = ReadOwnedWindowRect(fixture, fixture.receiver, receiverBounds, false);
    std::ostringstream json;
    json << "{\"type\":\"fixture.lifecycle\",\"operation_id\":" << fixture.lifecycle.runId
         << ",\"event\":\"" << eventName << '"'
         << ",\"detail\":" << JsonString(detail == nullptr ? "" : detail)
         << ",\"target_hwnd\":" << HwndHex(fixture.target)
         << ",\"target_bounds\":" << (hasTargetBounds ? JsonRect(targetBounds) : "null")
         << ",\"receiver_hwnd\":" << HwndHex(fixture.receiver)
         << ",\"receiver_bounds\":" << (hasReceiverBounds ? JsonRect(receiverBounds) : "null")
         << ",\"target_visible\":" << (fixture.target != nullptr && IsWindowVisible(fixture.target) ? "true" : "false")
         << ",\"target_iconic\":" << (fixture.target != nullptr && IsIconic(fixture.target) ? "true" : "false")
         << ",\"snapshot\":" << (snapshot == nullptr ? "null" : JsonSnapshot(*snapshot))
         << '}';
    AppendResult(fixture, json.str());
}

bool MoveOwnedTarget(Fixture& fixture, const RECT& bounds, const char* eventName);

void FinishLifecycleCheck(Fixture& fixture, LifecyclePhase phase, const char* detail)
{
    const bool wasRunning = IsLifecycleRunning(fixture);
    RECT currentBounds{};
    if (phase == LifecyclePhase::Failed && wasRunning &&
        ReadOwnedWindowRect(fixture, fixture.target, currentBounds, true))
    {
        if (IsIconic(fixture.target))
        {
            ShowWindow(fixture.target, SW_SHOWNOACTIVATE);
        }
        MoveOwnedTarget(fixture, fixture.lifecycle.originalBounds, "failure_reset_requested");
    }
    fixture.lifecycle.phase = phase;
    if (phase == LifecyclePhase::Failed)
    {
        fixture.lifecycle.failure = detail == nullptr ? "lifecycle probe failed" : detail;
    }
    AppendLifecycleRecord(fixture, phase == LifecyclePhase::Passed ? "complete" : "failed", detail, nullptr);
    KillTimer(fixture.target, kLifecycleTimer);
    UpdateStatus(fixture);
}

bool ValidateLifecycleIdentity(Fixture& fixture, const HostSnapshot& snapshot)
{
    const HostSnapshot& baseline = fixture.lifecycle.baseline;
    return snapshot.hostProcessId == fixture.hostProcessId &&
        snapshot.hostWindow == baseline.hostWindow &&
        snapshot.overlayWindow == baseline.overlayWindow &&
        snapshot.targetWindow == fixture.target &&
        snapshot.targetProcessId == baseline.targetProcessId &&
        snapshot.targetProcessStart == baseline.targetProcessStart &&
        snapshot.targetGeneration == baseline.targetGeneration &&
        snapshot.trackedEntries == 1 &&
        snapshot.creates == baseline.creates &&
        snapshot.removes == baseline.removes &&
        !snapshot.stopped;
}

bool ValidateReceiverContainment(Fixture& fixture, const RECT& targetBounds)
{
    RECT receiverBounds{};
    const LONG padding = static_cast<LONG>(fixture.lifecycle.baseline.borderPaddingPixels);
    return ReadOwnedWindowRect(fixture, fixture.receiver, receiverBounds, false) &&
        targetBounds.left - padding >= receiverBounds.left &&
        targetBounds.top - padding >= receiverBounds.top &&
        targetBounds.right + padding <= receiverBounds.right &&
        targetBounds.bottom + padding <= receiverBounds.bottom;
}

bool MoveOwnedTarget(Fixture& fixture, const RECT& bounds, const char* eventName)
{
    RECT actual{};
    RECT receiverBounds{};
    DWORD processId = 0;
    GetWindowThreadProcessId(fixture.target, &processId);
    const bool isOwned = processId == GetCurrentProcessId() &&
        ReadOwnedWindowRect(fixture, fixture.target, actual, true) &&
        ReadOwnedWindowRect(fixture, fixture.receiver, receiverBounds, false);
    const bool insideReceiver = isOwned &&
        ValidateReceiverContainment(fixture, bounds);
    const BOOL moved = insideReceiver
        ? SetWindowPos(
            fixture.target,
            nullptr,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER)
        : FALSE;

    std::ostringstream detail;
    detail << "requested=" << JsonRect(bounds)
           << ",owned=" << (isOwned ? "true" : "false")
           << ",inside_receiver=" << (insideReceiver ? "true" : "false")
           << ",set_window_pos=" << (moved ? "true" : "false")
           << ",error=" << (moved ? 0 : GetLastError());
    AppendLifecycleRecord(fixture, eventName, detail.str().c_str(), nullptr);
    return moved != FALSE;
}

void StartLifecycleCheck(Fixture& fixture)
{
    if (fixture.lifecycle.phase != LifecyclePhase::Idle &&
        fixture.lifecycle.phase != LifecyclePhase::Passed &&
        fixture.lifecycle.phase != LifecyclePhase::Failed)
    {
        AppendLifecycleRecord(fixture, "duplicate_start_ignored", "a lifecycle run is already active", nullptr);
        return;
    }

    const uint64_t nextRunId = fixture.lifecycle.runId + 1;
    if (fixture.hostState != HostState::Ready || fixture.hostProcess == nullptr ||
        WaitForSingleObject(fixture.hostProcess, 0) != WAIT_TIMEOUT)
    {
        fixture.lifecycle.runId = nextRunId;
        fixture.lifecycle.phase = LifecyclePhase::Failed;
        fixture.lifecycle.failure = "start the test host and wait for Host ready";
        AppendLifecycleRecord(fixture, "failed", fixture.lifecycle.failure.c_str(), nullptr);
        UpdateStatus(fixture);
        return;
    }

    HostSnapshot baseline;
    if (!ReadHostSnapshot(fixture, baseline) || !baseline.overlayVisible || baseline.stopped)
    {
        fixture.lifecycle.runId = nextRunId;
        fixture.lifecycle.phase = LifecyclePhase::Failed;
        fixture.lifecycle.failure = "host probe is not ready with a visible overlay";
        AppendLifecycleRecord(fixture, "failed", fixture.lifecycle.failure.c_str(), nullptr);
        UpdateStatus(fixture);
        return;
    }

    RECT original{};
    const LONG padding = static_cast<LONG>(baseline.borderPaddingPixels);
    if (!ReadOwnedWindowRect(fixture, fixture.target, original, true) ||
        !SameRect(baseline.overlayBounds, RECT{
            baseline.targetBounds.left - padding,
            baseline.targetBounds.top - padding,
            baseline.targetBounds.right + padding,
            baseline.targetBounds.bottom + padding }))
    {
        fixture.lifecycle.runId = nextRunId;
        fixture.lifecycle.phase = LifecyclePhase::Failed;
        fixture.lifecycle.failure = "target identity or original fixture geometry is invalid";
        AppendLifecycleRecord(fixture, "failed", fixture.lifecycle.failure.c_str(), &baseline);
        UpdateStatus(fixture);
        return;
    }

    RECT receiverBounds{};
    const LONG margin = padding + kLifecycleMoveOffset +
        (std::max)(kLifecycleResizeWidth, kLifecycleResizeHeight) + 8;
    if (!ReadOwnedWindowRect(fixture, fixture.receiver, receiverBounds, false) ||
        !SetWindowPos(fixture.receiver, nullptr, original.left - margin, original.top - margin,
            original.right - original.left + 2 * margin,
            original.bottom - original.top + 2 * margin,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
    {
        fixture.lifecycle.runId = nextRunId;
        fixture.lifecycle.phase = LifecyclePhase::Failed;
        fixture.lifecycle.failure = "fixture receiver could not surround the current target";
        AppendLifecycleRecord(fixture, "failed", fixture.lifecycle.failure.c_str(), &baseline);
        UpdateStatus(fixture);
        return;
    }

    LifecycleCheck next;
    next.phase = LifecyclePhase::WaitForMove;
    next.runId = nextRunId;
    next.originalBounds = original;
    next.movedBounds = {
        original.left + kLifecycleMoveOffset,
        original.top + kLifecycleMoveOffset,
        original.right + kLifecycleMoveOffset,
        original.bottom + kLifecycleMoveOffset
    };
    next.resizedBounds = next.movedBounds;
    next.resizedBounds.right += kLifecycleResizeWidth;
    next.resizedBounds.bottom += kLifecycleResizeHeight;
    next.movedHostBounds = OffsetRect(baseline.targetBounds, kLifecycleMoveOffset, kLifecycleMoveOffset);
    next.resizedHostBounds = next.movedHostBounds;
    next.resizedHostBounds.right += kLifecycleResizeWidth;
    next.resizedHostBounds.bottom += kLifecycleResizeHeight;
    next.baseline = baseline;
    next.deadline = GetTickCount64() + kLifecycleObservationMilliseconds;
    fixture.lifecycle = next;
    AppendLifecycleRecord(fixture, "started", "move, resize, settle, minimize, restore, sample, reset", &baseline);
    if (!MoveOwnedTarget(fixture, fixture.lifecycle.movedBounds, "move_requested"))
    {
        FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "fixture-only move request failed");
        return;
    }
    if (!SetTimer(fixture.target, kLifecycleTimer, kLifecyclePollMilliseconds, nullptr))
    {
        MoveOwnedTarget(fixture, fixture.lifecycle.originalBounds, "reset_after_timer_failure");
        FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "lifecycle timer could not start");
        return;
    }
    UpdateStatus(fixture);
}

void AdvanceLifecycleCheck(Fixture& fixture)
{
    LifecycleCheck& check = fixture.lifecycle;
    const ULONGLONG now = GetTickCount64();
    HostSnapshot snapshot;
    if (!ReadHostSnapshot(fixture, snapshot))
    {
        if (now >= check.deadline)
        {
            if (check.phase == LifecyclePhase::Resetting)
            {
                FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host did not publish reset geometry before timeout");
            }
            else
            {
                FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host probe became unavailable during lifecycle observation");
            }
        }
        return;
    }
    if (!ValidateLifecycleIdentity(fixture, snapshot))
    {
        FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "fullTargetId changed or the host stopped tracking the fixture");
        return;
    }

    if (check.phase == LifecyclePhase::WaitForMove)
    {
        if (snapshot.pureMoves > check.baseline.pureMoves &&
            SameRect(snapshot.targetBounds, check.movedHostBounds) &&
            snapshot.overlayVisible &&
            SameRect(snapshot.overlayBounds, RECT{
                check.baseline.overlayBounds.left + kLifecycleMoveOffset,
                check.baseline.overlayBounds.top + kLifecycleMoveOffset,
                check.baseline.overlayBounds.right + kLifecycleMoveOffset,
                check.baseline.overlayBounds.bottom + kLifecycleMoveOffset }))
        {
            check.moveSample = snapshot;
            AppendLifecycleRecord(fixture, "move_observed", "pure move retained the same overlay and target identity", &snapshot);
            check.phase = LifecyclePhase::WaitForResize;
            check.deadline = now + kLifecycleObservationMilliseconds;
            if (!MoveOwnedTarget(fixture, check.resizedBounds, "resize_requested"))
            {
                FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "fixture-only resize request failed");
            }
            UpdateStatus(fixture);
            return;
        }
        if (now >= check.deadline)
        {
            FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host did not observe the fixture-only move");
        }
        return;
    }

    if (check.phase == LifecyclePhase::WaitForResize)
    {
        if (snapshot.geometryUpdates > check.moveSample.geometryUpdates &&
            SameRect(snapshot.targetBounds, check.resizedHostBounds) &&
            snapshot.overlayVisible &&
            snapshot.draws > check.moveSample.draws)
        {
            check.resizeSample = snapshot;
            AppendLifecycleRecord(fixture, "resize_observed", "host updated geometry and drew the resized overlay", &snapshot);
            check.phase = LifecyclePhase::Settling;
            check.deadline = now + kLifecycleSettleMilliseconds;
            UpdateStatus(fixture);
            return;
        }
        if (now >= check.deadline)
        {
            FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host did not observe and draw the fixture-only resize");
        }
        return;
    }

    if (check.phase == LifecyclePhase::Settling)
    {
        if (now < check.deadline)
        {
            return;
        }
        if (!ReadHostSnapshot(fixture, check.resizeSample) ||
            !ValidateLifecycleIdentity(fixture, check.resizeSample) ||
            !SameRect(check.resizeSample.targetBounds, check.resizedHostBounds) ||
            !check.resizeSample.overlayVisible)
        {
            FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host geometry did not settle after resize");
            return;
        }
        AppendLifecycleRecord(fixture, "settled", "target geometry remained stable during the settle interval", &check.resizeSample);
        ShowWindow(fixture.target, SW_SHOWMINNOACTIVE);
        check.phase = LifecyclePhase::WaitForMinimize;
        check.deadline = now + kLifecycleObservationMilliseconds;
        AppendLifecycleRecord(fixture, "minimize_requested", "ShowWindow(SW_SHOWMINNOACTIVE)", nullptr);
        UpdateStatus(fixture);
        return;
    }

    if (check.phase == LifecyclePhase::WaitForMinimize)
    {
        if (IsIconic(fixture.target) && !snapshot.overlayVisible && snapshot.hides > check.resizeSample.hides)
        {
            check.minimizedSample = snapshot;
            AppendLifecycleRecord(fixture, "minimized_observed", "overlay hidden while fullTargetId remained tracked", &snapshot);
            ShowWindow(fixture.target, SW_SHOWNOACTIVATE);
            check.phase = LifecyclePhase::WaitForRestore;
            check.deadline = now + kLifecycleObservationMilliseconds;
            AppendLifecycleRecord(fixture, "restore_requested", "ShowWindow(SW_SHOWNOACTIVATE)", nullptr);
            UpdateStatus(fixture);
            return;
        }
        if (now >= check.deadline)
        {
            FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host did not hide the overlay for the minimized fixture");
        }
        return;
    }

    if (check.phase == LifecyclePhase::WaitForRestore)
    {
        if (!IsIconic(fixture.target) && IsWindowVisible(fixture.target) &&
            snapshot.overlayVisible && snapshot.shows > check.minimizedSample.shows &&
            SameRect(snapshot.targetBounds, check.resizedHostBounds))
        {
            check.restoredSample = snapshot;
            AppendLifecycleRecord(fixture, "restored_observed", "host showed the same target decoration after restore", &snapshot);
            check.phase = LifecyclePhase::Resetting;
            check.deadline = now + kLifecycleObservationMilliseconds;
            if (!MoveOwnedTarget(fixture, check.originalBounds, "reset_requested"))
            {
                FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "could not restore the fixture's original bounds");
                return;
            }
            UpdateStatus(fixture);
            return;
        }
        if (now >= check.deadline)
        {
            FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host did not restore the overlay after the fixture was restored");
        }
        return;
    }

    if (check.phase == LifecyclePhase::Resetting)
    {
        if (SameRect(snapshot.targetBounds, check.baseline.targetBounds) &&
            snapshot.overlayVisible &&
            ReadOwnedWindowRect(fixture, fixture.target, check.originalBounds, true))
        {
            check.resetSample = snapshot;
            AppendLifecycleRecord(fixture, "reset_observed", "fixture returned to its original bounds without target destruction", &snapshot);
            FinishLifecycleCheck(fixture, LifecyclePhase::Passed, "move/resize/minimize/restore preserved fullTargetId and clean overlay visibility");
            return;
        }
        if (now >= check.deadline)
        {
            FinishLifecycleCheck(fixture, LifecyclePhase::Failed, "host did not restore overlay after fixture bounds reset");
        }
    }
}

void RequestHostStop(Fixture& fixture)
{
    if (fixture.hostProcess == nullptr)
    {
        fixture.hostState = HostState::Stopped;
        UpdateStatus(fixture);
        return;
    }

    if (WaitForSingleObject(fixture.hostProcess, 0) == WAIT_OBJECT_0)
    {
        DWORD exitCode = 0;
        GetExitCodeProcess(fixture.hostProcess, &exitCode);
        AppendHostEvent(fixture, exitCode == 0 ? "stopped" : "failed", exitCode);
        CloseHostProcessHandle(fixture);
        fixture.hostState = exitCode == 0 ? HostState::Stopped : HostState::Failed;
        UpdateStatus(fixture);
        return;
    }

    HWND host = GetOwnedHostWindow(fixture);
    DWORD owner = 0;
    if (host != nullptr)
    {
        GetWindowThreadProcessId(host, &owner);
    }
    if (host == nullptr || owner != fixture.hostProcessId)
    {
        fixture.hostState = HostState::Failed;
        AppendHostEvent(fixture, "close_not_posted", 0);
        UpdateStatus(fixture);
        return;
    }

    fixture.hostState = HostState::Stopping;
    if (!PostMessageW(host, WM_CLOSE, 0, 0))
    {
        fixture.hostState = HostState::Failed;
        AppendHostEvent(fixture, "close_post_failed", GetLastError());
    }
    else
    {
        AppendHostEvent(fixture, "close_requested", 0);
    }
    if (IsWindow(fixture.target))
    {
        SetTimer(fixture.target, kHostPollTimer, 100, nullptr);
    }
    UpdateStatus(fixture);
}

void PollHostProcess(Fixture& fixture)
{
    if (fixture.hostProcess == nullptr)
    {
        KillTimer(fixture.target, kHostPollTimer);
        return;
    }

    const DWORD wait = WaitForSingleObject(fixture.hostProcess, 0);
    if (wait == WAIT_OBJECT_0)
    {
        DWORD exitCode = 1;
        GetExitCodeProcess(fixture.hostProcess, &exitCode);
        AppendHostEvent(fixture, exitCode == 0 ? "stopped" : "failed", exitCode);
        CloseHostProcessHandle(fixture);
        fixture.hostState = exitCode == 0 ? HostState::Stopped : HostState::Failed;
        KillTimer(fixture.target, kHostPollTimer);
        UpdateStatus(fixture);
        return;
    }
    if (wait != WAIT_TIMEOUT)
    {
        if (fixture.hostState != HostState::Failed)
        {
            fixture.hostState = HostState::Failed;
            UpdateStatus(fixture);
        }
        return;
    }

    if (fixture.hostState == HostState::Starting && VerifyHostProbe(fixture))
    {
        AppendHostEvent(fixture, "ready", 0);
        fixture.hostState = HostState::Ready;
        UpdateStatus(fixture);
    }
}

std::wstring QuoteCommandArgument(std::wstring_view argument)
{
    std::wstring quoted = L"\"";
    for (const wchar_t character : argument)
    {
        if (character == L'"')
        {
            quoted += L'\\';
        }
        quoted += character;
    }
    quoted += L'"';
    return quoted;
}

void StartTestHost(Fixture& fixture)
{
    if (fixture.hostProcess != nullptr)
    {
        if (WaitForSingleObject(fixture.hostProcess, 0) != WAIT_OBJECT_0)
        {
            fixture.hostState = fixture.hostState == HostState::Ready ? HostState::Ready : HostState::Starting;
            UpdateStatus(fixture);
            return;
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(fixture.hostProcess, &exitCode);
        AppendHostEvent(fixture, exitCode == 0 ? "stopped" : "failed", exitCode);
        CloseHostProcessHandle(fixture);
    }

    fixture.hostStatusDetail.clear();
    std::wstring directory;
    if (!GetExecutableDirectory(directory))
    {
        fixture.hostState = HostState::Failed;
        fixture.hostStatusDetail = L"host path unavailable";
        UpdateStatus(fixture);
        return;
    }

    const std::wstring hostPath = directory + L"\\WindowSweaters.Host.exe";
    const DWORD attributes = GetFileAttributesW(hostPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        fixture.hostState = HostState::Failed;
        fixture.hostStatusDetail = L"host executable not found";
        AppendHostEvent(fixture, "host_not_found", GetLastError());
        UpdateStatus(fixture);
        return;
    }

    std::wostringstream target;
    target << L" --target " << HwndHexWide(fixture.target) << L" --test-mode --probe-output "
           << QuoteCommandArgument(fixture.hostProbePath);
    std::wstring commandLine = QuoteCommandArgument(hostPath) + target.str();
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            hostPath.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            directory.c_str(),
            &startup,
            &process))
    {
        fixture.hostState = HostState::Failed;
        const DWORD error = GetLastError();
        fixture.hostStatusDetail = L"host launch failed";
        AppendHostEvent(fixture, "create_process_failed", error);
        UpdateStatus(fixture);
        return;
    }

    CloseHandle(process.hThread);
    fixture.hostProcess = process.hProcess;
    fixture.hostProcessId = process.dwProcessId;
    fixture.hostWindow = nullptr;
    fixture.overlayWindow = nullptr;
    fixture.hostState = HostState::Starting;
    fixture.hostStatusDetail.clear();
    AppendHostEvent(fixture, "started", 0);
    SetTimer(fixture.target, kHostPollTimer, 250, nullptr);
    UpdateStatus(fixture);
}

LRESULT CALLBACK FixtureControlSubclassProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR data)
{
    auto* fixture = reinterpret_cast<Fixture*>(data);
    if (fixture != nullptr)
    {
        RecordMessage(*fixture, window, message, wParam, lParam);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool CreateFixtureControls(Fixture& fixture, HWND parent, HINSTANCE instance)
{
    fixture.startHostButton = CreateWindowExW(
        0, L"BUTTON", L"Start test host",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        16, 12, 140, 34, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartHostControl)), instance, nullptr);
    fixture.stopHostButton = CreateWindowExW(
        0, L"BUTTON", L"Stop test host",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        166, 12, 140, 34, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopHostControl)), instance, nullptr);
    fixture.runLifecycleButton = CreateWindowExW(
        0, L"BUTTON", L"Run lifecycle checks",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        316, 12, 174, 34, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRunLifecycleControl)), instance, nullptr);
    fixture.openSettingsButton = CreateWindowExW(
        0, L"BUTTON", L"Open native settings",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        500, 12, 170, 34, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenSettingsControl)), instance, nullptr);
    fixture.statusText = CreateWindowExW(
        0, L"STATIC", L"Host: stopped | lifecycle: idle",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        680, 12, 204, 34, parent, nullptr, instance, nullptr);
    fixture.evidenceText = CreateWindowExW(
        0, L"STATIC", L"Fixture received: clicks 0 | wheel 0 | keyboard 0 | focus changes 0",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 52, 440, 24, parent, nullptr, instance, nullptr);
    fixture.settingsStatusText = CreateWindowExW(
        0, L"STATIC", L"Settings: idle",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        466, 52, 418, 24, parent, nullptr, instance, nullptr);
    fixture.lifecycleText = CreateWindowExW(
        0, L"STATIC", L"Lifecycle: idle",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 78, 870, 24, parent, nullptr, instance, nullptr);
    fixture.keyboardLabel = CreateWindowExW(
        0, L"STATIC", L"Keyboard input",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 110, 360, 22, parent, nullptr, instance, nullptr);
    fixture.keyboardEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"Click here, then type a short test string.",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        16, 134, 360, 30, parent, nullptr, instance, nullptr);
    fixture.scrollLabel = CreateWindowExW(
        0, L"STATIC", L"Scrollable fixture list",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 176, 360, 22, parent, nullptr, instance, nullptr);
    fixture.scrollList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        16, 200, 360, 320, parent, nullptr, instance, nullptr);
    fixture.interactionHelp = CreateWindowExW(
        0,
        L"STATIC",
        L"Test only this fixture window.\r\n"
        L"Start the host, then use Run lifecycle checks or Open native settings.\r\n"
        L"Move, resize, minimize, and restore operate only on this fixture.\r\n"
        L"Settings opens only through the fixture-owned test host.\r\n"
        L"Host snapshots record target identity, bounds, counters, and visibility.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        408, 110, 456, 150, parent, nullptr, instance, nullptr);
    if (fixture.startHostButton == nullptr || fixture.stopHostButton == nullptr ||
        fixture.runLifecycleButton == nullptr || fixture.openSettingsButton == nullptr ||
        fixture.statusText == nullptr || fixture.evidenceText == nullptr ||
        fixture.settingsStatusText == nullptr || fixture.lifecycleText == nullptr ||
        fixture.keyboardLabel == nullptr || fixture.keyboardEdit == nullptr ||
        fixture.scrollLabel == nullptr || fixture.scrollList == nullptr ||
        fixture.interactionHelp == nullptr)
    {
        return false;
    }

    if (!SetWindowSubclass(fixture.keyboardEdit, FixtureControlSubclassProc, kInputSubclassId, reinterpret_cast<DWORD_PTR>(&fixture)) ||
        !SetWindowSubclass(fixture.scrollList, FixtureControlSubclassProc, kInputSubclassId, reinterpret_cast<DWORD_PTR>(&fixture)))
    {
        return false;
    }

    for (int item = 1; item <= 32; ++item)
    {
        std::wostringstream text;
        text << L"Scroll item " << (item < 10 ? L"0" : L"") << item << L" — fixture-only input evidence";
        if (SendMessageW(fixture.scrollList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.str().c_str())) == LB_ERR)
        {
            return false;
        }
    }

    std::ostringstream controlsCreated;
    controlsCreated << "{\"type\":\"fixture.controls_created\",\"pid\":" << GetCurrentProcessId()
                    << ",\"target_hwnd\":" << HwndHex(parent)
                    << ",\"start_host_hwnd\":" << HwndHex(fixture.startHostButton)
                    << ",\"stop_host_hwnd\":" << HwndHex(fixture.stopHostButton)
                    << ",\"run_lifecycle_hwnd\":" << HwndHex(fixture.runLifecycleButton)
                    << ",\"open_settings_hwnd\":" << HwndHex(fixture.openSettingsButton)
                    << ",\"keyboard_label_hwnd\":" << HwndHex(fixture.keyboardLabel)
                    << ",\"keyboard_input_hwnd\":" << HwndHex(fixture.keyboardEdit)
                    << ",\"scroll_label_hwnd\":" << HwndHex(fixture.scrollLabel)
                    << ",\"scroll_list_hwnd\":" << HwndHex(fixture.scrollList)
                    << ",\"status_hwnd\":" << HwndHex(fixture.statusText)
                    << ",\"evidence_hwnd\":" << HwndHex(fixture.evidenceText)
                    << ",\"settings_status_hwnd\":" << HwndHex(fixture.settingsStatusText)
                    << ",\"lifecycle_status_hwnd\":" << HwndHex(fixture.lifecycleText)
                    << ",\"help_hwnd\":" << HwndHex(fixture.interactionHelp) << '}';
    AppendResult(fixture, controlsCreated.str());
    UpdateStatus(fixture);
    return true;
}

bool ExposeFixtureControls(Fixture& fixture)
{
    struct Control
    {
        const char* name;
        HWND window;
        const wchar_t* expectedClass;
        const wchar_t* expectedTitle;
    };
    const Control controls[] = {
        { "start_host", fixture.startHostButton, L"Button", L"Start test host" },
        { "stop_host", fixture.stopHostButton, L"Button", L"Stop test host" },
        { "run_lifecycle", fixture.runLifecycleButton, L"Button", L"Run lifecycle checks" },
        { "open_settings", fixture.openSettingsButton, L"Button", L"Open native settings" },
        { "keyboard_label", fixture.keyboardLabel, L"Static", L"Keyboard input" },
        { "keyboard_input", fixture.keyboardEdit, L"Edit", L"Click here, then type a short test string." },
        { "scroll_label", fixture.scrollLabel, L"Static", L"Scrollable fixture list" },
        { "scroll_list", fixture.scrollList, L"ListBox", L"" },
        { "status", fixture.statusText, L"Static", L"" },
        { "counters", fixture.evidenceText, L"Static", L"" },
        { "settings_status", fixture.settingsStatusText, L"Static", L"" },
        { "lifecycle_status", fixture.lifecycleText, L"Static", L"" },
        { "help", fixture.interactionHelp, L"Static", L"" }
    };

    RECT parentClient{};
    if (fixture.target == nullptr || !IsWindow(fixture.target) ||
        GetParent(fixture.target) != nullptr ||
        !GetClientRect(fixture.target, &parentClient) ||
        !IsWindowVisible(fixture.target))
    {
        return false;
    }

    size_t visibleControls = 0;
    size_t externalOcclusions = 0;
    bool valid = true;
    for (const Control& control : controls)
    {
        DWORD ownerProcessId = 0;
        wchar_t className[128]{};
        wchar_t title[256]{};
        RECT screenBounds{};
        POINT topLeft{};
        POINT bottomRight{};
        const bool exists = control.window != nullptr && IsWindow(control.window);
        if (exists)
        {
            GetWindowThreadProcessId(control.window, &ownerProcessId);
            GetClassNameW(control.window, className, static_cast<int>(std::size(className)));
            GetWindowTextW(control.window, title, static_cast<int>(std::size(title)));
        }
        const bool owned = exists && ownerProcessId == GetCurrentProcessId() &&
            GetParent(control.window) == fixture.target;
        const bool classMatches = exists && std::wstring_view(className) == control.expectedClass;
        const bool titleMatches = control.expectedTitle[0] == L'\0' ||
            (exists && std::wstring_view(title) == control.expectedTitle);

        if (owned && classMatches && titleMatches)
        {
            ShowWindow(control.window, SW_SHOWNOACTIVATE);
            RedrawWindow(control.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }

        const bool shown = owned && IsWindowVisible(control.window);
        bool insideParent = false;
        bool hitIsControl = false;
        DWORD hitProcessId = 0;
        HWND hitWindow = nullptr;
        wchar_t hitClass[128]{};
        if (shown && GetWindowRect(control.window, &screenBounds))
        {
            topLeft = { screenBounds.left, screenBounds.top };
            bottomRight = { screenBounds.right, screenBounds.bottom };
            ScreenToClient(fixture.target, &topLeft);
            ScreenToClient(fixture.target, &bottomRight);
            insideParent = topLeft.x >= parentClient.left &&
                topLeft.y >= parentClient.top &&
                bottomRight.x <= parentClient.right &&
                bottomRight.y <= parentClient.bottom;

            POINT center{ (screenBounds.left + screenBounds.right) / 2, (screenBounds.top + screenBounds.bottom) / 2 };
            hitWindow = WindowFromPoint(center);
            if (hitWindow != nullptr)
            {
                GetWindowThreadProcessId(hitWindow, &hitProcessId);
                GetClassNameW(hitWindow, hitClass, static_cast<int>(std::size(hitClass)));
            }
            hitIsControl = hitWindow == control.window;
            if (hitWindow != nullptr && hitProcessId != GetCurrentProcessId())
            {
                ++externalOcclusions;
            }
        }

        const bool controlValid = shown && classMatches && titleMatches && insideParent;
        valid = controlValid && valid;
        if (controlValid)
        {
            ++visibleControls;
        }

        std::ostringstream json;
        json << "{\"type\":\"fixture.control_state\",\"name\":\"" << control.name
             << "\",\"hwnd\":" << HwndHex(control.window)
             << ",\"owner_pid\":" << ownerProcessId
             << ",\"parent_hwnd\":" << HwndHex(GetParent(control.window))
             << ",\"class\":" << JsonString(className)
             << ",\"title\":" << JsonString(title)
             << ",\"created\":" << (exists ? "true" : "false")
             << ",\"owned_child\":" << (owned ? "true" : "false")
             << ",\"class_matches\":" << (classMatches ? "true" : "false")
             << ",\"title_matches\":" << (titleMatches ? "true" : "false")
             << ",\"is_window_visible\":" << (shown ? "true" : "false")
             << ",\"inside_parent_client\":" << (insideParent ? "true" : "false")
             << ",\"covered_by_other_process\":" <<
                (hitWindow != nullptr && hitProcessId != GetCurrentProcessId() ? "true" : "false")
             << ",\"screen_bounds\":" << (shown ? JsonRect(screenBounds) : "null")
             << ",\"hit_hwnd\":" << HwndHex(hitWindow)
             << ",\"hit_pid\":" << hitProcessId
             << ",\"hit_class\":" << JsonString(hitClass)
             << ",\"hit_is_control\":" << (hitIsControl ? "true" : "false")
             << '}';
        AppendResult(fixture, json.str());
    }

    std::ostringstream ready;
    ready << "{\"type\":\"fixture.controls_ready\",\"pid\":" << GetCurrentProcessId()
          << ",\"target_hwnd\":" << HwndHex(fixture.target)
          << ",\"child_controls_valid\":" << (valid ? "true" : "false")
          << ",\"is_window_visible_child_count\":" << visibleControls
          << ",\"expected_control_count\":" << std::size(controls)
          << ",\"external_window_coverage_samples\":" << externalOcclusions
          << ",\"start_host_hwnd\":" << HwndHex(fixture.startHostButton)
          << ",\"stop_host_hwnd\":" << HwndHex(fixture.stopHostButton)
          << ",\"run_lifecycle_hwnd\":" << HwndHex(fixture.runLifecycleButton)
          << ",\"open_settings_hwnd\":" << HwndHex(fixture.openSettingsButton)
          << ",\"keyboard_input_hwnd\":" << HwndHex(fixture.keyboardEdit)
          << ",\"scroll_list_hwnd\":" << HwndHex(fixture.scrollList)
          << ",\"status_hwnd\":" << HwndHex(fixture.statusText)
          << ",\"evidence_hwnd\":" << HwndHex(fixture.evidenceText)
          << ",\"settings_status_hwnd\":" << HwndHex(fixture.settingsStatusText)
          << ",\"lifecycle_status_hwnd\":" << HwndHex(fixture.lifecycleText) << '}';
    AppendResult(fixture, ready.str());
    return valid && visibleControls == std::size(controls);
}

bool VerifyIdleMessageStatusInvariant(Fixture& fixture)
{
    const size_t updateCallsBefore = fixture.statusUpdateCalls;
    const size_t statusWritesBefore = fixture.statusTextWrites;
    const size_t evidenceWritesBefore = fixture.evidenceTextWrites;
    const size_t settingsWritesBefore = fixture.settingsStatusWrites;
    const size_t lifecycleWritesBefore = fixture.lifecycleTextWrites;

    wchar_t title[256]{};
    SendMessageW(fixture.target, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(fixture.target, WM_GETTEXT, std::size(title), reinterpret_cast<LPARAM>(title));

    RECT targetBounds{};
    const bool haveBounds = GetWindowRect(fixture.target, &targetBounds) != FALSE;
    const LPARAM hitTestPoint = haveBounds
        ? MAKELPARAM(static_cast<SHORT>(targetBounds.left + 8), static_cast<SHORT>(targetBounds.top + 8))
        : 0;
    SendMessageW(fixture.target, WM_NCHITTEST, 0, hitTestPoint);
    SendMessageW(fixture.target, WM_PAINT, 0, 0);

    HDC dc = GetDC(fixture.statusText);
    if (dc != nullptr)
    {
        SendMessageW(
            fixture.target,
            WM_CTLCOLORSTATIC,
            reinterpret_cast<WPARAM>(dc),
            reinterpret_cast<LPARAM>(fixture.statusText));
        ReleaseDC(fixture.statusText, dc);
    }

    const bool passed = dc != nullptr &&
        fixture.statusUpdateCalls == updateCallsBefore &&
        fixture.statusTextWrites == statusWritesBefore &&
        fixture.evidenceTextWrites == evidenceWritesBefore &&
        fixture.settingsStatusWrites == settingsWritesBefore &&
        fixture.lifecycleTextWrites == lifecycleWritesBefore;
    std::ostringstream json;
    json << "{\"type\":\"fixture.idle_query_invariant\",\"messages\":[\"WM_GETTEXTLENGTH\","
         << "\"WM_GETTEXT\",\"WM_NCHITTEST\",\"WM_PAINT\",\"WM_CTLCOLORSTATIC\"]"
         << ",\"update_call_delta\":" << fixture.statusUpdateCalls - updateCallsBefore
         << ",\"status_write_delta\":" << fixture.statusTextWrites - statusWritesBefore
         << ",\"counter_write_delta\":" << fixture.evidenceTextWrites - evidenceWritesBefore
         << ",\"settings_write_delta\":" << fixture.settingsStatusWrites - settingsWritesBefore
         << ",\"lifecycle_write_delta\":" << fixture.lifecycleTextWrites - lifecycleWritesBefore
         << ",\"device_context_acquired\":" << (dc != nullptr ? "true" : "false")
         << ",\"passed\":" << (passed ? "true" : "false") << '}';
    AppendResult(fixture, json.str());
    return passed;
}

std::string DpiAwarenessName()
{
    const DPI_AWARENESS_CONTEXT context = GetThreadDpiAwarenessContext();
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return "per-monitor-v2";
    }
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
    {
        return "per-monitor";
    }
    if (AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
    {
        return "system";
    }
    return "unaware";
}

bool WriteReadyRecord(Fixture& fixture)
{
    RECT targetBounds{};
    RECT receiverBounds{};
    RECT client{};
    POINT clientOrigin{};
    if (!GetWindowRect(fixture.target, &targetBounds) ||
        !GetWindowRect(fixture.receiver, &receiverBounds) ||
        !GetClientRect(fixture.target, &client))
    {
        return false;
    }

    clientOrigin = { client.left, client.top };
    if (!ClientToScreen(fixture.target, &clientOrigin))
    {
        return false;
    }

    POINT targetCenter{
        clientOrigin.x + (client.right - client.left) / 2,
        clientOrigin.y + (client.bottom - client.top) / 2
    };
    POINT ringTop{
        targetBounds.left + (targetBounds.right - targetBounds.left) / 2,
        targetBounds.top - kBorderWidth / 2
    };

    std::ostringstream json;
    json << "{\"type\":\"fixture.ready\",\"schema_version\":1"
         << ",\"pid\":" << GetCurrentProcessId()
         << ",\"target_hwnd\":" << HwndHex(fixture.target)
         << ",\"target_class\":" << JsonString(kTargetClass)
         << ",\"target_title\":" << JsonString(kTargetTitle)
         << ",\"target_bounds\":" << JsonRect(targetBounds)
         << ",\"target_client_bounds\":[" << clientOrigin.x << ',' << clientOrigin.y << ','
         << clientOrigin.x + client.right - client.left << ','
         << clientOrigin.y + client.bottom - client.top << ']'
         << ",\"receiver_hwnd\":" << HwndHex(fixture.receiver)
         << ",\"receiver_class\":" << JsonString(kReceiverClass)
         << ",\"receiver_title\":" << JsonString(kReceiverTitle)
         << ",\"receiver_bounds\":" << JsonRect(receiverBounds)
         << ",\"dpi_awareness\":" << JsonString(DpiAwarenessName())
         << ",\"dpi\":" << GetDpiForWindow(fixture.target)
         << ",\"result_file\":" << JsonString(fixture.resultFilePath)
         << ",\"host_probe_file\":" << JsonString(fixture.hostProbePath)
         << ",\"host_sibling\":\"WindowSweaters.Host.exe\""
         << ",\"border_width_pixels\":" << kBorderWidth
         << ",\"expected_border_rgb\":[64,190,220]"
         << ",\"expected_target_rgb\":[37,59,79]"
         << ",\"expected_receiver_rgb\":[34,42,54]"
         << ",\"controls\":{\"start_host\":\"Start test host\",\"stop_host\":\"Stop test host\","
         << "\"run_lifecycle\":\"Run lifecycle checks\","
         << "\"open_settings\":\"Open native settings\","
         << "\"keyboard_input\":\"Keyboard input\",\"scroll_list\":\"Scrollable fixture list\","
         << "\"status\":\"Host and lifecycle state\",\"lifecycle_status\":\"Lifecycle event and evidence path\","
         << "\"settings_status\":\"Settings launch status\","
         << "\"counters\":\"Fixture received: clicks | wheel | keyboard | focus changes\"}"
         << ",\"points\":{\"target_center\":" << JsonPoint(targetCenter)
         << ",\"opaque_ring_top\":" << JsonPoint(ringTop) << "}}";
    return AppendResult(fixture, json.str());
}

void PaintWindow(Fixture& fixture, HWND window)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    FillRect(dc, &paint.rcPaint, window == fixture.target ? fixture.targetBrush : fixture.receiverBrush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(242, 246, 250));

    RECT client{};
    GetClientRect(window, &client);
    if (IsTargetOrChild(fixture, window))
    {
        RECT title{ 408, 74, 864, 108 };
        DrawTextW(dc, L"Window Sweaters interaction fixture", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    else
    {
        RECT details{ 10, 10, client.right - 10, 36 };
        DrawTextW(dc, L"Fixture-only hit receiver behind the test target", -1, &details, DT_LEFT | DT_SINGLELINE);
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK FixtureWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Fixture* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        fixture = static_cast<Fixture*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fixture));
        const std::wstring_view className = !IS_INTRESOURCE(create->lpszClass)
            ? std::wstring_view(create->lpszClass)
            : std::wstring_view{};
        if (className == kTargetClass)
        {
            fixture->target = window;
        }
        else if (className == kReceiverClass)
        {
            fixture->receiver = window;
        }
    }

    if (fixture != nullptr)
    {
        RecordMessage(*fixture, window, message, wParam, lParam);
    }

    if (message == WM_MOUSEACTIVATE && fixture != nullptr && window == fixture->receiver)
    {
        return MA_NOACTIVATE;
    }

    switch (message)
    {
    case WM_CREATE:
        if (fixture != nullptr && window == fixture->target &&
            !CreateFixtureControls(*fixture, window, reinterpret_cast<const CREATESTRUCTW*>(lParam)->hInstance))
        {
            return -1;
        }
        break;
    case WM_COMMAND:
        if (fixture != nullptr && window == fixture->target && HIWORD(wParam) == BN_CLICKED)
        {
            const int controlId = LOWORD(wParam);
            const char* controlName = controlId == kStartHostControl ? "start-test-host" :
                controlId == kStopHostControl ? "stop-test-host" :
                controlId == kRunLifecycleControl ? "run-lifecycle-checks" :
                controlId == kOpenSettingsControl ? "open-native-settings" : "unknown";
            std::ostringstream json;
            json << "{\"type\":\"control.command\",\"control\":\"" << controlName
                 << "\",\"notification\":\"BN_CLICKED\",\"pid\":" << GetCurrentProcessId() << '}';
            AppendResult(*fixture, json.str());
            ++fixture->clickCount;
            if (controlId == kStartHostControl)
            {
                if (IsLifecycleRunning(*fixture))
                {
                    AppendLifecycleRecord(*fixture, "host_start_ignored", "host ownership remains stable during the lifecycle run", nullptr);
                }
                else
                {
                    StartTestHost(*fixture);
                }
            }
            else if (controlId == kStopHostControl)
            {
                if (IsLifecycleRunning(*fixture))
                {
                    AppendLifecycleRecord(*fixture, "host_stop_ignored", "host ownership remains stable during the lifecycle run", nullptr);
                }
                else
                {
                    RequestHostStop(*fixture);
                }
            }
            else if (controlId == kRunLifecycleControl)
            {
                StartLifecycleCheck(*fixture);
            }
            else if (controlId == kOpenSettingsControl)
            {
                RequestOwnedHostSettings(*fixture);
            }
            UpdateStatus(*fixture);
            return 0;
        }
        break;
    case WM_PAINT:
        if (fixture != nullptr)
        {
            PaintWindow(*fixture, window);
            return 0;
        }
        break;
    case WM_TIMER:
        if (fixture != nullptr && window == fixture->target && wParam == kAutoExitTimer)
        {
            KillTimer(window, kAutoExitTimer);
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        if (fixture != nullptr && window == fixture->target && wParam == kHostPollTimer)
        {
            PollHostProcess(*fixture);
            return 0;
        }
        if (fixture != nullptr && window == fixture->target && wParam == kLifecycleTimer)
        {
            AdvanceLifecycleCheck(*fixture);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (fixture != nullptr && window == fixture->target && IsLifecycleRunning(*fixture))
        {
            AppendLifecycleRecord(*fixture, "close_ignored", "target close was ignored until the lifecycle sequence finishes", nullptr);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (fixture != nullptr && window == fixture->target)
        {
            KillTimer(window, kAutoExitTimer);
            KillTimer(window, kHostPollTimer);
            KillTimer(window, kLifecycleTimer);
            RequestHostStop(*fixture);
            if (fixture->hostProcess != nullptr)
            {
                const DWORD wait = WaitForSingleObject(fixture->hostProcess, 3000);
                DWORD exitCode = 1;
                if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(fixture->hostProcess, &exitCode))
                {
                    AppendHostEvent(*fixture, exitCode == 0 ? "stopped" : "failed", exitCode);
                }
                else
                {
                    AppendHostEvent(*fixture, "stop_unconfirmed", wait);
                }
                CloseHostProcessHandle(*fixture);
            }
            if (fixture->receiver != nullptr && IsWindow(fixture->receiver))
            {
                DestroyWindow(fixture->receiver);
                fixture->receiver = nullptr;
            }
            PostQuitMessage(0);
        }
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool ParseArguments(Arguments& arguments, std::wstring& error)
{
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr)
    {
        error = L"Could not parse the command line.";
        return false;
    }

    bool valid = true;
    for (int index = 1; index < count && valid; ++index)
    {
        const std::wstring_view option(values[index]);
        if (option == L"--result-file" && index + 1 < count)
        {
            arguments.resultFile = values[++index];
        }
        else if (option == L"--auto-exit" && index + 1 < count)
        {
            wchar_t* end = nullptr;
            const unsigned long seconds = std::wcstoul(values[++index], &end, 10);
            if (end == values[index] || *end != L'\0' || seconds == 0 || seconds > 86400)
            {
                error = L"--auto-exit must be an integer from 1 to 86400 seconds.";
                valid = false;
            }
            else
            {
                arguments.autoExitSeconds = static_cast<UINT>(seconds);
            }
        }
        else if (option == L"--help" || option == L"-?")
        {
            error = L"usage: Fixture.exe [--result-file <path>] [--auto-exit <seconds>]";
            valid = false;
        }
        else
        {
            error = L"Unknown or incomplete option: ";
            error.append(option);
            valid = false;
        }
    }

    LocalFree(values);
    return valid;
}

bool RegisterFixtureClass(HINSTANCE instance, const wchar_t* className, HBRUSH background)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = FixtureWindowProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background;
    windowClass.lpszClassName = className;
    return RegisterClassExW(&windowClass) != 0;
}

void ShowError(const std::wstring& message)
{
    MessageBoxW(nullptr, message.c_str(), L"WindowSweaters Fixture", MB_OK | MB_ICONERROR);
}

int RunFixture(const Arguments& arguments)
{
    if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        ShowError(L"The fixture manifest did not enable Per-Monitor V2 DPI awareness.");
        return 3;
    }

    Fixture fixture;
    fixture.autoExitSeconds = arguments.autoExitSeconds;
    if (!EnsureEvidencePaths(fixture, arguments.resultFile))
    {
        ShowError(L"Could not prepare a fixture-owned evidence path.");
        return 2;
    }
    fixture.resultFile = CreateFileW(
            fixture.resultFilePath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    if (fixture.resultFile == INVALID_HANDLE_VALUE)
    {
        ShowError(L"Could not create the requested fixture evidence file.");
        return 2;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    fixture.targetBrush = CreateSolidBrush(kTargetColor);
    fixture.receiverBrush = CreateSolidBrush(kReceiverColor);
    if (fixture.targetBrush == nullptr || fixture.receiverBrush == nullptr ||
        !RegisterFixtureClass(instance, kReceiverClass, fixture.receiverBrush) ||
        !RegisterFixtureClass(instance, kTargetClass, fixture.targetBrush))
    {
        ShowError(L"Could not initialize fixture window classes.");
        if (fixture.resultFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(fixture.resultFile);
        }
        if (fixture.targetBrush != nullptr)
        {
            DeleteObject(fixture.targetBrush);
        }
        if (fixture.receiverBrush != nullptr)
        {
            DeleteObject(fixture.receiverBrush);
        }
        return 2;
    }

    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
    {
        workArea = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }

    const UINT dpi = GetDpiForSystem();
    RECT targetClient{ 0, 0, 900, 560 };
    if (!AdjustWindowRectExForDpi(&targetClient, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi))
    {
        ShowError(L"Could not calculate the target window bounds.");
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 2;
    }
    const int targetWidth = targetClient.right - targetClient.left;
    const int targetHeight = targetClient.bottom - targetClient.top;
    constexpr int margin = 56;
    const int receiverWidth = targetWidth + margin * 2;
    const int receiverHeight = targetHeight + margin * 2;
    const int receiverX = workArea.left + ((workArea.right - workArea.left) - receiverWidth) / 2;
    const int receiverY = workArea.top + ((workArea.bottom - workArea.top) - receiverHeight) / 2;

    fixture.receiver = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kReceiverClass,
        kReceiverTitle,
        WS_POPUP,
        receiverX,
        receiverY,
        receiverWidth,
        receiverHeight,
        nullptr,
        nullptr,
        instance,
        &fixture);
    if (fixture.receiver == nullptr)
    {
        ShowError(L"Could not create the fixture-only exterior hit receiver.");
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 2;
    }
    ShowWindow(fixture.receiver, SW_SHOWNOACTIVATE);
    UpdateWindow(fixture.receiver);

    fixture.target = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kTargetClass,
        kTargetTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        receiverX + margin,
        receiverY + margin,
        targetWidth,
        targetHeight,
        nullptr,
        nullptr,
        instance,
        &fixture);
    if (fixture.target == nullptr)
    {
        DestroyWindow(fixture.receiver);
        ShowError(L"Could not create the fixture target window.");
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 2;
    }

    ShowWindow(fixture.target, SW_SHOWNOACTIVATE);
    if (!ExposeFixtureControls(fixture))
    {
        DestroyWindow(fixture.target);
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 2;
    }
    UpdateWindow(fixture.target);
    if (!VerifyIdleMessageStatusInvariant(fixture))
    {
        DestroyWindow(fixture.target);
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 4;
    }
    if (fixture.autoExitSeconds != 0 &&
        SetTimer(fixture.target, kAutoExitTimer, fixture.autoExitSeconds * 1000, nullptr) == 0)
    {
        ShowError(L"Could not start the fixture auto-exit timer.");
        DestroyWindow(fixture.target);
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 2;
    }

    if (!WriteReadyRecord(fixture))
    {
        ShowError(L"Could not write the fixture.ready record.");
        DestroyWindow(fixture.target);
        CloseHandle(fixture.resultFile);
        DeleteObject(fixture.targetBrush);
        DeleteObject(fixture.receiverBrush);
        return 2;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(fixture.target, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    AppendResult(fixture, "{\"type\":\"fixture.exit\",\"exit_code\":0}");
    if (fixture.resultFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(fixture.resultFile);
    }
    DeleteObject(fixture.targetBrush);
    DeleteObject(fixture.receiverBrush);
    return static_cast<int>(message.wParam);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Arguments arguments;
    std::wstring error;
    if (!ParseArguments(arguments, error))
    {
        ShowError(error);
        return 2;
    }
    return RunFixture(arguments);
}
