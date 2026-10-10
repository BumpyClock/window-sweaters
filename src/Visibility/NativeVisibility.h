#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include "VisibilityPolicy.h"

#include <unordered_map>

namespace window_sweaters::visibility {

struct StartResult {
    HRESULT notifications{E_UNEXPECTED};
    HRESULT virtualDesktop{E_UNEXPECTED};
};

struct MessageResult {
    bool handled{};
    PolicyChange policy;
};

struct NativeStats {
    std::uint64_t queries{};
    std::uint64_t queryFailures{};
    std::uint64_t overlayMoves{};
    HRESULT lastQueryError{S_OK};
    HRESULT desktopStatus{E_UNEXPECTED};
    DWORD lastRegistrationError{};
};

// Construct, Start, HandleMessage, query, registration, Stop and destruction on the
// receiver's thread. Receiver is an existing hidden top-level HWND, NOT HWND_MESSAGE.
class NativeVisibility final {
public:
    explicit NativeVisibility(HWND receiver) noexcept;
    // Deterministic tests provide only the documented interface, never a live shell manager.
    NativeVisibility(HWND receiver, IVirtualDesktopManager& manager) noexcept;
#ifdef WINDOW_SWEATERS_VISIBILITY_TESTING
    using FactsQuery = HRESULT (*)(HWND, WindowSnapshot&) noexcept;
    NativeVisibility(HWND receiver, IVirtualDesktopManager& manager, FactsQuery facts) noexcept;
#endif
    ~NativeVisibility();
    NativeVisibility(const NativeVisibility&) = delete;
    NativeVisibility& operator=(const NativeVisibility&) = delete;
    NativeVisibility(NativeVisibility&&) = delete;
    NativeVisibility& operator=(NativeVisibility&&) = delete;

    StartResult Start() noexcept;
    PolicyChange Stop() noexcept;
    MessageResult HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    PolicyChange SetPaused(bool paused) noexcept;
    PolicySnapshot Snapshot() const noexcept { return policy_.Snapshot(); }
    NativeStats Stats() const noexcept { return stats_; }
    PolicyChange RefreshUncertainSession(Milliseconds now) noexcept;
    HRESULT RetryVirtualDesktop(Milliseconds now) noexcept;

    HRESULT RegisterOverlay(HWND overlay);
    void UnregisterOverlay(HWND overlay) noexcept;
    Observation Observe(const ProbeToken& token, HWND overlay) noexcept;
    // Caller first consumes the Hide transition. Only a hidden, registered, owned
    // overlay can move; success includes fresh target/overlay membership validation.
    HRESULT AssociateOverlay(HWND overlay, const ProbeToken& token, const DesktopId& desktop) noexcept;

    static SessionState QueryCurrentSession(DWORD sessionId) noexcept;

private:
    class DesktopOperation final {
    public:
        explicit DesktopOperation(NativeVisibility& owner) noexcept;
        ~DesktopOperation();
        DesktopOperation(const DesktopOperation&) = delete;
        DesktopOperation& operator=(const DesktopOperation&) = delete;
        bool Current() const noexcept;
        bool Authorized(const ProbeToken& token) const noexcept;
        IVirtualDesktopManager* Manager() const noexcept { return manager_.Get(); }

    private:
        NativeVisibility& owner_;
        std::uint64_t lifetime_{};
        Microsoft::WRL::ComPtr<IVirtualDesktopManager> manager_;
    };

    bool OnOwnerThread() const noexcept;
    bool ValidReceiver() const noexcept;
    bool ValidOverlay(HWND overlay) const noexcept;
    bool RegisteredOverlay(HWND overlay) const noexcept;
    std::optional<std::uint64_t> OverlayEpoch(HWND overlay) const noexcept;
    bool CurrentOverlay(HWND overlay, std::uint64_t epoch) const noexcept;
    HRESULT ValidateTarget(const TargetId& target) const noexcept;
    HRESULT QueryDesktop(const DesktopOperation& operation, const ProbeToken& token,
                         HWND window, DesktopId& desktop, bool& current) noexcept;
    HRESULT QueryFacts(HWND target, WindowSnapshot& facts) const noexcept;
    HRESULT ActivateDesktopManager() noexcept;
    void RecordFailure(HRESULT error) noexcept;
    void ReleaseRegistrations() noexcept;
    void FinishOperation() noexcept;
    void ReleaseDeferredCom() noexcept;

    HWND receiver_{};
    DWORD ownerThreadId_{};
    DWORD sessionId_{};
    VisibilityPolicy policy_;
    NativeStats stats_;
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> manager_;
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> suppliedDesktopManager_;
    std::unordered_map<HWND, std::uint64_t> overlays_;
    std::uint64_t nextOverlayEpoch_{1};
    HPOWERNOTIFY displayRegistration_{};
    HPOWERNOTIFY suspendRegistration_{};
    bool sessionRegistered_{};
    bool started_{};
    bool suppliedManager_{};
    bool comInitialized_{};
    std::size_t activeOperations_{};
    std::uint64_t lifetime_{1};
    bool releaseManagerPending_{};
    bool uninitializePending_{};
    bool releasingCom_{};
    std::optional<Milliseconds> lastActivation_;
    std::optional<Milliseconds> lastSessionQuery_;
#ifdef WINDOW_SWEATERS_VISIBILITY_TESTING
    FactsQuery factsQuery_{};
#endif
};

} // namespace window_sweaters::visibility
