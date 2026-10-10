#include "Startup.h"

#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>

namespace window_sweaters::startup
{
    namespace detail { std::shared_ptr<Backend> MakeNativeBackend(); }

    std::optional<bool> EffectiveEnabled(OsState state) noexcept
    {
        switch (state)
        {
        case OsState::Enabled:
        case OsState::EnabledByPolicy:
            return true;
        case OsState::Disabled:
        case OsState::DisabledByUser:
        case OsState::DisabledByPolicy:
            return false;
        default:
            return std::nullopt;
        }
    }

    Decision Decide(Command command, OsState state) noexcept
    {
        using Action = Decision::Action;
        if (state == OsState::UnavailableNoPackage)
            return {Action::ObserveOnly, {Outcome::UnavailableNoPackage, state}};
        if (state == OsState::Unknown)
            return {Action::ObserveOnly, {Outcome::Failed, state}};
        if (command == Command::Read)
            return {Action::ObserveOnly, {Outcome::Observed, state}};
        switch (state)
        {
        case OsState::DisabledByUser:
            return {Action::ObserveOnly, {Outcome::BlockedByUser, state}};
        case OsState::DisabledByPolicy:
        case OsState::EnabledByPolicy:
            return {Action::ObserveOnly, {Outcome::BlockedByPolicy, state}};
        case OsState::Enabled:
            return {command == Command::Disable ? Action::Disable : Action::ObserveOnly,
                {command == Command::Disable ? Outcome::NotApplied : Outcome::AlreadyEffective, state}};
        case OsState::Disabled:
            return {command == Command::Enable ? Action::RequestEnable : Action::ObserveOnly,
                {command == Command::Enable ? Outcome::NotApplied : Outcome::AlreadyEffective, state}};
        default:
            return {Action::ObserveOnly, {Outcome::Failed, OsState::Unknown}};
        }
    }

    Result AfterMutation(Command command, OsState state) noexcept
    {
        auto const decision = Decide(command, state);
        if (decision.result.outcome == Outcome::AlreadyEffective &&
            decision.action == Decision::Action::ObserveOnly)
            return {Outcome::Applied, state};
        if (decision.action != Decision::Action::ObserveOnly)
            return {Outcome::NotApplied, state};
        return decision.result;
    }

    struct Controller::Shared
    {
        struct Active
        {
            std::uint64_t generation;
            bool cancelled{};
            std::shared_ptr<PendingOperation> operation;
        };
        mutable std::mutex mutex;
        Snapshot snapshot;
        std::uint64_t nextGeneration{};
        std::optional<Active> active;
        std::optional<Completion> mailbox;
    };

    Controller::Controller(Consumer consumer, DesiredPreference savedDesired)
        : Controller(detail::MakeNativeBackend(), consumer, savedDesired) {}

    Controller::Controller(std::shared_ptr<Backend> backend, Consumer consumer,
        DesiredPreference savedDesired)
        : shared_(std::make_shared<Shared>()), backend_(std::move(backend)), consumer_(consumer)
    {
        if (!backend_)
            throw std::invalid_argument("Startup backend must not be null");
        shared_->snapshot.savedDesired = savedDesired;
    }

    Controller::~Controller() { Shutdown(); }

    Submission Controller::ReadState() { return Submit(Command::Read); }

    Submission Controller::RequestUserChange(DesiredPreference desired, UserIntent intent)
    {
        if (consumer_ != Consumer::SettingsUserActions)
            return {SubmissionStatus::ReadOnly};
        if (intent != UserIntent::SettingsGesture)
            return {SubmissionStatus::ExplicitIntentRequired};
        return Submit(desired == DesiredPreference::On ? Command::Enable : Command::Disable);
    }

    Submission Controller::Submit(Command command)
    {
        auto const shared = shared_;
        std::uint64_t generation{};
        {
            std::lock_guard lock(shared->mutex);
            if (shared->snapshot.stopped)
                return {SubmissionStatus::Stopped};
            if (shared->active)
                return {SubmissionStatus::Busy};
            if (shared->mailbox)
                return {SubmissionStatus::CompletionPending};
            if (shared->nextGeneration == std::numeric_limits<std::uint64_t>::max())
                return {SubmissionStatus::GenerationExhausted};
            generation = ++shared->nextGeneration;
            shared->active.emplace(Shared::Active{generation, false, {}});
            shared->snapshot.pendingGeneration = generation;
            shared->snapshot.cancellationRequested = false;
        }
        auto completion = [weak = std::weak_ptr<Shared>(shared), generation, command](Result result)
        {
            auto const state = weak.lock();
            if (!state)
                return;
            std::lock_guard lock(state->mutex);
            if (state->snapshot.stopped || !state->active ||
                state->active->generation != generation)
                return;
            if (state->active->cancelled)
                result = {Outcome::Cancelled, OsState::Unknown, result.error};
            Completion const completed{generation, command, result};
            state->snapshot.lastObservation = completed;
            state->snapshot.pendingGeneration.reset();
            state->snapshot.cancellationRequested = false;
            state->mailbox = completed;
            state->active.reset();
        };

        std::shared_ptr<PendingOperation> operation;
        try
        {
            operation = backend_->Begin(command, completion);
            if (!operation)
                completion({Outcome::Failed, OsState::Unknown, static_cast<std::int32_t>(0x80004005u)});
        }
        catch (std::bad_alloc const&)
        {
            completion({Outcome::Failed, OsState::Unknown, static_cast<std::int32_t>(0x8007000Eu)});
        }
        bool cancel{};
        {
            std::lock_guard lock(shared->mutex);
            if (shared->active && shared->active->generation == generation)
            {
                shared->active->operation = operation;
                cancel = shared->active->cancelled;
            }
            else
                cancel = true;
        }
        if (cancel && operation)
            operation->Cancel();
        return {SubmissionStatus::Accepted, generation};
    }

    void Controller::SetSavedPreference(DesiredPreference committedPreference) noexcept
    {
        std::lock_guard lock(shared_->mutex);
        shared_->snapshot.savedDesired = committedPreference;
    }

    Snapshot Controller::GetSnapshot() const noexcept
    {
        std::lock_guard lock(shared_->mutex);
        return shared_->snapshot;
    }

    std::optional<Completion> Controller::TakeCompletion() noexcept
    {
        std::lock_guard lock(shared_->mutex);
        auto const result = shared_->mailbox;
        shared_->mailbox.reset();
        return result;
    }

    void Controller::CancelPending() noexcept
    {
        std::shared_ptr<PendingOperation> operation;
        {
            std::lock_guard lock(shared_->mutex);
            if (!shared_->active || shared_->active->cancelled)
                return;
            shared_->active->cancelled = true;
            shared_->snapshot.cancellationRequested = true;
            operation = shared_->active->operation;
        }
        if (operation)
            operation->Cancel();
    }

    void Controller::Shutdown() noexcept
    {
        std::shared_ptr<PendingOperation> operation;
        {
            std::lock_guard lock(shared_->mutex);
            if (shared_->snapshot.stopped)
                return;
            shared_->snapshot.stopped = true;
            shared_->snapshot.pendingGeneration.reset();
            shared_->snapshot.cancellationRequested = false;
            shared_->mailbox.reset();
            if (shared_->active)
                operation = std::move(shared_->active->operation);
            shared_->active.reset();
        }
        if (operation)
            operation->Cancel();
    }
}
