#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace window_sweaters::tracking {

using WindowHandle = std::uintptr_t;
using Milliseconds = std::uint64_t;

struct TargetId {
    WindowHandle hwnd{};
    std::uint32_t pid{};
    std::uint64_t processStart{};
    std::uint64_t generation{};
    bool operator==(const TargetId&) const = default;
};

struct PhysicalRect {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
    std::int64_t Width() const noexcept { return std::int64_t{right} - left; }
    std::int64_t Height() const noexcept { return std::int64_t{bottom} - top; }
    bool operator==(const PhysicalRect&) const = default;
};

struct WindowIdentity {
    std::uint32_t pid{};
    std::uint64_t processStart{};
    std::uint64_t observedIdentity{};
    bool operator==(const WindowIdentity&) const = default;
};

struct AliveWindow {
    WindowHandle hwnd{};
    WindowIdentity identity{};
    std::wstring className;
    PhysicalRect rect{};
    std::uint32_t dpi{};
    bool ordinaryTopLevel{true};
    bool visible{true};
    bool minimized{};
    bool cloaked{};
    bool maximized{};
    bool fullscreen{};
    bool topmost{};
};

struct DestroyedWindow { WindowHandle hwnd{}; };
struct UncertainWindow { WindowHandle hwnd{}; };
using Observation = std::variant<AliveWindow, DestroyedWindow, UncertainWindow>;

enum class Eligibility { Eligible, Suppressed, Unsupported, Destroyed, Uncertain };
enum class TrackingMode { Production, FixtureOnly, SelectedOnly };
inline constexpr std::size_t MaximumSelectedTargets = 2;
struct WindowSelection {
    WindowHandle hwnd{};
    WindowIdentity identity;
};

struct Policy {
    TrackingMode mode{TrackingMode::Production};
    WindowHandle fixtureTarget{};
    std::optional<WindowIdentity> fixtureIdentity;
    std::uint32_t hostPid{};
    std::vector<std::uint32_t> excludedPids;
    std::vector<std::wstring> excludedClasses;
    std::size_t maxTargets{512};
    Milliseconds resizeQuietPeriod{250};
    std::vector<WindowSelection> selectedTargets;
};

Eligibility Evaluate(const Observation& observation, const Policy& policy);
bool MatchesFixtureSelection(const AliveWindow& window, const WindowIdentity& selected,
                             std::wstring_view title) noexcept;

enum class Dirty : std::uint32_t {
    None = 0,
    Geometry = 1,
    Appearance = 2,
    Ordering = 4,
    Lifecycle = 8,
    MoveSizeStart = 16,
    MoveSizeEnd = 32,
    Settlement = 64,
};
constexpr Dirty operator|(Dirty left, Dirty right) noexcept {
    return static_cast<Dirty>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}
constexpr bool Has(Dirty value, Dirty bit) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(bit)) != 0;
}

enum class ActionKind { Create, Move, Hide, Show, Remove, Reorder };

struct Action {
    ActionKind kind{};
    TargetId target{};
    PhysicalRect rect{};
    std::uint32_t dpi{};
    std::uint64_t appearanceRevision{};
    bool rebuildAppearance{};
    bool topmost{};
};
using ActionBatch = std::vector<Action>;

struct WorkToken {
    TargetId target{};
    std::uint64_t appearanceRevision{};
    bool operator==(const WorkToken&) const = default;
};

struct TargetSnapshot {
    TargetId target;
    PhysicalRect rect;
    std::uint32_t dpi;
    std::uint64_t appearanceRevision;
    bool topmost;
    bool visible;
};

// Owner-thread reducer. Observations, not notification names, authorize all actions.
class TrackingModel final {
public:
    explicit TrackingModel(Policy policy);
    ActionBatch Observe(const Observation& observation, Dirty dirty, Milliseconds now,
                        std::optional<Milliseconds> createdAt = std::nullopt);
    ActionBatch Clear();
    bool Accepts(const WorkToken& token) const noexcept;
    std::optional<TargetSnapshot> CurrentSnapshot(const WorkToken& token) const noexcept;
    std::optional<TargetId> Current(WindowHandle hwnd) const;
    std::vector<WindowHandle> Handles() const;
    std::vector<WindowHandle> SettlingHandles() const;
    bool SealFixtureIdentity(const WindowIdentity& identity);
    std::size_t Size() const noexcept { return targets_.size(); }
    const Policy& GetPolicy() const noexcept { return policy_; }
    bool SelectionActive(WindowHandle hwnd) const noexcept;

private:
    struct Entry {
        TargetId id;
        AliveWindow snapshot;
        Eligibility eligibility{Eligibility::Eligible};
        Milliseconds bornAt{};
        std::optional<Milliseconds> lastSizeChange;
        std::uint64_t appearanceRevision{1};
        std::uint64_t publishedAppearanceRevision{};
        bool created{};
        bool shown{};
        bool orderPending{};
    };
    static Action MakeAction(ActionKind kind, const Entry& entry, bool rebuild = false);
    static void Hide(Entry& entry, ActionBatch& actions);
    Policy policy_;
    std::uint64_t nextGeneration_{1};
    std::unordered_map<WindowHandle, Entry> targets_;
    std::vector<WindowHandle> retiredSelections_;
    void RetireSelection(WindowHandle hwnd);
};

}
