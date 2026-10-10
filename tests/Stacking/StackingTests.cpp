// SPDX-License-Identifier: GPL-3.0-only
#include "..\..\src\Rendering\Stacking.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace window_sweaters::rendering;

namespace {
constexpr wchar_t WindowClass[] = L"WindowSweaters.StackingTests";
constexpr UINT PeerBand = WM_APP + 1;
constexpr UINT PeerStop = WM_APP + 2;
constexpr UINT PositionFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
constexpr DWORD OverlayStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
    WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
int checks{};

void Check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string(message));
}

class Handle final {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
private:
    HANDLE value_{};
};

struct PositionCount {
    unsigned changes{};
    bool refuseZOrder{};
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const auto count = reinterpret_cast<PositionCount*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_WINDOWPOSCHANGING && count) {
        ++count->changes;
        if (count->refuseZOrder) reinterpret_cast<WINDOWPOS*>(lparam)->flags |= SWP_NOZORDER;
    }
    if (message == PeerBand) {
        return SetWindowPos(window, wparam ? HWND_TOPMOST : HWND_NOTOPMOST,
                            0, 0, 0, 0, PositionFlags);
    }
    if (message == PeerStop) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void RegisterWindows() {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = WindowClass;
    Check(RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
          "register hidden fixture class");
}

class Window final {
public:
    explicit Window(DWORD extended = 0, HWND owner = nullptr, DWORD style = WS_OVERLAPPEDWINDOW) {
        window_ = CreateWindowExW(extended, WindowClass, L"Hidden stacking test", style,
                                 120, 120, 300, 200, owner, nullptr, GetModuleHandleW(nullptr), &count);
        Check(window_ != nullptr, "create only an owned fixture HWND");
    }
    ~Window() { if (window_) DestroyWindow(window_); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    HWND Get() const { return window_; }
    PositionCount count;
private:
    HWND window_{};
};

class IsolatedDesktop final {
public:
    IsolatedDesktop() : name(L"WindowSweaters.StackingTests." + std::to_wstring(GetCurrentProcessId())),
        previous_(GetThreadDesktop(GetCurrentThreadId())) {
        desktop_ = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        Check(desktop_ != nullptr, "create private noninteractive test desktop");
        if (!SetThreadDesktop(desktop_)) {
            CloseDesktop(desktop_);
            desktop_ = nullptr;
            Check(false, "attach test thread to private desktop without switching the input desktop");
        }
    }
    ~IsolatedDesktop() {
        SetThreadDesktop(previous_);
        if (desktop_) CloseDesktop(desktop_);
    }
    std::wstring name;
private:
    HDESK previous_{};
    HDESK desktop_{};
};

struct PeerWindows { HWND target{}; HWND other{}; HWND dialog{}; };

int RunPeer(const wchar_t* name) {
    Handle mapping(OpenFileMappingW(FILE_MAP_WRITE, FALSE, name));
    Check(mapping.Get() != nullptr, "peer opens private handoff");
    auto windows = static_cast<PeerWindows*>(MapViewOfFile(mapping.Get(), FILE_MAP_WRITE, 0, 0, sizeof(PeerWindows)));
    Check(windows != nullptr, "peer maps private handoff");
    RegisterWindows();
    Window target;
    Window other;
    Window dialog(WS_EX_DLGMODALFRAME, target.Get(), WS_POPUP | WS_CAPTION);
    *windows = {target.Get(), other.Get(), dialog.Get()};
    UnmapViewOfFile(windows);
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(name) + L".ready").c_str()));
    Check(ready.Get() && SetEvent(ready.Get()), "peer signals actual hidden HWND creation");
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

