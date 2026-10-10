#include "Startup.h"

#include <windows.h>
#include <appmodel.h>
#include <roapi.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

#include <atomic>
#include <mutex>
#include <new>
#include <string_view>

static_assert(std::string_view{CPPWINRT_VERSION} == "2.0.240405.15",
    "Startup requires the pinned Microsoft.Windows.CppWinRT 2.0.240405.15 projection.");

namespace window_sweaters::startup
{
    namespace
    {
        using winrt::Windows::ApplicationModel::StartupTask;
        using winrt::Windows::ApplicationModel::StartupTaskState;
        using winrt::Windows::Foundation::AsyncStatus;
        using winrt::Windows::Foundation::IAsyncInfo;

        class Apartment final
        {
        public:
            Apartment() { winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED)); }
            ~Apartment() { RoUninitialize(); }
            Apartment(Apartment const&) = delete;
            Apartment& operator=(Apartment const&) = delete;
        };

        OsState Translate(StartupTaskState state) noexcept
        {
            switch (state)
            {
            case StartupTaskState::Disabled: return OsState::Disabled;
            case StartupTaskState::DisabledByUser: return OsState::DisabledByUser;
            case StartupTaskState::DisabledByPolicy: return OsState::DisabledByPolicy;
            case StartupTaskState::Enabled: return OsState::Enabled;
            case StartupTaskState::EnabledByPolicy: return OsState::EnabledByPolicy;
            default: return OsState::Unknown;
            }
        }

        class NativeOperation final : public PendingOperation,
            public std::enable_shared_from_this<NativeOperation>
        {
        public:
            NativeOperation(Command command, Backend::Callback completion)
                : command_(command), completion_(std::move(completion)) {}

            ~NativeOperation() override
            {
                current_ = nullptr;
                if (mtaCookie_)
                {
                    auto const result = CoDecrementMTAUsage(mtaCookie_);
                    if (FAILED(result))
                        OutputDebugStringW(L"WindowSweaters.Startup: CoDecrementMTAUsage failed.\n");
                }
            }

            void Cancel() noexcept override
            {
                cancelled_.store(true);
                IAsyncInfo operation{nullptr};
                {
                    std::lock_guard lock(mutex_);
                    operation = current_;
                }
                if (operation)
                {
                    try { operation.Cancel(); }
                    catch (winrt::hresult_error const& error)
                    {
                        // Keep the slot occupied until the underlying async operation completes.
                        cancellationError_.store(error.code().value);
                    }
                }
            }

            void Start() noexcept
            {
                Guard([this]
                {
                    if (Cancelled())
                        return;
                    UINT32 length{};
                    LONG const identity = GetCurrentPackageFullName(&length, nullptr);
                    if (identity == APPMODEL_ERROR_NO_PACKAGE)
                    {
                        Finish({Outcome::UnavailableNoPackage, OsState::UnavailableNoPackage});
                        return;
                    }
                    if (identity != ERROR_INSUFFICIENT_BUFFER)
                    {
                        Finish({Outcome::Failed, OsState::Unknown,
                            identity == ERROR_SUCCESS ? E_UNEXPECTED : HRESULT_FROM_WIN32(identity)});
                        return;
                    }
                    // Lookup and enable span worker callbacks; keep the MTA alive between them.
                    winrt::check_hresult(CoIncrementMTAUsage(&mtaCookie_));
                    Apartment apartment;
                    auto const lookup = StartupTask::GetAsync(TaskId);
                    Track(lookup);
                    lookup.Completed([self = shared_from_this()](auto const& operation, AsyncStatus status)
                    {
                        self->Guard([&]
                        {
                            Apartment callbackApartment;
                            if (self->Cancelled())
                                return;
                            if (status == AsyncStatus::Canceled)
                            {
                                self->Finish({Outcome::Cancelled, OsState::Unknown});
                                return;
                            }
                            if (status == AsyncStatus::Error)
                            {
                                self->Finish({Outcome::Failed, OsState::Unknown, operation.ErrorCode().value});
                                return;
                            }
                            if (status != AsyncStatus::Completed)
                            {
                                self->Finish({Outcome::Failed, OsState::Unknown, E_UNEXPECTED});
                                return;
                            }
                            self->OnTask(operation.GetResults());
                        });
                    });
                });
            }

            void Fail(std::int32_t error) noexcept { Finish({Outcome::Failed, OsState::Unknown, error}); }

        private:
            template<class Function>
            void Guard(Function&& function) noexcept
            {
                try { function(); }
                catch (winrt::hresult_error const& error)
                {
                    Finish({error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
                        error.code() == E_ABORT ? Outcome::Cancelled : Outcome::Failed,
                        OsState::Unknown, error.code().value});
                }
                catch (std::bad_alloc const&) { Fail(E_OUTOFMEMORY); }
            }

            bool Cancelled() noexcept
            {
                if (!cancelled_.load())
                    return false;
                Finish({Outcome::Cancelled, OsState::Unknown, cancellationError_.load()});
                return true;
            }

            void Track(IAsyncInfo const& operation)
            {
                {
                    std::lock_guard lock(mutex_);
                    current_ = operation;
                }
                if (cancelled_.load())
                {
                    try { operation.Cancel(); }
                    catch (winrt::hresult_error const& error) { cancellationError_.store(error.code().value); }
                }
            }

            void OnTask(StartupTask const& task)
            {
                if (Cancelled())
                    return;
                auto const decision = Decide(command_, Translate(task.State()));
                if (decision.action == Decision::Action::ObserveOnly)
                {
                    Finish(decision.result);
                    return;
                }
                if (Cancelled())
                    return;
                if (decision.action == Decision::Action::Disable)
                {
                    task.Disable();
                    Finish(AfterMutation(command_, Translate(task.State())));
                    return;
                }
                // Packaged Win32 has no UWP consent dialog/UI-thread requirement.
                // Only SettingsUserActions + SettingsGesture reaches this through Controller.
                auto const enable = task.RequestEnableAsync();
                Track(enable);
                enable.Completed([self = shared_from_this()](auto const& operation, AsyncStatus status)
                {
                    self->Guard([&]
                    {
                        Apartment callbackApartment;
                        if (self->Cancelled())
                            return;
                        if (status == AsyncStatus::Canceled)
                            self->Finish({Outcome::Cancelled, OsState::Unknown});
                        else if (status == AsyncStatus::Error)
                            self->Finish({Outcome::Failed, OsState::Unknown, operation.ErrorCode().value});
                        else if (status == AsyncStatus::Completed)
                            self->Finish(AfterMutation(self->command_, Translate(operation.GetResults())));
                        else
                            self->Fail(E_UNEXPECTED);
                    });
                });
            }

            void Finish(Result result) noexcept
            {
                Backend::Callback completion;
                {
                    std::lock_guard lock(mutex_);
                    if (finished_)
                        return;
                    finished_ = true;
                    current_ = nullptr;
                    completion = std::move(completion_);
                }
                if (completion)
                    completion(result);
            }

            Command command_;
            Backend::Callback completion_;
            std::mutex mutex_;
            IAsyncInfo current_{nullptr};
            std::atomic<bool> cancelled_{false};
            std::atomic<std::int32_t> cancellationError_{};
            CO_MTA_USAGE_COOKIE mtaCookie_{};
            bool finished_{};
        };

        class NativeBackend final : public Backend
        {
        public:
            std::shared_ptr<PendingOperation> Begin(Command command, Callback completion) override
            {
                auto operation = std::make_shared<NativeOperation>(command, std::move(completion));
                auto context = std::make_unique<std::shared_ptr<NativeOperation>>(operation);
                if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* raw)
                    {
                        auto const owner = std::unique_ptr<std::shared_ptr<NativeOperation>>(
                            static_cast<std::shared_ptr<NativeOperation>*>(raw));
                        (*owner)->Start();
                    }, context.get(), nullptr))
                {
                    operation->Fail(HRESULT_FROM_WIN32(GetLastError()));
                }
                else
                    (void)context.release();
                return operation;
            }
        };
    }

    namespace detail
    {
        std::shared_ptr<Backend> MakeNativeBackend() { return std::make_shared<NativeBackend>(); }
    }
}
