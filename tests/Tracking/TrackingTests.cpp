#include "../../src/Tracking/WindowTracker.h"

#include <array>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace window_sweaters::tracking;

namespace window_sweaters::tracking {
struct TrackerTestAccess {
    static NativeQueryCounts Probe(WindowTracker& tracker, HWND hwnd) {
        const auto before = QueryCounts();
        ActionBatch actions;
        tracker.Enqueue(reinterpret_cast<WindowHandle>(hwnd), Dirty::Lifecycle);
        tracker.Drain(actions, 0);
        if (!actions.empty()) throw std::runtime_error("In-process candidates never produce overlay actions");
        const auto after = QueryCounts();
        return {after.processOpens - before.processOpens, after.dwmReads - before.dwmReads};
    }
};
}

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void Expect(const ActionBatch& actions, std::initializer_list<ActionKind> kinds) {
    Check(actions.size() == kinds.size(), "Unexpected action count");
    std::size_t index{};
    for (const auto kind : kinds) {
        Check(actions[index++].kind == kind, "Unexpected action order/kind");
    }
}

AliveWindow Window(WindowHandle hwnd = 100) {
    AliveWindow result;
    result.hwnd = hwnd;
    result.identity = {200, 123456789, 400};
    result.className = L"OrdinaryApp";
    result.rect = {100, 200, 900, 800};
    result.dpi = 96;
    return result;
}

Policy DefaultPolicy() {
    Policy policy;
    policy.hostPid = 999;
    return policy;
}