class Peer final {
public:
    explicit Peer(const std::wstring& desktop) {
        const std::wstring name = L"Local\\WindowSweaters.StackingTests." + std::to_wstring(GetCurrentProcessId());
        Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(PeerWindows), name.c_str()));
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, (name + L".ready").c_str()));
        Check(mapping.Get() && ready.Get(), "create bounded native peer handoff");
        auto shared = static_cast<PeerWindows*>(MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, sizeof(PeerWindows)));
        Check(shared != nullptr, "map native peer handoff");
        std::array<wchar_t, 32768> executable{};
        Check(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) != 0,
              "locate own native test executable");
        std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --peer \"" + name + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        std::wstring desktopPath = L"WinSta0\\" + desktop;
        startup.lpDesktop = desktopPath.data();
        PROCESS_INFORMATION process{};
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Check(job_ && SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
              "bound fixture peer lifetime to the test");
        const bool launched = CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process) != FALSE;
        if (!launched) {
            UnmapViewOfFile(shared);
            CloseHandle(job_);
            job_ = nullptr;
            Check(false, "launch only our own hidden peer on the private desktop");
        }
        process_ = process.hProcess;
        Handle thread(process.hThread);
        if (!AssignProcessToJobObject(job_, process_) || ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            CloseHandle(job_);
            process_ = nullptr;
            job_ = nullptr;
            UnmapViewOfFile(shared);
            Check(false, "start bounded hidden peer");
        }
        const std::array waits{ready.Get(), process_};
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 10000);
        if (wait == WAIT_OBJECT_0) windows = *shared;
        UnmapViewOfFile(shared);
        if (wait != WAIT_OBJECT_0) {
            CloseHandle(job_);
            job_ = nullptr;
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
            Check(false, "native peer creates hidden fixture windows within deadline");
        }
        Check(IsWindow(windows.target) && IsWindow(windows.other) && IsWindow(windows.dialog),
              "peer handoff names live fixture windows");
    }
    ~Peer() {
        DWORD_PTR result{};
        SendMessageTimeoutW(windows.target, PeerStop, 0, 0, SMTO_ABORTIFHUNG, 1000, &result);
        WaitForSingleObject(process_, 2000);
        if (job_) CloseHandle(job_);
        if (process_) CloseHandle(process_);
    }
    void Band(bool topmost) {
        DWORD_PTR result{};
        Check(SendMessageTimeoutW(windows.target, PeerBand, topmost, 0, SMTO_ABORTIFHUNG, 2000, &result) && result,
              "only the native peer changes its own target band");
    }
    PeerWindows windows;
private:
    HANDLE process_{};
    HANDLE job_{};
};

std::vector<HWND> FixtureOrder(std::initializer_list<HWND> fixtureWindows) {
    std::vector<HWND> order;
    unsigned visited{};
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        Check(++visited < 10000, "native z-order traversal is bounded");
        if (std::find(fixtureWindows.begin(), fixtureWindows.end(), window) != fixtureWindows.end()) order.push_back(window);
    }
    Check(order.size() == fixtureWindows.size(), "native stack includes every fixture HWND exactly once");
    return order;
}

struct Facts {
    LONG_PTR style{};
    LONG_PTR extended{};
    HWND owner{};
    RECT bounds{};
};

Facts ReadFacts(HWND window) {
    Facts facts{GetWindowLongPtrW(window, GWL_STYLE), GetWindowLongPtrW(window, GWL_EXSTYLE),
                GetWindow(window, GW_OWNER), {}};
    Check(GetWindowRect(window, &facts.bounds) != FALSE, "read native fixture rectangle");
    return facts;
}

void Unchanged(HWND window, const Facts& before, bool allowBand = false) {
    const Facts after = ReadFacts(window);
    const LONG_PTR mask = allowBand ? ~static_cast<LONG_PTR>(WS_EX_TOPMOST) : ~LONG_PTR{};
    Check(after.style == before.style && (after.extended & mask) == (before.extended & mask) &&
          after.owner == before.owner && EqualRect(&after.bounds, &before.bounds),
          "placement preserves HWND styles, owner and geometry except its explicit overlay band");
}

void Placed(HWND overlay, HWND target) {
    Check(GetWindow(target, GW_HWNDNEXT) == overlay, "actual native z-order places overlay immediately below target");
    Check((GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) ==
          (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST), "actual native overlay band matches current target");
}

struct ModelWindow { char id; StackingBand band; };

