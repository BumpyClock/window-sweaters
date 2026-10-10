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
constexpr UINT_PTR kInputSubclassId = 1;
constexpr int kStartHostControl = 101;
constexpr int kStopHostControl = 102;
constexpr COLORREF kTargetColor = RGB(37, 59, 79);
constexpr COLORREF kReceiverColor = RGB(34, 42, 54);
constexpr int kBorderWidth = 12;

enum class HostState
{
    Stopped,
    Starting,
    Ready,
    Stopping,
    Failed
};

struct Fixture
{
    HANDLE resultFile = INVALID_HANDLE_VALUE;
    HANDLE hostProcess = nullptr;
    HWND target = nullptr;
    HWND receiver = nullptr;
    HWND startHostButton = nullptr;
    HWND stopHostButton = nullptr;
    HWND statusText = nullptr;
    HWND evidenceText = nullptr;
    HWND keyboardEdit = nullptr;
    HWND scrollList = nullptr;
    HWND hostWindow = nullptr;
    HWND overlayWindow = nullptr;
    DWORD hostProcessId = 0;
    HBRUSH targetBrush = nullptr;
    HBRUSH receiverBrush = nullptr;
    UINT autoExitSeconds = 0;
    HostState hostState = HostState::Stopped;
    std::wstring resultFilePath;
    std::wstring hostProbePath;
    size_t clickCount = 0;
    size_t wheelCount = 0;
    size_t keyboardCount = 0;
    size_t focusChangeCount = 0;
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
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    case WM_MOVE: return "WM_MOVE";
    case WM_NCHITTEST: return "WM_NCHITTEST";
    case WM_NCLBUTTONDOWN: return "WM_NCLBUTTONDOWN";
    case WM_NCLBUTTONUP: return "WM_NCLBUTTONUP";
    case WM_NCMOUSEMOVE: return "WM_NCMOUSEMOVE";
    case WM_PAINT: return "WM_PAINT";
    case WM_SETFOCUS: return "WM_SETFOCUS";
    case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
    case WM_SIZE: return "WM_SIZE";
    case WM_SIZING: return "WM_SIZING";
    case WM_SYSCOMMAND: return "WM_SYSCOMMAND";
    case WM_TIMER: return "WM_TIMER";
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
    text << L"Fixture received: clicks " << fixture.clickCount
         << L" | wheel " << fixture.wheelCount
         << L" | keyboard " << fixture.keyboardCount
         << L" | focus changes " << fixture.focusChangeCount
         << L" | host " << hostState;
    if (fixture.hostProcessId != 0)
    {
        text << L" (PID " << fixture.hostProcessId << L')';
    }
    if (fixture.hostState == HostState::Ready && fixture.overlayWindow != nullptr)
    {
        text << L" | overlay " << HwndHexWide(fixture.overlayWindow);
    }
    SetWindowTextW(fixture.statusText, text.str().c_str());
}

