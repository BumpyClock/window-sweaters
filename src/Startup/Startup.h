#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace window_sweaters::startup
{
    inline constexpr wchar_t TaskId[] = L"WindowSweatersStartup";

    enum class DesiredPreference { Off, On };
    enum class OsState
    {
        Unknown,
        UnavailableNoPackage,
        Disabled,
        DisabledByUser,
        DisabledByPolicy,
        Enabled,
        EnabledByPolicy
    };
    enum class Command { Read, Enable, Disable };
    enum class Outcome
    {
        Observed,
        Applied,
        AlreadyEffective,
        UnavailableNoPackage,
        BlockedByUser,
        BlockedByPolicy,
        NotApplied,
        Failed,
        Cancelled
    };
    struct Result
    {
        Outcome outcome{Outcome::Failed};
        OsState state{OsState::Unknown};
        std::int32_t error{}; // HRESULT; zero does not imply that a request was applied.
    };
    struct Decision
    {
        enum class Action { ObserveOnly, RequestEnable, Disable };
        Action action{Action::ObserveOnly};
        Result result; // Current observation, not a predicted mutation result.
    };
    [[nodiscard]] Decision Decide(Command command, OsState state) noexcept;
    [[nodiscard]] std::optional<bool> EffectiveEnabled(OsState state) noexcept;
    [[nodiscard]] Result AfterMutation(Command command, OsState state) noexcept;

    class PendingOperation
    {
    public:
        virtual ~PendingOperation() = default;
        virtual void Cancel() noexcept = 0;
    };

    // A startup-specific seam. Begin is nonblocking; its callback can complete inline.
    // Completion must be terminal; cancellation does not free the active slot until completion.
    // Begin may throw std::bad_alloc, other failures are returned through completion.
    class Backend
    {
    public:
        using Callback = std::function<void(Result)>;
        virtual ~Backend() = default;
        [[nodiscard]] virtual std::shared_ptr<PendingOperation> Begin(
            Command command, Callback completion) = 0;
    };

    enum class Consumer { HostReadOnly, SettingsUserActions };
    enum class UserIntent { None, SettingsGesture };
    enum class SubmissionStatus
    {
        Accepted,
        Busy,
        CompletionPending,
        ReadOnly,
        ExplicitIntentRequired,
        Stopped,
        GenerationExhausted
    };
    struct Submission
    {
        SubmissionStatus status{SubmissionStatus::Stopped};
        std::uint64_t generation{};
    };
    struct Completion
    {
        std::uint64_t generation{};
        Command command{Command::Read};
        Result result;
    };
    struct Snapshot
    {
        DesiredPreference savedDesired{DesiredPreference::Off};
        std::optional<Completion> lastObservation;
        std::optional<std::uint64_t> pendingGeneration;
        bool cancellationRequested{};
        bool stopped{};
    };

    class Controller final
    {
    public:
        explicit Controller(Consumer consumer,
            DesiredPreference savedDesired = DesiredPreference::Off);
        // Injection is for isolated tests; native creation exposes no direct mutation backend.
        explicit Controller(std::shared_ptr<Backend> backend, Consumer consumer,
            DesiredPreference savedDesired = DesiredPreference::Off);
        ~Controller();
        Controller(Controller const&) = delete;
        Controller& operator=(Controller const&) = delete;
        Controller(Controller&&) = delete;
        Controller& operator=(Controller&&) = delete;

        [[nodiscard]] Submission ReadState();
        [[nodiscard]] Submission RequestUserChange(DesiredPreference desired, UserIntent intent);
        void SetSavedPreference(DesiredPreference committedPreference) noexcept;
        [[nodiscard]] Snapshot GetSnapshot() const noexcept;
        [[nodiscard]] std::optional<Completion> TakeCompletion() noexcept;
        void CancelPending() noexcept;
        void Shutdown() noexcept;

    private:
        struct Shared;
        [[nodiscard]] Submission Submit(Command command);
        std::shared_ptr<Shared> shared_;
        std::shared_ptr<Backend> backend_;
        Consumer consumer_;
    };
}
