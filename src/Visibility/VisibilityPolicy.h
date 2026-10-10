#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace window_sweaters::visibility {

using Milliseconds = std::uint64_t;
inline constexpr Milliseconds PollIntervalMs = 250;
inline constexpr std::size_t PollBudget = 32;
inline constexpr std::size_t MaxOverlays = 512;

struct TargetId {
    std::uintptr_t hwnd{};
    std::uint32_t pid{};
    std::uint64_t processStart{};
    std::uint64_t generation{};
    bool operator==(const TargetId&) const = default;
};

struct DesktopId {
    std::array<std::uint8_t, 16> bytes{};
    bool operator==(const DesktopId&) const = default;
    bool IsValid() const noexcept;
};

enum class SessionState { Unknown, Interactive, Locked, Disconnected };
enum class PowerState { Unknown, Awake, Suspended };
enum class DisplayState { Unknown, Off, On, Dimmed };
enum class Suppression {
    None, NotificationsUnavailable, Paused, SessionUnknown, Locked, Disconnected,
    PowerUnknown, Suspended, DisplayUnknown, DisplayOff, QueryUncertain,
    Hidden, Minimized, Cloaked, Maximized, Fullscreen, OffDesktop, OverlayUnassociated
};

// Copies have no connection to mutable native state and can be passed to workers.
class PolicySnapshot final {
public:
    std::uint64_t Revision() const noexcept { return revision_; }
    SessionState Session() const noexcept { return session_; }
    PowerState Power() const noexcept { return power_; }
    DisplayState Display() const noexcept { return display_; }
    bool Paused() const noexcept { return paused_; }
    bool NotificationsReady() const noexcept { return notificationsReady_; }
    Suppression Reason() const noexcept;
    bool MayWork() const noexcept { return Reason() == Suppression::None; }
    bool operator==(const PolicySnapshot&) const = default;

private:
    friend class VisibilityPolicy;
    std::uint64_t revision_{1};
    SessionState session_{SessionState::Unknown};
    PowerState power_{PowerState::Unknown};
    DisplayState display_{DisplayState::Unknown};
    bool paused_{};
    bool notificationsReady_{};
};

struct PolicyChange {
    PolicySnapshot snapshot;
    bool changed{};
    bool reconcile{};
};

class VisibilityPolicy final {
public:
    PolicySnapshot Snapshot() const noexcept { return snapshot_; }
    PolicyChange SetSession(SessionState state) noexcept;
    PolicyChange SetPower(PowerState state) noexcept;
    PolicyChange SetDisplay(DisplayState state) noexcept;
    PolicyChange SetPaused(bool paused) noexcept;
    PolicyChange SetNotificationsReady(bool ready) noexcept;
    PolicyChange Reset() noexcept;

private:
    PolicyChange Changed(const PolicySnapshot& previous) noexcept;
    PolicySnapshot snapshot_;
};

struct ProbeToken {
    TargetId target;
    std::uint64_t policyRevision{};
    std::uint64_t sequence{};
    bool operator==(const ProbeToken&) const = default;
};

enum class OverlayAssociation { Matching, CurrentDesktopVerified, Missing, Mismatched, RecreateRequired, Uncertain };
enum class OverlayDesktopCapability { NotQueried, Available, Unsupported };

struct WindowSnapshot {
    DesktopId desktop;
    bool onCurrentDesktop{};
    bool visible{};
    bool minimized{};
    bool cloaked{};
    bool maximized{};
    bool fullscreen{};
    OverlayAssociation overlay{OverlayAssociation::Uncertain};
    std::optional<DesktopId> overlayDesktop;
    OverlayDesktopCapability overlayDesktopCapability{OverlayDesktopCapability::NotQueried};
};

struct Observation {
    ProbeToken token;
    std::optional<WindowSnapshot> window;
    std::int32_t nativeError{};
};

struct Transition {
    bool accepted{};
    bool hide{};
    bool associate{};
    bool show{};
    Suppression reason{Suppression::QueryUncertain};
    ProbeToken token;
    std::optional<DesktopId> desktop;
    bool recreate{};
};

// One instance per tracking lifetime; the host still owns overlay creation/destruction.
class TargetVisibility final {
public:
    explicit TargetVisibility(TargetId target) noexcept : target_(target) {}
    Transition ApplyPolicy(const PolicySnapshot& policy) noexcept;
    ProbeToken BeginProbe(const PolicySnapshot& policy) noexcept;
    Transition Reconcile(const PolicySnapshot& policy, const Observation& observation) noexcept;
    Transition CompleteAssociation(const PolicySnapshot& policy, const ProbeToken& token,
                                   const DesktopId& desktop, bool success) noexcept;
    bool CanAssociate(const PolicySnapshot& policy, const ProbeToken& token,
                      const DesktopId& desktop) const noexcept;
    bool CanPublish(const PolicySnapshot& policy, const ProbeToken& token) const noexcept;
    bool Shown() const noexcept { return shown_; }
    TargetId Target() const noexcept { return target_; }

private:
    bool IsCurrent(const PolicySnapshot& policy, const ProbeToken& token) const noexcept;
    Transition Suppress(Suppression reason, bool accepted = true) noexcept;
    TargetId target_;
    std::uint64_t policyRevision_{};
    std::uint64_t sequence_{};
    bool shown_{};
    std::optional<DesktopId> pendingAssociation_;
};

// Host uses a round-robin batch of at most PollBudget targets; never a redraw timer.
class PollGate final {
public:
    bool Due(Milliseconds now, const PolicySnapshot& policy) noexcept;

private:
    std::optional<Milliseconds> lastPoll_;
    std::uint64_t policyRevision_{};
};

} // namespace window_sweaters::visibility