void ReconcileModel(std::vector<ModelWindow>& stack, char overlayId, char targetId) {
    const auto find = [&stack](char id) {
        return std::find_if(stack.begin(), stack.end(), [id](const auto& window) { return window.id == id; });
    };
    const auto target = find(targetId);
    const auto overlay = find(overlayId);
    Check(target != stack.end() && overlay != stack.end(), "model target and overlay exist");
    const StackingSnapshot snapshot{target->band, overlay->band, target + 1 == overlay};
    const auto action = PlanStacking(snapshot);
    if (action == StackingAction::None) return;
    ModelWindow moved = *overlay;
    if (action == StackingAction::PromoteThenPlaceBelow) moved.band = StackingBand::Topmost;
    if (action == StackingAction::DemoteThenPlaceBelow) moved.band = StackingBand::Ordinary;
    stack.erase(overlay);
    stack.insert(find(targetId) + 1, moved);
}

std::string Ids(const std::vector<ModelWindow>& stack) {
    std::string result;
    for (const auto& window : stack) result += window.id;
    return result;
}

void ModelTests() {
    constexpr auto ordinary = StackingBand::Ordinary;
    constexpr auto topmost = StackingBand::Topmost;
    Check(PlanStacking({ordinary, ordinary, true}) == StackingAction::None, "settled ordinary pair does no work");
    Check(PlanStacking({topmost, topmost, true}) == StackingAction::None, "settled topmost pair does no work");
    Check(PlanStacking({ordinary, ordinary, false}) == StackingAction::PlaceBelow, "displaced ordinary pair reorders only overlay");
    Check(PlanStacking({topmost, topmost, false}) == StackingAction::PlaceBelow, "displaced topmost pair reorders only overlay");
    for (bool adjacent : {false, true}) {
        Check(PlanStacking({topmost, ordinary, adjacent}) == StackingAction::PromoteThenPlaceBelow,
              "band mismatch explicitly promotes even at a band boundary");
        Check(PlanStacking({ordinary, topmost, adjacent}) == StackingAction::DemoteThenPlaceBelow,
              "band mismatch explicitly demotes even at a band boundary");
    }
    std::vector<ModelWindow> stack{{'D', ordinary}, {'F', ordinary}, {'B', ordinary},
                                  {'A', ordinary}, {'a', ordinary}, {'b', ordinary}, {'R', ordinary}};
    ReconcileModel(stack, 'b', 'B');
    Check(Ids(stack) == "DFBbAaR", "background yarn remains below dialog, foreground and overlapping target");
    ReconcileModel(stack, 'a', 'A');
    Check(Ids(stack) == "DFBbAaR", "repeated reconciliation does not perturb an unchanged stack");
    stack = {{'B', topmost}, {'D', ordinary}, {'F', ordinary}, {'A', ordinary},
             {'a', ordinary}, {'b', ordinary}, {'R', ordinary}};
    ReconcileModel(stack, 'b', 'B');
    Check(Ids(stack) == "BbDFAaR" && stack[1].band == topmost && stack[4].band == ordinary,
          "one promoted target promotes only its own overlay");
    stack[0].band = ordinary;
    ReconcileModel(stack, 'b', 'B');
    Check(Ids(stack) == "BbDFAaR" && stack[1].band == ordinary, "demotion clears stale topmost yarn");
    for (int event = 0; event < 1000; ++event) {
        ReconcileModel(stack, 'a', 'A');
        ReconcileModel(stack, 'b', 'B');
    }
    Check(Ids(stack) == "BbDFAaR", "duplicate ordering notifications converge without stack oscillation");
}

