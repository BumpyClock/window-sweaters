#include "..\..\src\Startup\Startup.h"

#include <windows.h>
#include <appmodel.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

using namespace window_sweaters::startup;

namespace
{
    int checks{};

    void Check(bool condition, char const* description)
    {
        ++checks;
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", description);
            std::exit(1);
        }
    }

    class FakeOperation final : public PendingOperation
    {
    public:
        void Cancel() noexcept override { ++cancelCalls; }
        unsigned cancelCalls{};
    };

    class FakeBackend final : public Backend
    {
    public:
        struct Request
        {
            Command command;
            Callback completion;
            std::shared_ptr<FakeOperation> operation;
        };

        std::shared_ptr<PendingOperation> Begin(Command command, Callback completion) override
        {
            if (throwAllocation)
                throw std::bad_alloc();
            auto operation = std::make_shared<FakeOperation>();
            requests.push_back({command, std::move(completion), operation});
            if (inlineResult)
                requests.back().completion(*inlineResult);
            if (beforeReturn)
                beforeReturn();
            if (nullOperation)
                return {};
            return operation;
        }

        void Complete(std::size_t index, Result result) { requests.at(index).completion(result); }

        void Apply(std::size_t index)
        {
            auto const& request = requests.at(index);
            auto const decision = Decide(request.command, state);
            Result result = decision.result;
            if (decision.action != Decision::Action::ObserveOnly)
            {
                ++mutationCalls;
                state = request.command == Command::Enable ? OsState::Enabled : OsState::Disabled;
                result = AfterMutation(request.command, state);
            }
            request.completion(result);
        }

        OsState state{OsState::Disabled};
        unsigned mutationCalls{};
        bool throwAllocation{};
        bool nullOperation{};
        std::function<void()> beforeReturn;
        std::optional<Result> inlineResult;
        std::vector<Request> requests;
    };

    struct FakePreferenceStore
    {
        DesiredPreference committed{DesiredPreference::Off};
        bool fail{};
        bool Save(DesiredPreference desired) noexcept
        {
            if (fail)
                return false;
            committed = desired;
            return true;
        }
    };

    Completion Take(Controller& controller)
    {
        auto const completion = controller.TakeCompletion();
        Check(completion.has_value(), "Expected one completion");
        return *completion;
    }

    void PolicyMatrix()
    {
        struct Row
        {
            OsState state;
            Outcome read;
            bool enableMutation;
            Outcome enable;
            bool disableMutation;
            Outcome disable;
            std::optional<bool> effective;
        };
        std::array const rows{
            Row{OsState::Unknown, Outcome::Failed, false, Outcome::Failed,
                false, Outcome::Failed, std::nullopt},
            Row{OsState::UnavailableNoPackage, Outcome::UnavailableNoPackage,
                false, Outcome::UnavailableNoPackage, false, Outcome::UnavailableNoPackage, std::nullopt},
            Row{OsState::Disabled, Outcome::Observed, true, Outcome::NotApplied,
                false, Outcome::AlreadyEffective, false},
            Row{OsState::DisabledByUser, Outcome::Observed, false, Outcome::BlockedByUser,
                false, Outcome::BlockedByUser, false},
            Row{OsState::DisabledByPolicy, Outcome::Observed, false, Outcome::BlockedByPolicy,
                false, Outcome::BlockedByPolicy, false},
            Row{OsState::Enabled, Outcome::Observed, false, Outcome::AlreadyEffective,
                true, Outcome::NotApplied, true},
            Row{OsState::EnabledByPolicy, Outcome::Observed, false, Outcome::BlockedByPolicy,
                false, Outcome::BlockedByPolicy, true}};
        for (auto const& row : rows)
        {
            auto const read = Decide(Command::Read, row.state);
            Check(read.action == Decision::Action::ObserveOnly && read.result.outcome == row.read &&
                read.result.state == row.state,
                "Read preserves every OS status and never mutates");
            auto const enable = Decide(Command::Enable, row.state);
            Check(enable.action == (row.enableMutation ? Decision::Action::RequestEnable : Decision::Action::ObserveOnly) &&
                enable.result.outcome == row.enable,
                "Enable honors user and policy ownership");
            auto const disable = Decide(Command::Disable, row.state);
            Check(disable.action == (row.disableMutation ? Decision::Action::Disable : Decision::Action::ObserveOnly) &&
                disable.result.outcome == row.disable,
                "Disable honors user and policy ownership");
            Check(EffectiveEnabled(row.state) == row.effective, "Effective state is not a saved preference");
        }
        Check(AfterMutation(Command::Enable, OsState::Enabled).outcome == Outcome::Applied,
            "Enable requires effective Enabled");
        Check(AfterMutation(Command::Disable, OsState::Disabled).outcome == Outcome::Applied,
            "Disable requires effective Disabled");
        Check(AfterMutation(Command::Enable, OsState::Disabled).outcome == Outcome::NotApplied,
            "OS rejection is not successful enable");
        Check(AfterMutation(Command::Disable, OsState::Enabled).outcome == Outcome::NotApplied,
            "Unchanged enabled state is not successful disable");
        Check(AfterMutation(Command::Enable, OsState::DisabledByUser).outcome == Outcome::BlockedByUser,
            "User disable during request remains visible");
        Check(AfterMutation(Command::Enable, OsState::EnabledByPolicy).outcome == Outcome::BlockedByPolicy,
            "Policy enable is not misreported as application-owned");
        Check(AfterMutation(Command::Disable, OsState::DisabledByPolicy).outcome == Outcome::BlockedByPolicy,
            "Policy disable is not misreported as application-owned");
    }

    void IntentAndSavedPreference()
    {
        auto backend = std::make_shared<FakeBackend>();
        FakePreferenceStore store{DesiredPreference::On};
        Controller host(backend, Consumer::HostReadOnly, store.committed);
        Check(backend->requests.empty(), "Construction with saved On never enables startup");
        Check(host.GetSnapshot().savedDesired == DesiredPreference::On &&
            !host.GetSnapshot().lastObservation, "No successful default observation");
        Check(host.RequestUserChange(DesiredPreference::On, UserIntent::SettingsGesture).status ==
            SubmissionStatus::ReadOnly, "Host cannot use Settings gesture marker to mutate");
        Check(host.ReadState().status == SubmissionStatus::Accepted, "Host may read");
        backend->Apply(0);
        auto completion = Take(host);
        Check(completion.result.state == OsState::Disabled &&
            host.GetSnapshot().savedDesired == DesiredPreference::On && backend->mutationCalls == 0,
            "Saved On plus effective Disabled remains distinct on launch/read");

        Controller settings(backend, Consumer::SettingsUserActions, store.committed);
        Check(settings.RequestUserChange(DesiredPreference::On, UserIntent::None).status ==
            SubmissionStatus::ExplicitIntentRequired, "No launch/default/reconnect intent can enable");
        Check(backend->requests.size() == 1, "Rejected intent does not reach backend");
        Check(settings.RequestUserChange(DesiredPreference::On, UserIntent::SettingsGesture).status ==
            SubmissionStatus::Accepted, "Explicit enable is accepted");
        backend->Apply(1);
        Check(Take(settings).result.outcome == Outcome::Applied && backend->state == OsState::Enabled,
            "Isolated backend enables on gesture");
        Check(settings.GetSnapshot().savedDesired == DesiredPreference::On,
            "OS completion does not write preference");

        Check(settings.RequestUserChange(DesiredPreference::Off, UserIntent::SettingsGesture).status ==
            SubmissionStatus::Accepted, "Explicit disable is accepted");
        backend->Apply(2);
        Check(Take(settings).result.outcome == Outcome::Applied && backend->state == OsState::Disabled,
            "Isolated enable/disable round trip is reversible");
        Check(settings.GetSnapshot().savedDesired == DesiredPreference::On,
            "Desired remains committed On until persistence succeeds");
        store.fail = true;
        Check(!store.Save(DesiredPreference::Off), "Isolated save failure is not success");
        Check(settings.GetSnapshot().savedDesired == DesiredPreference::On, "Failed save cannot alter committed view");
        store.fail = false;
        Check(store.Save(DesiredPreference::Off), "Isolated save succeeds");
        settings.SetSavedPreference(store.committed);
        Check(settings.GetSnapshot().savedDesired == DesiredPreference::Off, "Committed preference can be mirrored");
        Controller reopened(backend, Consumer::SettingsUserActions, store.committed);
        Check(reopened.GetSnapshot().savedDesired == DesiredPreference::Off &&
            backend->requests.size() == 3, "Reopen restores saved preference with no OS action");
    }

    void RepeatedRequestsAndOverrides()
    {
        auto backend = std::make_shared<FakeBackend>();
        Controller settings(backend, Consumer::SettingsUserActions);
        for (int repeat = 0; repeat != 16; ++repeat)
        {
            Check(settings.RequestUserChange(DesiredPreference::On, UserIntent::SettingsGesture).status ==
                SubmissionStatus::Accepted, "Repeated explicit request accepted serially");
            backend->Apply(static_cast<std::size_t>(repeat));
            Check(Take(settings).result.outcome == (repeat == 0 ? Outcome::Applied : Outcome::AlreadyEffective),
                "Repeated enable is idempotent");
        }
        Check(backend->mutationCalls == 1, "One enable mutation despite repeated requests");
        std::array const blockedStates{
            OsState::DisabledByUser, OsState::DisabledByPolicy, OsState::EnabledByPolicy,
            OsState::UnavailableNoPackage};
        for (auto const state : blockedStates)
        {
            backend->state = state;
            for (auto const desired : {DesiredPreference::On, DesiredPreference::Off})
            {
                auto const index = backend->requests.size();
                Check(settings.RequestUserChange(desired, UserIntent::SettingsGesture).status ==
                    SubmissionStatus::Accepted, "Explicit request observes block");
                backend->Apply(index);
                auto const completion = Take(settings);
                auto const expected = state == OsState::DisabledByUser ? Outcome::BlockedByUser :
                    state == OsState::UnavailableNoPackage ? Outcome::UnavailableNoPackage : Outcome::BlockedByPolicy;
                Check(completion.result.outcome == expected && completion.result.state == state,
                    "Blocked effective status remains exact");
                Check(backend->mutationCalls == 1 && backend->state == state, "Blocked OS state is never overridden");
            }
        }
    }

    void BoundedMailboxAndStaleResults()
    {
        auto backend = std::make_shared<FakeBackend>();
        Controller controller(backend, Consumer::SettingsUserActions);
        auto const first = controller.ReadState();
        Check(first.status == SubmissionStatus::Accepted && first.generation == 1, "First generation admitted");
        for (int i = 0; i != 10000; ++i)
            Check(controller.ReadState().status == SubmissionStatus::Busy, "No queued work while active");
        Check(backend->requests.size() == 1, "Bounded one in-flight operation");
        backend->Complete(0, {Outcome::Observed, OsState::Enabled});
        for (int i = 0; i != 10000; ++i)
            Check(controller.ReadState().status == SubmissionStatus::CompletionPending,
                "Unconsumed completion applies backpressure");
        Check(backend->requests.size() == 1, "Mailbox cannot grow an unbounded queue");
        Check(Take(controller).generation == first.generation, "First completion preserved");
        Check(!controller.TakeCompletion(), "Completion consumed exactly once");
        auto const second = controller.ReadState();
        Check(second.status == SubmissionStatus::Accepted && second.generation == 2, "Generation increases");
        backend->Complete(0, {Outcome::Observed, OsState::DisabledByPolicy});
        Check(controller.GetSnapshot().pendingGeneration == second.generation &&
            controller.GetSnapshot().lastObservation->result.state == OsState::Enabled &&
            !controller.TakeCompletion(), "Duplicate/out-of-order first callback cannot clobber second");
        backend->Complete(1, {Outcome::Observed, OsState::DisabledByUser});
        Check(Take(controller).result.state == OsState::DisabledByUser, "Current generation applied");
        backend->Complete(0, {Outcome::Observed, OsState::EnabledByPolicy});
        Check(controller.GetSnapshot().lastObservation->result.state == OsState::DisabledByUser &&
            !controller.TakeCompletion(), "Old callback cannot rewrite final observation");
    }

    void CancellationAndShutdown()
    {
        auto backend = std::make_shared<FakeBackend>();
        Controller controller(backend, Consumer::SettingsUserActions, DesiredPreference::On);
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Cancellation read admitted");
        controller.CancelPending();
        controller.CancelPending();
        Check(backend->requests[0].operation->cancelCalls == 1 &&
            controller.GetSnapshot().cancellationRequested,             "Cancellation forwarded idempotently without blocking");
        Check(controller.ReadState().status == SubmissionStatus::Busy, "Cancelled OS work still owns active slot");
        backend->Complete(0, {Outcome::Observed, OsState::Enabled});
        Check(Take(controller).result.outcome == Outcome::Cancelled &&
            controller.GetSnapshot().lastObservation->result.state == OsState::Unknown,
            "Cancelled late success drops effective state");
        Check(controller.GetSnapshot().savedDesired == DesiredPreference::On, "Cancel never undoes saved intent");
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "New operation only after terminal callback");
        controller.Shutdown();
        controller.Shutdown();
        Check(backend->requests[1].operation->cancelCalls == 1, "Idempotent shutdown cancels once");
        backend->Complete(1, {Outcome::Observed, OsState::Enabled});
        Check(!controller.TakeCompletion() && controller.GetSnapshot().stopped &&
            !controller.GetSnapshot().pendingGeneration, "Shutdown safely drops late callback/mailbox");
        Check(controller.ReadState().status == SubmissionStatus::Stopped, "Stopped controller accepts no read");
        Check(controller.RequestUserChange(DesiredPreference::Off, UserIntent::SettingsGesture).status ==
            SubmissionStatus::Stopped, "Stopped controller accepts no mutation");

        {
            Controller destroyed(backend, Consumer::HostReadOnly);
            Check(destroyed.ReadState().status == SubmissionStatus::Accepted, "Destruction read admitted");
        }
        Check(backend->requests[2].operation->cancelCalls == 1, "Destructor requests cancellation");
        backend->Complete(2, {Outcome::Observed, OsState::Enabled});
        Check(true, "Callback after controller destruction holds no dangling owner");
    }

    void FailuresAndInlineCompletion()
    {
        auto backend = std::make_shared<FakeBackend>();
        Controller controller(backend, Consumer::SettingsUserActions, DesiredPreference::On);
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Failure read admitted");
        backend->Complete(0, {Outcome::Failed, OsState::Unknown, static_cast<std::int32_t>(0x80070005u)});
        auto const failed = Take(controller);
        Check(failed.result.outcome == Outcome::Failed && failed.result.error == static_cast<std::int32_t>(0x80070005u) &&
            !EffectiveEnabled(failed.result.state), "HRESULT failure is visible, not successful Disabled");
        Check(controller.GetSnapshot().savedDesired == DesiredPreference::On, "Failure does not rewrite desired");
        backend->throwAllocation = true;
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Submission allocation failure gets completion");
        auto const allocation = Take(controller);
        Check(allocation.result.outcome == Outcome::Failed &&
            allocation.result.error == static_cast<std::int32_t>(0x8007000Eu), "Allocation failure releases active slot");
        backend->throwAllocation = false;
        backend->inlineResult = Result{Outcome::Observed, OsState::Enabled};
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Inline completion does not deadlock");
        Check(Take(controller).result.state == OsState::Enabled &&
            !controller.GetSnapshot().pendingGeneration, "Inline completion stays terminal");
        backend->inlineResult.reset();
        backend->nullOperation = true;
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Null backend operation accepted for result");
        Check(Take(controller).result.outcome == Outcome::Failed, "Null operation cannot leave controller stuck");

        auto otherBackend = std::make_shared<FakeBackend>();
        Controller second(otherBackend, Consumer::HostReadOnly);
        Check(second.ReadState().status == SubmissionStatus::Accepted, "OS-side cancel read admitted");
        otherBackend->Complete(0, {Outcome::Cancelled, OsState::Unknown});
        Check(Take(second).result.outcome == Outcome::Cancelled, "OS cancellation distinct from failure");
    }

    void ConcurrentAdmission()
    {
        auto backend = std::make_shared<FakeBackend>();
        Controller controller(backend, Consumer::HostReadOnly);
        std::atomic<unsigned> accepted{};
        std::array<std::thread, 16> threads;
        for (auto& thread : threads)
            thread = std::thread([&]
            {
                if (controller.ReadState().status == SubmissionStatus::Accepted)
                    ++accepted;
            });
        for (auto& thread : threads)
            thread.join();
        Check(accepted.load() == 1 && backend->requests.size() == 1, "Concurrent producers admit only one operation");
        backend->Complete(0, {Outcome::Observed, OsState::Disabled});
        Check(Take(controller).result.state == OsState::Disabled, "Concurrent admission completes normally");
    }

    void CancellationDuringSubmission()
    {
        auto backend = std::make_shared<FakeBackend>();
        Controller controller(backend, Consumer::HostReadOnly);
        backend->beforeReturn = [&] { controller.CancelPending(); };
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Cancel before handle installation accepted");
        Check(backend->requests[0].operation->cancelCalls == 1 &&
            controller.ReadState().status == SubmissionStatus::Busy,
            "Late-installed handle receives cancel and still holds bounded slot");
        backend->Complete(0, {Outcome::Observed, OsState::Enabled});
        Check(Take(controller).result.outcome == Outcome::Cancelled, "Cancel during submission drops stale success");
        backend->beforeReturn = [&] { controller.Shutdown(); };
        Check(controller.ReadState().status == SubmissionStatus::Accepted, "Shutdown during accepted submission is safe");
        Check(backend->requests[1].operation->cancelCalls == 1, "Handle returned after shutdown is cancelled");
        backend->Complete(1, {Outcome::Observed, OsState::Enabled});
        Check(!controller.TakeCompletion() && controller.ReadState().status == SubmissionStatus::Stopped,
            "Handle returned after shutdown cannot revive controller");
    }

    void NativeReadOnlyUnpackaged()
    {
        UINT32 length{};
        Check(GetCurrentPackageFullName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE,
            "Native test must run unpackaged; never mutate real StartupTask state");
        Controller native(Consumer::HostReadOnly, DesiredPreference::On);
        Check(native.ReadState().status == SubmissionStatus::Accepted, "Native read is asynchronous");
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::optional<Completion> completion;
        while (!(completion = native.TakeCompletion()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(completion.has_value(), "Native unpackaged read completes within test deadline");
        Check(completion->result.outcome == Outcome::UnavailableNoPackage &&
            completion->result.state == OsState::UnavailableNoPackage && completion->result.error == 0,
            "Native read reports UnavailableNoPackage without registry fallback");
        Check(native.GetSnapshot().savedDesired == DesiredPreference::On, "Native read does not act on desired On");
        native.Shutdown();
    }
}

int main()
{
    PolicyMatrix();
    IntentAndSavedPreference();
    RepeatedRequestsAndOverrides();
    BoundedMailboxAndStaleResults();
    CancellationAndShutdown();
    FailuresAndInlineCompletion();
    ConcurrentAdmission();
    CancellationDuringSubmission();
    NativeReadOnlyUnpackaged();
    std::printf("PASS: %d startup checks (fake mutations only; native read unpackaged).\n", checks);
    return 0;
}