void DiscoveryMovementAndOrdering() {
    TrackingModel model{DefaultPolicy()};
    auto window = Window();
    const auto initial = model.Observe(window, Dirty::Lifecycle, 10);
    Expect(initial, {ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    Check(initial[0].target == TargetId{100, 200, 123456789, 1}, "Identity payload");
    Check(initial[0].rect == PhysicalRect{100, 200, 900, 800}, "Physical geometry payload");
    Check(initial[0].dpi == 96 && initial[0].appearanceRevision == 1
          && initial[0].rebuildAppearance, "Initial appearance payload");
    Expect(model.Observe(window, Dirty::Lifecycle, 20), {});
    Expect(model.Observe(window, Dirty::MoveSizeStart, 30), {});
    window.rect = {150, 180, 950, 780};
    const auto moved = model.Observe(window, Dirty::Geometry, 40);
    Expect(moved, {ActionKind::Move});
    Check(moved[0].target == initial[0].target && !moved[0].rebuildAppearance
          && moved[0].appearanceRevision == 1, "Pure movement must retain yarn");
    Expect(model.Observe(window, Dirty::MoveSizeEnd, 50), {});
    const auto ordered = model.Observe(window, Dirty::Ordering, 60);
    Expect(ordered, {ActionKind::Reorder});
    Check(!ordered[0].rebuildAppearance && ordered[0].appearanceRevision == 1,
          "Focus/ordering must retain yarn");
    window.topmost = true;
    const auto band = model.Observe(window, Dirty::Lifecycle, 70);
    Expect(band, {ActionKind::Reorder});
    Check(band[0].topmost, "Topmost band is a stacking contract payload");
}

void ResizeSettlementWithoutEnd() {
    TrackingModel model{DefaultPolicy()};
    auto window = Window();
    const auto initial = model.Observe(window, Dirty::Lifecycle, 0);
    const WorkToken oldWork{initial[0].target, 1};
    Expect(model.Observe(window, Dirty::MoveSizeStart, 10), {});
    Check(model.Accepts(oldWork), "A move-size loop alone must not suppress work");
    window.rect.right = 1000;
    const auto resizing = model.Observe(window, Dirty::Geometry, 20);
    Expect(resizing, {ActionKind::Hide, ActionKind::Move});
    Check(!resizing[1].rebuildAppearance && resizing[1].appearanceRevision == 2,
          "Live resizing must defer regeneration");
    Check(!model.Accepts(oldWork), "Old work cannot publish during resizing");
    Expect(model.Observe(window, Dirty::MoveSizeEnd, 30), {});
    Expect(model.Observe(window, Dirty::Settlement, 269), {});
    const auto settled = model.Observe(window, Dirty::Settlement, 270);
    Expect(settled, {ActionKind::Show, ActionKind::Reorder});
    Check(settled[0].rebuildAppearance && settled[0].appearanceRevision == 2,
          "Settlement must rebuild only the final dimensions");
    Check(model.Accepts({initial[0].target, 2}), "Final work token is current");
    Expect(model.Observe(window, Dirty::MoveSizeEnd, 280), {});
    Expect(model.Observe(window, Dirty::Settlement, 300), {});
    window.rect.bottom += 10;
    Expect(model.Observe(window, Dirty::Geometry, 310), {ActionKind::Hide, ActionKind::Move});
    window.rect.bottom += 10;
    Expect(model.Observe(window, Dirty::Geometry, 550), {ActionKind::Move});
    Expect(model.Observe(window, Dirty::Settlement, 799), {});
    Expect(model.Observe(window, Dirty::Settlement, 800), {ActionKind::Show, ActionKind::Reorder});
}

void QueryFailureRetainsLifetime() {
    TrackingModel model{DefaultPolicy()};
    auto window = Window();
    const auto id = model.Observe(window, Dirty::Lifecycle, 0)[0].target;
    Expect(model.Observe(UncertainWindow{100}, Dirty::Lifecycle, 10), {ActionKind::Hide});
    Check(model.Current(100) == id, "Uncertainty must preserve the target lifetime");
    Check(!model.Accepts({id, 1}), "Uncertainty must reject late work");
    Expect(model.Observe(UncertainWindow{100}, Dirty::Lifecycle, 20), {});
    Expect(model.Observe(window, Dirty::Lifecycle, 30), {ActionKind::Show, ActionKind::Reorder});
    Check(model.Current(100) == id && model.Accepts({id, 1}), "Recovery retains lifetime/yarn");
    window.dpi = 0;
    Expect(model.Observe(window, Dirty::Ordering, 40), {ActionKind::Hide});
    Check(model.Current(100) == id, "An incomplete alive snapshot is query-uncertain");
    window.dpi = 96;
    Expect(model.Observe(window, Dirty::Lifecycle, 50), {ActionKind::Show, ActionKind::Reorder});
    Expect(model.Observe(UncertainWindow{100}, Dirty::Appearance, 51), {ActionKind::Hide});
    const auto styleRecovered = model.Observe(window, Dirty::Lifecycle, 52);
    Expect(styleRecovered, {ActionKind::Show, ActionKind::Reorder});
    Check(styleRecovered[0].rebuildAppearance && styleRecovered[0].appearanceRevision == 2,
          "Query failure must not lose a pending appearance invalidation");
    Expect(model.Observe(DestroyedWindow{100}, Dirty::Lifecycle, 60), {ActionKind::Remove});
    Expect(model.Observe(DestroyedWindow{100}, Dirty::Lifecycle, 70), {});
    Check(!model.Current(100) && !model.Accepts({id, 1}), "Confirmed death invalidates work");
}

void EverySuppressionUsesTheSameGate() {
    const std::array<bool AliveWindow::*, 5> flags{
        &AliveWindow::minimized, &AliveWindow::cloaked, &AliveWindow::maximized,
        &AliveWindow::fullscreen, &AliveWindow::visible,
    };
    for (const auto flag : flags) {
        auto window = Window();
        TrackingModel model{DefaultPolicy()};
        const auto id = model.Observe(window, Dirty::Lifecycle, 0)[0].target;
        window.*flag = flag != &AliveWindow::visible;
        Check(Evaluate(window, DefaultPolicy()) == Eligibility::Suppressed, "Suppression gate");
        Expect(model.Observe(window, Dirty::Ordering, 10), {ActionKind::Hide});
        Expect(model.Observe(window, Dirty::Geometry | Dirty::MoveSizeEnd, 20), {});
        Check(model.Current(100) == id && !model.Accepts({id, 1}), "Suppression retains lifetime");
        window.*flag = flag == &AliveWindow::visible;
        Expect(model.Observe(window, Dirty::Lifecycle, 30), {ActionKind::Show, ActionKind::Reorder});

        TrackingModel discoveredHidden{DefaultPolicy()};
        window.*flag = flag != &AliveWindow::visible;
        Expect(discoveredHidden.Observe(window, Dirty::Lifecycle, 0), {});
        Check(discoveredHidden.Size() == 1, "Initially suppressed targets remain discoverable");
        window.*flag = flag == &AliveWindow::visible;
        Expect(discoveredHidden.Observe(window, Dirty::Geometry, 10),
               {ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    }
}

void UnsupportedAndFixtureIsolation() {
    auto policy = DefaultPolicy();
    policy.excludedPids = {300};
    policy.excludedClasses = {L"SettingsSurface"};
    std::array windows{Window(1), Window(2), Window(3), Window(4), Window(5), Window(6), Window(7)};
    windows[0].identity.pid = 999;
    windows[1].identity.pid = 300;
    windows[2].className = L"Shell_TrayWnd";
    windows[3].className = L"SettingsSurface";
    windows[4].ordinaryTopLevel = false;
    windows[5].className = L"WindowSweaters.TestFixture";
    windows[6].className = L"WindowSweaters.TestFixtureReceiver";
    auto elevated = Window(8);
    elevated.outranksHost = true;
    Check(Evaluate(elevated, policy) == Eligibility::Unsupported, "Higher-integrity windows are unsupported");
    Expect(TrackingModel{policy}.Observe(elevated, Dirty::Lifecycle | Dirty::Ordering | Dirty::Appearance, 0), {});
    TrackingModel model{policy};
    for (const auto& window : windows) {
        Check(Evaluate(window, policy) == Eligibility::Unsupported, "Unsupported policy");
        Expect(model.Observe(window, Dirty::Lifecycle | Dirty::Ordering | Dirty::Appearance, 0), {});
    }
    Check(model.Size() == 0, "Unsupported windows never become targets");

    auto window = Window();
    const auto id = model.Observe(window, Dirty::Lifecycle, 1)[0].target;
    window.ordinaryTopLevel = false;
    Expect(model.Observe(window, Dirty::Geometry | Dirty::Ordering, 2), {ActionKind::Hide});
    Check(model.Current(100) == id, "A style transition is not authoritative death");
    window.ordinaryTopLevel = true;
    Expect(model.Observe(window, Dirty::Lifecycle, 3), {ActionKind::Show, ActionKind::Reorder});

    policy.mode = TrackingMode::FixtureOnly;
    policy.fixtureTarget = 100;
    TrackingModel fixture{policy};
    Check(fixture.SealFixtureIdentity(window.identity), "Fixture identity seal");
    Expect(fixture.Observe(Window(101), Dirty::Lifecycle, 0), {});
    window.className = L"WindowSweaters.TestFixture";
    const auto fixtureId = fixture.Observe(window, Dirty::Lifecycle, 10)[0].target;
    Check(!fixture.SealFixtureIdentity(window.identity), "Cannot reseal an active fixture");
    window.identity.processStart++;
    Expect(fixture.Observe(window, Dirty::Lifecycle, 20), {ActionKind::Remove});
    Check(!fixture.Current(100) && !fixture.Accepts({fixtureId, 1}), "Reused HWND cannot escape fixture");
}

void ReuseAndReorderedNotifications() {
    TrackingModel model{DefaultPolicy()};
    auto window = Window();
    const auto oldId = model.Observe(window, Dirty::Lifecycle, 100)[0].target;
    // Notification order is not evidence of death: a late DESTROY queries a live target.
    Expect(model.Observe(window, Dirty::Lifecycle, 110), {});
    Expect(model.Observe(window, Dirty::Lifecycle, 120, 90), {});
    Check(model.Current(100) == oldId, "Late CREATE cannot replace existing generation");
    window.identity.pid = 201;
    window.identity.processStart = 987654321;
    const auto replaced = model.Observe(window, Dirty::Lifecycle, 130);
    Expect(replaced, {ActionKind::Remove, ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    Check(replaced[0].target == oldId && replaced[1].target.generation == 2,
          "PID/process reuse removes only the matching old generation");
    Check(!model.Accepts({oldId, 1}), "Late work cannot resurrect a reused HWND");
    const auto nextId = replaced[1].target;
    Expect(model.Observe(window, Dirty::Lifecycle, 140, 125), {});
    Check(model.Current(100) == nextId, "Reordered old CREATE is harmless");
    const auto sameProcessReuse = model.Observe(window, Dirty::Lifecycle, 160, 150);
    Expect(sameProcessReuse, {ActionKind::Remove, ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    Check(sameProcessReuse[1].target.generation == 3, "Observed CREATE detects same-process HWND reuse");
    Expect(model.Observe(window, Dirty::Lifecycle, 170, 150), {});
    Check(!model.Accepts({nextId, 1}), "Same-process reuse invalidates previous work");
    window.identity.observedIdentity++;
    const auto changedClassOrThread = model.Observe(window, Dirty::Lifecycle, 180);
    Expect(changedClassOrThread, {ActionKind::Remove, ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    Check(changedClassOrThread[1].target.generation == 4, "Observed identity changes revalidate ownership");
    Expect(model.Observe(DestroyedWindow{100}, Dirty::Lifecycle, 190), {ActionKind::Remove});
    const auto rediscovered = model.Observe(window, Dirty::Lifecycle, 200);
    Check(rediscovered[0].target.generation == 5, "Rediscovery never restarts the generation counter");
    Expect(model.Clear(), {ActionKind::Remove});
    Check(model.Observe(window, Dirty::Lifecycle, 210)[0].target.generation == 6,
          "Stop/restart cannot accept tokens from the previous run");
}

void AppearanceAndCapacity() {
    auto policy = DefaultPolicy();
    policy.maxTargets = 2;
    TrackingModel model{policy};
    auto window = Window(1);
    const auto first = model.Observe(window, Dirty::Lifecycle, 0)[0].target;
    Expect(model.Observe(Window(2), Dirty::Lifecycle, 0),
           {ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    Expect(model.Observe(Window(3), Dirty::Lifecycle, 0), {});
    Check(model.Size() == 2, "Target registry must be bounded");
    const auto appearance = model.Observe(window, Dirty::Appearance, 10);
    Expect(appearance, {ActionKind::Move});
    Check(appearance[0].appearanceRevision == 2 && appearance[0].rebuildAppearance,
          "Explicit appearance dirtiness regenerates without a hide");
    Check(!model.Accepts({first, 1}) && model.Accepts({first, 2}),
          "Appearance results are independently generation-tagged");
    window.dpi = 144;
    const auto dpi = model.Observe(window, Dirty::Geometry, 20);
    Expect(dpi, {ActionKind::Move});
    Check(dpi[0].dpi == 144 && dpi[0].appearanceRevision == 3 && dpi[0].rebuildAppearance,
          "DPI changes invalidate appearance even when size is unchanged");
    Expect(model.Observe(DestroyedWindow{2}, Dirty::Lifecycle, 30), {ActionKind::Remove});
    const auto missedCreate = model.Observe(Window(3), Dirty::None, 40);
    Expect(missedCreate, {ActionKind::Create, ActionKind::Show, ActionKind::Reorder});
    Check(missedCreate[0].target.generation == 3, "Reconciliation discovers a missed CREATE");
    Check(model.Handles() == std::vector<WindowHandle>{1, 3}, "Bounded reconciliation handles");
}

void CurrentSnapshotContract() {
    TrackingModel model{DefaultPolicy()};
    auto window = Window();
    const auto initial = model.Observe(window, Dirty::Lifecycle, 0);
    const WorkToken token{initial[0].target, 1};
    auto snapshot = model.CurrentSnapshot(token);
    Check(snapshot && snapshot->visible && snapshot->rect == PhysicalRect{100, 200, 900, 800},
          "Exact-token current snapshot");
    window.rect = {-100, -200, 700, 400};
    window.topmost = true;
    Expect(model.Observe(window, Dirty::Geometry | Dirty::Ordering, 10),
           {ActionKind::Move, ActionKind::Reorder});
    Expect(model.Observe(window, Dirty::Lifecycle, 20), {});
    snapshot = model.CurrentSnapshot(token);
    Check(snapshot && snapshot->rect == PhysicalRect{-100, -200, 700, 400}
          && snapshot->topmost && snapshot->dpi == 96, "No-delta validation retains canonical geometry/band");
    Expect(model.Observe(UncertainWindow{100}, Dirty::Lifecycle, 30), {ActionKind::Hide});
    snapshot = model.CurrentSnapshot(token);
    Check(snapshot && !snapshot->visible && !model.Accepts(token),
          "Nested uncertainty updates policy even after an earlier accepted query");
    Expect(model.Observe(window, Dirty::Lifecycle, 40), {ActionKind::Show, ActionKind::Reorder});
    window.dpi = 144;
    Expect(model.Observe(window, Dirty::Geometry, 50), {ActionKind::Move});
    Check(!model.CurrentSnapshot(token), "Snapshot rejects stale appearance token");
    snapshot = model.CurrentSnapshot({token.target, 2});
    Check(snapshot && snapshot->dpi == 144 && snapshot->appearanceRevision == 2, "Current DPI snapshot");
    auto wrong = token.target;
    ++wrong.generation;
    Check(!model.CurrentSnapshot({wrong, 2}), "Snapshot rejects a different lifetime");
    Expect(model.Observe(DestroyedWindow{100}, Dirty::Lifecycle, 60), {ActionKind::Remove});
    Check(!model.CurrentSnapshot({token.target, 2}), "Destroyed lifetime has no snapshot");
}

void FixtureStartupSelection() {
    auto fixture = Window();
    fixture.className = L"WindowSweaters.TestFixture";
    const auto selected = fixture.identity;
    auto policy = DefaultPolicy();
    policy.mode = TrackingMode::FixtureOnly;
    policy.fixtureTarget = fixture.hwnd;
    policy.fixtureIdentity = selected;
    Check(MatchesFixtureSelection(fixture, selected, L"WindowSweaters Fixture"),
          "Startup accepts the initially selected exact fixture identity/class/title");
    for (int change = 0; change < 5; ++change) {
        auto reused = fixture;
        if (change == 0) ++reused.identity.pid;
        if (change == 1) ++reused.identity.processStart;
        if (change == 2) ++reused.identity.observedIdentity;
        if (change == 3) reused.className = L"OrdinaryApp";
        const auto title = change == 4 ? L"Ordinary application" : L"WindowSweaters Fixture";
        Check(!MatchesFixtureSelection(reused, selected, title),
              "Startup gap cannot reseal a reused HWND or changed fixture class/title");
        if (change < 3) Check(Evaluate(reused, policy) == Eligibility::Unsupported,
                             "Initial fixture seal rejects PID/process/thread reuse before Start");
    }
    TrackingModel model{policy};
    Check(model.SealFixtureIdentity(selected), "Initial identity is sealed, not current reused identity");
    auto foreign = fixture;
    ++foreign.identity.pid;
    foreign.className = L"OrdinaryApp";
    Expect(model.Observe(foreign, Dirty::Lifecycle, 0), {});
    Check(model.Size() == 0, "No foreign target is selected across startup reuse");
}

void DelayedGeometryObservations() {
    TrackingModel model{DefaultPolicy()};
    GeometryReprobe reprobe;
    auto window = Window();
    const auto initial = model.Observe(window, Dirty::Lifecycle, 0);
    const auto target = initial[0].target;
    const WorkToken token{target, 1};
    // LOCATIONCHANGE can precede DWM's extended-frame update; this first sample remains old.
    Expect(model.Observe(window, Dirty::Geometry, 10), {});
    Check(reprobe.Request(target, 10), "Schedule finite compositor-lag probes after an event");
    Check(reprobe.TakeDue(109).empty(), "No immediate hot-loop retry");
    const auto first = reprobe.TakeDue(110);
    Check(first == std::vector<TargetId>{target}, "First delayed sample is due after 100 ms");
    window.rect = {114, 214, 914, 814};
    const auto move = model.Observe(window, Dirty::Geometry, 110);
    Expect(move, {ActionKind::Move});
    Check(move[0].rect == PhysicalRect{114, 214, 914, 814}
          && !move[0].rebuildAppearance && move[0].appearanceRevision == 1
          && model.Accepts(token), "Fresh delayed sample repositions without regeneration or suppression");
    Check(reprobe.TakeDue(110).empty(), "One target cannot be queried twice in one tick");
    Check(reprobe.TakeDue(210) == std::vector<TargetId>{target}, "Second bounded lag sample");
    Expect(model.Observe(window, Dirty::Geometry, 210), {});
    Check(reprobe.TakeDue(310) == std::vector<TargetId>{target}, "Third bounded lag sample");
    Expect(model.Observe(window, Dirty::Geometry, 310), {});
    Check(reprobe.Size() == 0 && reprobe.TakeDue(410).empty()
          && reprobe.TakeDue(5000).empty(), "Completed event has no recurring queries or rendering");

    Check(reprobe.Request(target, 600), "New geometry hint schedules only its own lifetime");
    Check(reprobe.Request(target, 650), "Duplicate hints coalesce");
    Check(reprobe.Size() == 1 && reprobe.TakeDue(700) == std::vector<TargetId>{target},
          "Duplicate hint cannot postpone the first deadline");
    Expect(model.Observe(DestroyedWindow{window.hwnd}, Dirty::Lifecycle, 710), {ActionKind::Remove});
    const auto rediscovered = model.Observe(window, Dirty::Lifecycle, 720)[0].target;
    Check(rediscovered != target && model.Current(window.hwnd) != target,
          "Queued delayed probe cannot authorize a reused HWND lifetime");
    Check(reprobe.Request(rediscovered, 720), "Reuse replaces delayed identity rather than copying a pointer");
    Check(reprobe.TakeDue(800).empty() && reprobe.TakeDue(820) == std::vector<TargetId>{rediscovered},
          "New lifetime owns its own delay");
    reprobe.Cancel(window.hwnd);
    Check(reprobe.Size() == 0 && reprobe.TakeDue(1000).empty(), "Confirmed death cancels delayed sampling");

    Check(reprobe.Request(rediscovered, 1100), "Pending retry before shutdown");
    reprobe.Clear();
    Check(reprobe.TakeDue(1200).empty(), "Stop discards delayed work");
}

void DelayedGeometryBudgets() {
    GeometryReprobe reprobe;
    for (std::size_t index = 0; index < GeometryReprobeLimit; ++index)
        Check(reprobe.Request({index + 1, 200, 123456789, index + 1}, 0), "Fill bounded delayed registry");
    Check(!reprobe.Request({GeometryReprobeLimit + 1, 200, 123456789, 1}, 0)
          && reprobe.Size() == GeometryReprobeLimit, "Delayed registry is capped");
    Check(reprobe.Request({1, 200, 123456789, 1}, 90), "Existing delayed target survives capacity");
    Check(reprobe.TakeDue(99).empty(), "Capacity does not bypass minimum delay");
    std::vector<unsigned> queries(GeometryReprobeLimit, 0);
    for (unsigned round = 1; round <= GeometryReprobeAttempts; ++round) {
        std::size_t total{};
        for (std::size_t batch = 0; batch < GeometryReprobeLimit / GeometryReprobeBudget; ++batch) {
            const auto due = reprobe.TakeDue(round * GeometryReprobeDelayMs);
            Check(due.size() == GeometryReprobeBudget && due.size() <= TrackerDispatchBudget,
                  "At most 32 delayed candidates share the existing 64-query dispatch budget");
            for (const auto& target : due) {
                Check(target.hwnd && target.hwnd <= GeometryReprobeLimit, "Full lifetime due payload");
                ++queries[target.hwnd - 1];
            }
            total += due.size();
        }
        Check(total == GeometryReprobeLimit, "Round-robin sampling cannot starve later targets");
        Check(reprobe.TakeDue(round * GeometryReprobeDelayMs).empty(),
              "All sampled targets wait until another tick");
    }
    for (const auto count : queries) Check(count == GeometryReprobeAttempts, "Per-hint retry bound");
    Check(reprobe.Size() == 0 && reprobe.TakeDue(10000).empty(), "No polling after finite retry exhaustion");
}

void NativeReceiverContract() {
    Check(std::holds_alternative<DestroyedWindow>(WindowTracker::Query(nullptr)), "Null HWND query");
    const HWND receiver = CreateWindowExW(0, L"STATIC", L"Tracking contract receiver",
                                         WS_OVERLAPPEDWINDOW, 0, 0, 200, 200,
                                         nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(receiver != nullptr, "Create isolated receiver");
    {
        TrackerConfig config;
        config.mode = TrackingMode::FixtureOnly;
        config.fixtureTarget = receiver;
        WindowTracker tracker{receiver, config};
        ActionBatch actions;
        Check(!tracker.Start(actions), "Fixture cannot track this host process");
        Check(actions.empty(), "A rejected fixture cannot create any overlay");
        Check(!tracker.HandleMessage(TrackerDispatchMessage, 0, 0, actions), "Stopped receiver");
        Check(tracker.Stop().empty(), "Idempotent stop");
    }
    {
        const auto observation = WindowTracker::Query(receiver);
        Check(std::holds_alternative<AliveWindow>(observation), "Hidden native receiver identity query");
        TrackerConfig config;
        config.mode = TrackingMode::FixtureOnly;
        config.fixtureTarget = receiver;
        config.initialFixtureIdentity = std::get<AliveWindow>(observation).identity;
        WindowTracker tracker{receiver, config};
        ActionBatch actions;
        Check(!tracker.Start(actions) && actions.empty(), "A sealed initial identity cannot select this process");
    }
    DestroyWindow(receiver);
    Check(std::holds_alternative<DestroyedWindow>(WindowTracker::Query(receiver)), "Confirmed native death");
}

void NativeApplicationFramePolicy() {
    const auto instance=GetModuleHandleW(nullptr);
    const wchar_t* classes[]{L"ApplicationFrameWindow",L"WindowSweaters.NativePopupPolicy"};
    struct Classes {
        HINSTANCE instance;
        const wchar_t* const* names;
        unsigned registered{};
        ~Classes() {
            for (unsigned i=0;i<registered;++i) UnregisterClassW(names[i],instance);
        }
    } registered{instance,classes};
    for (const auto name:classes) {
        WNDCLASSW registration{};
        registration.hInstance=instance;
        registration.lpfnWndProc=DefWindowProcW;
        registration.lpszClassName=name;
        if (!RegisterClassW(&registration))
            throw std::runtime_error("Register hidden native popup/frame test class "+
                std::to_string(registered.registered)+": "+std::to_string(GetLastError()));
        ++registered.registered;
    }
    struct OwnedWindow {
        HWND value{};
        ~OwnedWindow() { if (value) DestroyWindow(value); }
    } owner{CreateWindowExW(0,classes[1],L"",WS_OVERLAPPEDWINDOW,0,0,240,160,
        nullptr,nullptr,instance,nullptr)};
    Check(owner.value!=nullptr,"Create hidden native owner for owned-frame rejection");
    unsigned assertions{};
    const auto verify=[&](HWND window,bool expected) {
        NativeQueryFailure failure;
        const auto query=WindowTracker::Query(window,&failure);
        if (!std::holds_alternative<AliveWindow>(query)) {
            Check(!expected && failure.stage!=NativeQueryStage::None &&
                Evaluate(query,Policy{})==Eligibility::Uncertain,
                "Unsupported hidden frame with unavailable native geometry stays fail-closed");
            ++assertions;
            return;
        }
        Check(failure.stage==NativeQueryStage::None,"Complete native frame query has no failure diagnostic");
        const auto& alive=std::get<AliveWindow>(query);
        Check(alive.ordinaryTopLevel==expected,"Native popup/frame style must match exact supported gate");
        Policy policy;
        const auto eligibility=Evaluate(query,policy);
        Check(eligibility==(expected ? Eligibility::Suppressed : Eligibility::Unsupported),
            "Production gate preserves hidden suppression or unsupported style without overlay authorization");
        assertions+=3;
    };
    struct Case { const wchar_t* name; DWORD style,extended; HWND owner; bool admitted; };
    const Case cases[]{
        {classes[0],WS_POPUP|WS_OVERLAPPEDWINDOW,0,nullptr,true},
        {classes[0],WS_OVERLAPPEDWINDOW,0,nullptr,true},
        {classes[0],WS_POPUP|WS_CAPTION,WS_EX_TOOLWINDOW,nullptr,false},
        {classes[0],WS_POPUP|WS_CAPTION,WS_EX_NOACTIVATE,nullptr,false},
        {classes[0],WS_POPUP,0,nullptr,false},
        {classes[0],WS_POPUP|WS_CAPTION,0,owner.value,false},
        {classes[0],WS_CHILD|WS_CAPTION,0,owner.value,false},
        {classes[1],WS_POPUP|WS_CAPTION,0,nullptr,false},
        {classes[1],WS_POPUP|WS_CAPTION|WS_SYSMENU,0,nullptr,false},
        {classes[1],WS_POPUP|WS_CAPTION|WS_THICKFRAME,0,nullptr,false},
        {classes[1],WS_POPUP|WS_OVERLAPPEDWINDOW,0,nullptr,true},
        {classes[1],WS_POPUP|WS_OVERLAPPEDWINDOW,WS_EX_TOOLWINDOW,nullptr,false},
        {classes[1],WS_POPUP|WS_OVERLAPPEDWINDOW,0,owner.value,false}
    };
    for (const auto& item:cases) {
        OwnedWindow window{CreateWindowExW(item.extended,item.name,L"",item.style,0,0,240,160,
            item.owner,nullptr,instance,nullptr)};
        Check(window.value!=nullptr,"Create hidden native frame/popup policy case");
        verify(window.value,item.admitted);
        verify(window.value,item.admitted);
    }
    OwnedWindow changing{CreateWindowExW(0,classes[0],L"",WS_POPUP|WS_CAPTION,0,0,240,160,
        nullptr,nullptr,instance,nullptr)};
    Check(changing.value!=nullptr,"Create hidden native frame for style revalidation");
    verify(changing.value,true);
    auto menu=std::get<AliveWindow>(WindowTracker::Query(changing.value));
    menu.className=L"#32768";
    Check(Evaluate(menu,Policy{})==Eligibility::Unsupported,
        "Existing explicit menu-class exclusion outranks otherwise ordinary frame facts");
    ++assertions;
    SetLastError(0);
    const auto previous=SetWindowLongPtrW(changing.value,GWL_EXSTYLE,WS_EX_TOOLWINDOW);
    Check(previous!=0 || GetLastError()==0,"Change only owned hidden frame extended style");
    verify(changing.value,false);
    SetLastError(0);
    Check(SetWindowLongPtrW(changing.value,GWL_EXSTYLE,previous)!=0 || GetLastError()==0,
        "Restore only owned hidden frame extended style");
    verify(changing.value,true);
    std::cout<<"PASS: "<<assertions<<" native popup-frame style/query/gate assertions; "
        "ApplicationFrameWindow or system-framed ownerless popups only, hidden windows, no desktop app mutation\n";
}

void NativeProbePrefilter() {
    const auto instance=GetModuleHandleW(nullptr);
    const wchar_t* name=L"WindowSweaters.NativeProbePrefilter";
    WNDCLASSW registration{};
    registration.hInstance=instance;
    registration.lpfnWndProc=DefWindowProcW;
    registration.lpszClassName=name;
    Check(RegisterClassW(&registration)!=0,"Register hidden native prefilter test class");
    struct Unregister { const wchar_t* name; HINSTANCE instance; ~Unregister(){UnregisterClassW(name,instance);} } unregister{name,instance};
    struct OwnedWindow {
        HWND value{};
        ~OwnedWindow() { if (value) DestroyWindow(value); }
    };
    const auto make=[&](DWORD style,DWORD extended,HWND parent) {
        OwnedWindow window{CreateWindowExW(extended,name,L"",style,0,0,240,160,parent,nullptr,instance,nullptr)};
        Check(window.value!=nullptr,"Create hidden native prefilter case");
        return window;
    };
    const auto owner=make(WS_OVERLAPPEDWINDOW,0,nullptr);
    WindowTracker tracker{owner.value,TrackerConfig{}};
    const auto probe=[&](HWND window) { return TrackerTestAccess::Probe(tracker,window); };
    const auto zero=[](const NativeQueryCounts& counts) { return counts.processOpens==0 && counts.dwmReads==0; };
    const auto full=[](const NativeQueryCounts& counts) { return counts.processOpens==2 && counts.dwmReads==2; };
    struct Case { DWORD style,extended; HWND parent; };
    const Case rejected[]{
        {WS_OVERLAPPEDWINDOW,WS_EX_TOOLWINDOW,nullptr},
        {WS_OVERLAPPEDWINDOW,WS_EX_NOACTIVATE,nullptr},
        {WS_OVERLAPPEDWINDOW,0,owner.value},
        {WS_POPUP|WS_CAPTION,0,nullptr},
        {WS_POPUP,0,nullptr},
        {WS_CHILD|WS_CAPTION,0,owner.value},
    };
    for (const auto& item:rejected) {
        const auto window=make(item.style,item.extended,item.parent);
        Check(zero(probe(window.value)) && zero(probe(window.value)),
            "Unknown tool/owned/popup/child candidates make no process or DWM call, on every pass");
    }
    const auto hidden=make(WS_OVERLAPPEDWINDOW,0,nullptr);
    const auto own=WindowTracker::Query(hidden.value);
    Check(std::holds_alternative<AliveWindow>(own) && !std::get<AliveWindow>(own).outranksHost,
        "A same-integrity process does not outrank the host");
    Check(!IsWindowVisible(hidden.value),"Prefilter admission case is hidden");
    Check(full(probe(hidden.value)) && full(probe(hidden.value)),
        "Hidden supported frames are never rejected by the prefilter and each probe queries fully");
    const auto frame=make(WS_OVERLAPPEDWINDOW,0,nullptr);
    SetLastError(0);
    const auto previous=SetWindowLongPtrW(frame.value,GWL_EXSTYLE,WS_EX_TOOLWINDOW);
    Check(previous!=0 || GetLastError()==0,"Make hidden frame a tool window");
    Check(zero(probe(frame.value)),"Tool-window state is rejected without native queries");
    SetLastError(0);
    Check(SetWindowLongPtrW(frame.value,GWL_EXSTYLE,previous)!=0 || GetLastError()==0,"Restore hidden frame style");
    Check(full(probe(frame.value)),"Restored styles allow rediscovery, nothing is cached across probes");
    SetWindowLongPtrW(frame.value,GWL_EXSTYLE,WS_EX_TOOLWINDOW);
    Check(zero(probe(frame.value)),"Rejection is re-evaluated on each probe");
    std::cout<<"PASS: native Probe prefilter spends zero process/DWM calls on unknown rejected candidates; hidden frames and restored styles keep full Query\n";
}

void SelectedLifetimeIsolation() {
    for (const auto hidden : {false,true}) {
        auto selected=Window(101);
        selected.minimized=!hidden;
        selected.visible=!hidden;
        auto policy=DefaultPolicy();
        policy.mode=TrackingMode::SelectedOnly;
        policy.maxTargets=2;
        policy.selectedTargets={{selected.hwnd,selected.identity},{102,Window(102).identity}};
        TrackingModel model(policy);
        for (WindowHandle hwnd=1000;hwnd<1600;++hwnd) {
            auto unrelated=Window(hwnd); unrelated.minimized=(hwnd%2)==0;
            Expect(model.Observe(unrelated,Dirty::Lifecycle,1),{});
        }
        Check(model.Size()==0,"Unselected eligible/suppressed windows cannot consume selected tracker capacity");
        Expect(model.Observe(selected,Dirty::Lifecycle,10),{});
        const auto original=model.Current(101);
        Check(original && original->generation==1 && model.SelectionActive(101),
            "Initially suppressed selection receives its real tracking generation before any Create");
        Expect(model.Observe(DestroyedWindow{101},Dirty::Lifecycle,11),{});
        selected.visible=true; selected.minimized=false;
        Expect(model.Observe(selected,Dirty::Lifecycle,12,12),{});
        Check(!model.Current(101) && !model.SelectionActive(101),
            "Destroyed uncreated selection permanently retires same-HWND/PID/thread/class replacement");
        auto other=Window(102);
        const auto created=model.Observe(other,Dirty::Lifecycle,13);
        Expect(created,{ActionKind::Create,ActionKind::Show,ActionKind::Reorder});
        Check(created[0].target==TargetId{102,200,123456789,2},
            "Second selection retains reserved capacity and independent lifetime");
        model.Clear();
        Check(!model.SelectionActive(102),"Stop cannot reopen a selected lifetime");
        Expect(model.Observe(other,Dirty::Lifecycle,14),{});
    }
    for (const auto replacement:{0,1,2,3}) {
        auto selected=Window();
        selected.minimized=true;
        auto policy=DefaultPolicy(); policy.mode=TrackingMode::SelectedOnly;
        policy.selectedTargets={{selected.hwnd,selected.identity}};
        TrackingModel model(policy);
        Expect(model.Observe(selected,Dirty::Lifecycle,10),{});
        auto changed=selected;
        if (replacement==0) ++changed.identity.pid;
        if (replacement==1) ++changed.identity.processStart;
        if (replacement==2) ++changed.identity.observedIdentity;
        Expect(model.Observe(changed,Dirty::Lifecycle,20,replacement==3?std::optional<Milliseconds>{20}:std::nullopt),{});
        selected.minimized=false;
        Expect(model.Observe(selected,Dirty::Lifecycle,21),{});
        Check(!model.SelectionActive(100),"Identity or newer CREATE retires original suppressed authority permanently");
    }
    auto selected=Window(); selected.visible=false;
    auto policy=DefaultPolicy(); policy.mode=TrackingMode::SelectedOnly;
    policy.selectedTargets={{selected.hwnd,selected.identity}};
    TrackingModel model(policy);
    Expect(model.Observe(selected,Dirty::Lifecycle,10),{});
    const auto original=model.Current(100);
    Expect(model.Observe(UncertainWindow{100},Dirty::Lifecycle,11),{});
    selected.visible=true;
    Expect(model.Observe(selected,Dirty::Lifecycle,12),{ActionKind::Create,ActionKind::Show,ActionKind::Reorder});
    Check(model.Current(100)==original,"Suppression/uncertainty recovery retains the initially selected generation");
}

int SelectedNativeChild(HANDLE pipe) {
    std::array<HWND,2> windows{};
    for (auto& window:windows)
        window=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPEDWINDOW,0,0,200,200,
            nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const std::array<UINT_PTR,2> values{reinterpret_cast<UINT_PTR>(windows[0]),reinterpret_cast<UINT_PTR>(windows[1])};
    DWORD written{};
    const bool success=windows[0] && windows[1] && WriteFile(pipe,values.data(),sizeof(values),&written,nullptr) &&
        written==sizeof(values);
    CloseHandle(pipe);
    if (!success) return 1;
    MSG message{};
    while (GetMessageW(&message,nullptr,0,0)>0) {
        if (message.message==WM_APP+77) break;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    for (const auto window:windows) if (IsWindow(window)) DestroyWindow(window);
    return 0;
}

void NativeSelectedTracker() {
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE read{},write{};
    Check(CreatePipe(&read,&write,&security,0)!=FALSE,"Create private hidden-child metadata pipe");
    SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
    std::array<wchar_t,32768> path{};
    Check(GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()))!=0,"Locate private tracking child image");
    std::wstring command=L"\""+std::wstring(path.data())+L"\" --selected-native-child "+
        std::to_wstring(reinterpret_cast<UINT_PTR>(write));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const bool started=CreateProcessW(path.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,
        nullptr,nullptr,&startup,&process)!=FALSE;
    CloseHandle(write);
    Check(started,"Start only private hidden tracking child");
    struct Cleanup {
        HANDLE read;
        PROCESS_INFORMATION process;
        ~Cleanup() {
            PostThreadMessageW(process.dwThreadId,WM_APP+77,0,0);
            WaitForSingleObject(process.hProcess,10000);
            CloseHandle(read); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
    } cleanup{read,process};
    std::array<UINT_PTR,2> handles{};
    DWORD bytes{};
    const auto deadline=GetTickCount64()+10000;
    DWORD available{};
    while (GetTickCount64()<deadline && PeekNamedPipe(read,nullptr,0,nullptr,&available,nullptr) && available<sizeof(handles))
        Sleep(10);
    Check(available>=sizeof(handles) && ReadFile(read,handles.data(),sizeof(handles),&bytes,nullptr) &&
        bytes==sizeof(handles),"Read only private hidden-child HWND metadata");
    const HWND receiver=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPEDWINDOW,0,0,200,200,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(receiver!=nullptr,"Create private selected-tracker receiver");
    struct ReceiverCleanup { HWND window; ~ReceiverCleanup(){DestroyWindow(window);} } receiverCleanup{receiver};
    TrackerConfig config;
    config.mode=TrackingMode::SelectedOnly;
    config.maxTargets=2;
    for (const auto handle:handles) {
        const auto observation=WindowTracker::Query(reinterpret_cast<HWND>(handle));
        const auto* window=std::get_if<AliveWindow>(&observation);
        Check(window && !window->visible,"Selected native child starts hidden/suppressed without GUI presentation");
        config.selectedTargets.push_back({static_cast<WindowHandle>(handle),window->identity});
    }
    WindowTracker tracker(receiver,config);
    ActionBatch initial;
    Check(tracker.Start(initial) && initial.empty() && tracker.Model().Size()==2,
        "Real tracker initially seals two suppressed selections and excludes unrelated desktop enumeration");
    const auto original=tracker.Model().Current(static_cast<WindowHandle>(handles[0]));
    Check(original.has_value(),"Real selected tracker has generation before any Host Create");
    SendMessageW(reinterpret_cast<HWND>(handles[0]),WM_CLOSE,0,0);
    const auto destroyDeadline=GetTickCount64()+10000;
    while (tracker.Model().SelectionActive(static_cast<WindowHandle>(handles[0])) && GetTickCount64()<destroyDeadline) {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            ActionBatch actions;
            tracker.HandleMessage(message.message,message.wParam,message.lParam,actions);
        }
        ActionBatch actions;
        tracker.HandleMessage(WM_TIMER,TrackerTimerId,0,actions);
        Sleep(10);
    }
    Check(!tracker.Model().SelectionActive(static_cast<WindowHandle>(handles[0])) &&
        !tracker.Model().Current(static_cast<WindowHandle>(handles[0])) && tracker.Model().Size()==1,
        "Real suppressed child destruction permanently retires its original authority without Remove action");
    Check(tracker.Stop().empty(),"Stopping only hidden/uncreated selected tracker emits no GUI removal work");
    std::cout << "PASS: real cross-process hidden tracker seals suppressed selections before Create, avoids unrelated enumeration and permanently retires destroyed authority\n";
}

}

int main(int argc, char** argv) {
    try {
        if (argc==3 && std::string_view(argv[1])=="--selected-native-child")
            return SelectedNativeChild(reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(std::strtoull(argv[2],nullptr,10))));
        DiscoveryMovementAndOrdering();
        ResizeSettlementWithoutEnd();
        QueryFailureRetainsLifetime();
        EverySuppressionUsesTheSameGate();
        UnsupportedAndFixtureIsolation();
        ReuseAndReorderedNotifications();
        AppearanceAndCapacity();
        CurrentSnapshotContract();
        FixtureStartupSelection();
        DelayedGeometryObservations();
        DelayedGeometryBudgets();
        SelectedLifetimeIsolation();
        std::cout << "PASS: 12 deterministic tracking contract scenarios, including suppressed selections/reuse/capacity (scripted, not desktop evidence)\n";
        if (argc == 2 && std::string_view{argv[1]} == "--native-contract") {
            NativeReceiverContract();
            NativeApplicationFramePolicy();
            NativeProbePrefilter();
            NativeSelectedTracker();
            std::cout << "PASS: isolated native receiver/query rejection contract (not cross-process fixture evidence)\n";
        } else if (argc == 2 && std::string_view{argv[1]} == "--frame-policy-only") {
            NativeApplicationFramePolicy();
        } else if (argc != 1) {
            throw std::runtime_error("Usage: TrackingTests.exe [--native-contract|--frame-policy-only]");
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