void NativeTests() {
    IsolatedDesktop desktop;
    RegisterWindows();
    Peer peer(desktop.name);
    Window overlay(OverlayStyle, nullptr, WS_POPUP);
    Window otherOverlay(OverlayStyle, nullptr, WS_POPUP);
    Window background;
    Window ownedOverlay(OverlayStyle, background.Get(), WS_POPUP);
    Window child(0, background.Get(), WS_CHILD);

    const HWND target = peer.windows.target;
    const HWND other = peer.windows.other;
    const HWND dialog = peer.windows.dialog;
    const auto targetFacts = ReadFacts(target);
    const auto otherFacts = ReadFacts(other);
    const auto dialogFacts = ReadFacts(dialog);
    const auto overlayFacts = ReadFacts(overlay.Get());
    const auto foreignOrder = FixtureOrder({target, other, dialog});
    const HWND foreground = GetForegroundWindow();
    const HWND active = GetActiveWindow();
    Check(SUCCEEDED(PlaceOverlayBelow(overlay.Get(), target)), "ordinary native placement succeeds");
    Placed(overlay.Get(), target);
    Check(SUCCEEDED(PlaceOverlayBelow(otherOverlay.Get(), other)), "second overlapping pair places independently");
    Placed(otherOverlay.Get(), other);
    Check(FixtureOrder({target, other, dialog}) == foreignOrder, "ordinary placement leaves foreign relative order intact");
    Unchanged(target, targetFacts);
    Unchanged(other, otherFacts);
    Unchanged(dialog, dialogFacts);
    Unchanged(overlay.Get(), overlayFacts);

    const unsigned settled = overlay.count.changes;
    for (int i = 0; i < 100; ++i) Check(PlaceOverlayBelow(overlay.Get(), target) == S_OK, "repeated native placement is successful");
    Check(overlay.count.changes == settled, "settled native placement emits no new positioning notifications");
    Check(GetForegroundWindow() == foreground && GetActiveWindow() == active, "hidden placement never activates a window");

    peer.Band(true);
    const auto promotedTarget = ReadFacts(target);
    const auto promotedDialog = ReadFacts(dialog);
    const auto promotedOrder = FixtureOrder({target, other, dialog});
    Check(SUCCEEDED(PlaceOverlayBelow(overlay.Get(), target)), "overlay follows real native target promotion");
    Placed(overlay.Get(), target);
    Check(!(GetWindowLongPtrW(otherOverlay.Get(), GWL_EXSTYLE) & WS_EX_TOPMOST),
          "one target's promotion never promotes unrelated overlay");
    Check(FixtureOrder({target, other, dialog}) == promotedOrder, "promotion preserves dialog/foreign ordering");
    Unchanged(target, promotedTarget);
    Unchanged(dialog, promotedDialog);
    Unchanged(overlay.Get(), overlayFacts, true);

    const unsigned promotedSettled = overlay.count.changes;
    Check(PlaceOverlayBelow(overlay.Get(), target) == S_OK && overlay.count.changes == promotedSettled,
          "settled topmost placement emits no ordering feedback");
    peer.Band(false);
    const auto demotedTarget = ReadFacts(target);
    const auto demotedOrder = FixtureOrder({target, other, dialog});
    Check(SUCCEEDED(PlaceOverlayBelow(overlay.Get(), target)), "overlay follows real native target demotion");
    Placed(overlay.Get(), target);
    Check(FixtureOrder({target, other, dialog}) == demotedOrder, "demotion leaves foreign relative order intact");
    Unchanged(target, demotedTarget);
    Unchanged(overlay.Get(), overlayFacts);

    Check(SetWindowPos(overlay.Get(), HWND_BOTTOM, 0, 0, 0, 0, PositionFlags) != FALSE, "displace only our test overlay");
    Check(SUCCEEDED(PlaceOverlayBelow(overlay.Get(), target)), "native displaced overlay is repaired");
    Placed(overlay.Get(), target);
    const unsigned beforeInvalid = overlay.count.changes;
    const auto invalidOrder = FixtureOrder({target, other, dialog, overlay.Get(), otherOverlay.Get(), ownedOverlay.Get(), background.Get()});
    Check(PlaceOverlayBelow(other, target) == E_INVALIDARG, "foreign overlay candidate cannot be mutated");
    Check(PlaceOverlayBelow(ownedOverlay.Get(), target) == E_INVALIDARG, "owned overlay cannot change a foreign band through ownership");
    Check(PlaceOverlayBelow(overlay.Get(), dialog) == E_INVALIDARG, "owned/dialog target is unsupported");
    Check(PlaceOverlayBelow(overlay.Get(), child.Get()) == E_INVALIDARG, "child target is unsupported");
    Check(PlaceOverlayBelow(child.Get(), target) == E_INVALIDARG, "child overlay is unsupported");
    Check(PlaceOverlayBelow(overlay.Get(), overlay.Get()) == E_INVALIDARG, "self target is rejected");
    Check(PlaceOverlayBelow(nullptr, target) == E_INVALIDARG && PlaceOverlayBelow(overlay.Get(), nullptr) == E_INVALIDARG,
          "null HWNDs are rejected");
    Check(overlay.count.changes == beforeInvalid, "invalid requests cannot position the overlay");
    Check(FixtureOrder({target, other, dialog, overlay.Get(), otherOverlay.Get(), ownedOverlay.Get(), background.Get()}) == invalidOrder,
          "invalid requests leave every fixture top-level HWND untouched");
    Check(GetForegroundWindow() == foreground && GetActiveWindow() == active, "all band transitions remain nonactivating");
    HRESULT wrongThread{};
    std::thread wrongOwner([&] { wrongThread = PlaceOverlayBelow(overlay.Get(), target); });
    wrongOwner.join();
    Check(wrongThread == E_INVALIDARG && overlay.count.changes == beforeInvalid,
          "a nonowning thread cannot position the overlay");

    Window boundaryTarget;
    Window boundaryOverlay(OverlayStyle, nullptr, WS_POPUP);
    Check(PlaceOverlayBelow(boundaryOverlay.Get(), boundaryTarget.Get()) == S_OK, "establish ordinary band-boundary pair");
    Check(SetWindowPos(boundaryTarget.Get(), HWND_TOPMOST, 0, 0, 0, 0, PositionFlags) != FALSE,
          "promote only our native band-boundary target");
    Check(GetWindow(boundaryTarget.Get(), GW_HWNDNEXT) == boundaryOverlay.Get() &&
          !(GetWindowLongPtrW(boundaryOverlay.Get(), GWL_EXSTYLE) & WS_EX_TOPMOST),
          "adjacency alone does not prove the overlay is in the correct native band");
    Check(PlaceOverlayBelow(boundaryOverlay.Get(), boundaryTarget.Get()) == S_OK,
          "adjacent overlay still performs a required band transition");
    Placed(boundaryOverlay.Get(), boundaryTarget.Get());

    Window refusedOverlay(OverlayStyle, nullptr, WS_POPUP);
    refusedOverlay.count.refuseZOrder = true;
    Check(PlaceOverlayBelow(refusedOverlay.Get(), target) == HRESULT_FROM_WIN32(ERROR_RETRY),
          "successful SetWindowPos with refused z-order cannot claim placement success");
    Check(PlaceOverlayBelow(refusedOverlay.Get(), boundaryTarget.Get()) == HRESULT_FROM_WIN32(ERROR_RETRY),
          "successful SetWindowPos with refused promotion cannot claim band success");
    Check(!(GetWindowLongPtrW(refusedOverlay.Get(), GWL_EXSTYLE) & WS_EX_TOPMOST),
          "refused band transition leaves native overlay ordinary");

    refusedOverlay.count.refuseZOrder = false;
    Check(PlaceOverlayBelow(refusedOverlay.Get(), boundaryTarget.Get()) == S_OK,
          "a later reconciliation can recover from a refused band transition");
    Placed(refusedOverlay.Get(), boundaryTarget.Get());
    Check(SetWindowPos(boundaryTarget.Get(), HWND_NOTOPMOST, 0, 0, 0, 0, PositionFlags) != FALSE,
          "demote only our native band-boundary target");
    Check(PlaceOverlayBelow(refusedOverlay.Get(), boundaryTarget.Get()) == S_OK,
          "recovered placement still follows a subsequent target demotion");
    Placed(refusedOverlay.Get(), boundaryTarget.Get());
    for (HWND window : {target, other, dialog, overlay.Get(), otherOverlay.Get(), background.Get()}) {
        Check(!IsWindowVisible(window), "native unit fixture never becomes visible");
    }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--peer") return RunPeer(argv[2]);
        Check(argc == 1, "native unit tests accept no UI-launch options");
        ModelTests();
        NativeTests();
        std::cout << "PASS: " << checks << " model/private-desktop native checks; hidden HWNDs only.\n"
                  << "LIMIT: no visible-pixel, desktop input, real-app or foreground-switch evidence.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