void RecordMessage(Fixture& fixture, HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (IsTargetOrChild(fixture, window))
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
            ++fixture.clickCount;
            break;
        case WM_MOUSEWHEEL:
            ++fixture.wheelCount;
            break;
        case WM_KEYDOWN:
        case WM_CHAR:
            ++fixture.keyboardCount;
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            ++fixture.focusChangeCount;
            break;
        }
    }

    LogMessage(fixture, window, message, wParam, lParam);
    UpdateStatus(fixture);
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
    std::string json;
    if (!ReadSmallFile(fixture.hostProbePath, json))
    {
        return false;
    }

    uint64_t processId = 0;
    uint64_t hostWindow = 0;
    uint64_t overlayWindow = 0;
    uint64_t targetWindow = 0;
    if (!ReadJsonNumber(json, "host_pid", processId) ||
        !ReadJsonNumber(json, "host_hwnd", hostWindow) ||
        !ReadJsonNumber(json, "overlay_hwnd", overlayWindow) ||
        !ReadJsonNumber(json, "target_hwnd", targetWindow) ||
        processId != fixture.hostProcessId ||
        targetWindow != reinterpret_cast<uintptr_t>(fixture.target))
    {
        return false;
    }

    const HWND host = reinterpret_cast<HWND>(static_cast<uintptr_t>(hostWindow));
    const HWND overlay = reinterpret_cast<HWND>(static_cast<uintptr_t>(overlayWindow));
    DWORD hostOwner = 0;
    DWORD overlayOwner = 0;
    wchar_t hostClass[128]{};
    wchar_t overlayClass[128]{};
    if (!IsWindow(host) || !IsWindow(overlay) ||
        GetWindowThreadProcessId(host, &hostOwner) == 0 ||
        GetWindowThreadProcessId(overlay, &overlayOwner) == 0 ||
        hostOwner != fixture.hostProcessId || overlayOwner != fixture.hostProcessId)
    {
        return false;
    }

    GetClassNameW(host, hostClass, static_cast<int>(std::size(hostClass)));
    GetClassNameW(overlay, overlayClass, static_cast<int>(std::size(overlayClass)));
    if (std::wstring_view(hostClass) != L"WindowSweaters.Host" ||
        std::wstring_view(overlayClass) != L"WindowSweaters.Overlay")
    {
        return false;
    }

    fixture.hostWindow = host;
    fixture.overlayWindow = overlay;
    fixture.hostState = HostState::Ready;
    return true;
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
        fixture.hostState = HostState::Failed;
        UpdateStatus(fixture);
        return;
    }

    if (fixture.hostState == HostState::Starting && VerifyHostProbe(fixture))
    {
        AppendHostEvent(fixture, "ready", 0);
        fixture.hostState = HostState::Ready;
    }
    UpdateStatus(fixture);
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

    std::wstring directory;
    if (!GetExecutableDirectory(directory))
    {
        fixture.hostState = HostState::Failed;
        UpdateStatus(fixture);
        return;
    }

    const std::wstring hostPath = directory + L"\\WindowSweaters.Host.exe";
    const DWORD attributes = GetFileAttributesW(hostPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        fixture.hostState = HostState::Failed;
        SetWindowTextW(fixture.statusText, L"Host not found beside Fixture.exe. Build both x64 projects first.");
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
        SetWindowTextW(fixture.statusText, L"Could not start the sibling test host. Check the Host build and Windows error.");
        AppendHostEvent(fixture, "create_process_failed", GetLastError());
        return;
    }

    CloseHandle(process.hThread);
    fixture.hostProcess = process.hProcess;
    fixture.hostProcessId = process.dwProcessId;
    fixture.hostWindow = nullptr;
    fixture.overlayWindow = nullptr;
    fixture.hostState = HostState::Starting;
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
        16, 12, 150, 34, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartHostControl)), instance, nullptr);
    fixture.stopHostButton = CreateWindowExW(
        0, L"BUTTON", L"Stop test host",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        176, 12, 150, 34, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopHostControl)), instance, nullptr);
    fixture.statusText = CreateWindowExW(
        0, L"STATIC", L"Fixture received: clicks 0 | wheel 0 | keyboard 0 | focus changes 0 | host stopped",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        344, 12, 540, 34, parent, nullptr, instance, nullptr);
    fixture.evidenceText = CreateWindowExW(
        0, L"STATIC", L"Use the visible test window. The target and its ring receiver are fixture-owned.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 52, 870, 26, parent, nullptr, instance, nullptr);
    CreateWindowExW(
        0, L"STATIC", L"Keyboard input",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 84, 360, 22, parent, nullptr, instance, nullptr);
    fixture.keyboardEdit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"Click here, then type a short test string.",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        16, 108, 360, 30, parent, nullptr, instance, nullptr);
    CreateWindowExW(
        0, L"STATIC", L"Scrollable fixture list",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        16, 150, 360, 22, parent, nullptr, instance, nullptr);
    fixture.scrollList = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        16, 174, 360, 360, parent, nullptr, instance, nullptr);
    HWND interactionHelp = CreateWindowExW(
        0,
        L"STATIC",
        L"Test only this fixture window.\r\n"
        L"Click in the open center to check pass-through.\r\n"
        L"Scroll the list and type in the field to record input.\r\n"
        L"Use the regular caption buttons and outer resize frame.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        408, 112, 456, 110, parent, nullptr, instance, nullptr);
    if (fixture.startHostButton == nullptr || fixture.stopHostButton == nullptr ||
        fixture.statusText == nullptr || fixture.evidenceText == nullptr ||
        fixture.keyboardEdit == nullptr || fixture.scrollList == nullptr ||
        interactionHelp == nullptr)
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

    SetWindowTextW(fixture.evidenceText, fixture.resultFilePath.c_str());
    std::ostringstream controlsReady;
    controlsReady << "{\"type\":\"fixture.controls_ready\",\"pid\":" << GetCurrentProcessId()
                  << ",\"target_hwnd\":" << HwndHex(parent)
                  << ",\"start_host_hwnd\":" << HwndHex(fixture.startHostButton)
                  << ",\"stop_host_hwnd\":" << HwndHex(fixture.stopHostButton)
                  << ",\"keyboard_input_hwnd\":" << HwndHex(fixture.keyboardEdit)
                  << ",\"scroll_list_hwnd\":" << HwndHex(fixture.scrollList)
                  << ",\"status_hwnd\":" << HwndHex(fixture.statusText) << '}';
    AppendResult(fixture, controlsReady.str());
    UpdateStatus(fixture);
    return true;
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
         << "\"keyboard_input\":\"Keyboard input\",\"scroll_list\":\"Scrollable fixture list\","
         << "\"status\":\"Fixture received: clicks | wheel | keyboard | focus changes | host state\"}"
         << ",\"points\":{\"target_center\":" << JsonPoint(targetCenter)
         << ",\"opaque_ring_top\":" << JsonPoint(ringTop) << "}}";
    return AppendResult(fixture, json.str());
}

void PaintWindow(Fixture& fixture, HWND window)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
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
                controlId == kStopHostControl ? "stop-test-host" : "unknown";
            std::ostringstream json;
            json << "{\"type\":\"control.command\",\"control\":\"" << controlName
                 << "\",\"notification\":\"BN_CLICKED\",\"pid\":" << GetCurrentProcessId() << '}';
            AppendResult(*fixture, json.str());
            ++fixture->clickCount;
            if (controlId == kStartHostControl)
            {
                StartTestHost(*fixture);
            }
            else if (controlId == kStopHostControl)
            {
                RequestHostStop(*fixture);
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
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (fixture != nullptr && window == fixture->target)
        {
            KillTimer(window, kAutoExitTimer);
            KillTimer(window, kHostPollTimer);
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
        0,
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
    UpdateWindow(fixture.target);
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
        TranslateMessage(&message);
        DispatchMessageW(&message);
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
