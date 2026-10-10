#include "Session.h"
#include "..\Configuration\Pipe.h"
#include <microsoft.ui.xaml.window.h>
#include <sddl.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Automation;
using namespace window_sweaters;
namespace {
constexpr UINT activate_message=WM_APP+41;
LRESULT CALLBACK ActivationProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==activate_message) {
        auto target=reinterpret_cast<HWND>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (IsWindow(target)) { ShowWindow(target,SW_RESTORE); SetForegroundWindow(target); }
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
std::wstring ActivationClass(settings::Options const& options) {
    return L"WindowSweaters.Settings.Activation."+configuration::CurrentIdentity().logon_sid+
        (options.endpoint_suffix.empty()?L"":L"."+options.endpoint_suffix);
}
configuration::Handle OwnSettingsInstance(settings::Options const& options,bool& existing) {
    auto name=L"Local\\WindowSweaters.Settings."+configuration::CurrentIdentity().logon_sid+
        (options.endpoint_suffix.empty()?L"":L"."+options.endpoint_suffix);
    auto sddl=L"D:P(A;;GA;;;"+configuration::CurrentIdentity().logon_sid+L")";
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))
        throw std::runtime_error("Cannot create settings single-instance descriptor");
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
    configuration::Handle mutex(CreateMutexExW(&security,name.c_str(),CREATE_MUTEX_INITIAL_OWNER,
                                               SYNCHRONIZE|MUTEX_MODIFY_STATE));
    auto error=GetLastError(); LocalFree(descriptor);
    if (!mutex) throw std::runtime_error("Cannot own settings instance");
    existing=error==ERROR_ALREADY_EXISTS; return mutex;
}
bool ActivateExisting(settings::Options const& options) {
    auto name=ActivationClass(options);
    for (unsigned i=0;i<100;++i) {
        HWND window=FindWindowExW(HWND_MESSAGE,nullptr,name.c_str(),nullptr);
        if (window) {
            DWORD pid=0; GetWindowThreadProcessId(window,&pid);
            configuration::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
            wchar_t image[32768]{}; DWORD length=32768,session=0;
            if (!process || !QueryFullProcessImageNameW(process.get(),0,image,&length) ||
                !ProcessIdToSessionId(pid,&session) || session!=configuration::CurrentIdentity().session ||
                CompareStringOrdinal(image,-1,configuration::SiblingExecutable(L"WindowSweaters.Settings.exe").c_str(),-1,TRUE)!=CSTR_EQUAL)
                return false;
            AllowSetForegroundWindow(pid);
            return PostMessageW(window,activate_message,0,0)!=FALSE;
        }
        Sleep(50);
    }
    return false;
}
struct SettingsApp : ApplicationT<SettingsApp> {
    explicit SettingsApp(settings::Options options) : options_(std::move(options)) {
        Resources().MergedDictionaries().Append(XamlControlsResources{});
        UnhandledException([](auto const&,UnhandledExceptionEventArgs const& args) {
            OutputDebugStringW(L"WindowSweaters: unhandled settings UI exception.\n");
            args.Handled(false);
        });
    }
    void OnLaunched(LaunchActivatedEventArgs const&) {
        window_=Window{}; window_.Title(L"Window Sweaters — Settings");
        auto layout=StackPanel{}; layout.Spacing(16); layout.Margin(Thickness{24});
        auto heading=TextBlock{}; heading.Text(L"Window Sweaters"); heading.FontSize(28);
        layout.Children().Append(heading);
        auto description=TextBlock{}; description.Text(L"Native knitted borders. Closing settings keeps decoration running.");
        description.TextWrapping(TextWrapping::Wrap); layout.Children().Append(description);
        pause_=ToggleSwitch{}; pause_.Header(box_value(L"Pause decoration")); pause_.TabIndex(0);
        pause_.OnContent(box_value(L"Paused")); pause_.OffContent(box_value(L"Enabled"));
        AutomationProperties::SetName(pause_,L"Pause decoration");
        layout.Children().Append(pause_);
        width_label_=TextBlock{}; width_label_.Text(L"Border width: 16 DIP");
        layout.Children().Append(width_label_);
        width_=Slider{}; width_.Minimum(contracts::minimum_width_dip); width_.Maximum(contracts::maximum_width_dip);
        width_.StepFrequency(0.5); width_.Value(contracts::default_width_dip); width_.TabIndex(1);
        AutomationProperties::SetName(width_,L"Border width");
        AutomationProperties::SetHelpText(width_,L"Exterior border width, from 2 to 64 device-independent pixels.");
        layout.Children().Append(width_);
        status_=TextBlock{}; status_.Text(L"Connecting to host…"); status_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(status_,L"Connection and saved revision status");
        AutomationProperties::SetLiveSetting(status_,Automation::Peers::AutomationLiveSetting::Polite);
        layout.Children().Append(status_);
        reset_=Button{}; reset_.Content(box_value(L"Reset preserved configuration…")); reset_.TabIndex(2);
        reset_.Visibility(Visibility::Collapsed); AutomationProperties::SetName(reset_,L"Reset preserved configuration");
        layout.Children().Append(reset_);
        auto scroll=ScrollViewer{}; scroll.Content(layout); window_.Content(scroll);
        auto weak=get_weak();
        pause_.Toggled([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        width_.ValueChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        reset_.Click([weak](auto const&,auto const&) { if (auto app=weak.get()) app->ConfirmReset(); });
        window_.Closed([weak](auto const&,auto const&) {
            if (auto app=weak.get()) {
                app->closed_=true;
                app->session_->Stop();
                if (app->activation_) { DestroyWindow(app->activation_); app->activation_=nullptr; }
            }
        });
        HWND native=nullptr; window_.as<IWindowNative>()->get_WindowHandle(&native);
        SetWindowPos(native,nullptr,0,0,580,470,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        auto class_name=ActivationClass(options_);
        WNDCLASSW registration{}; registration.lpfnWndProc=ActivationProcedure;
        registration.hInstance=GetModuleHandleW(nullptr); registration.lpszClassName=class_name.c_str();
        if (!RegisterClassW(&registration)) throw std::runtime_error("Cannot register settings activation window");
        activation_=CreateWindowExW(0,class_name.c_str(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,registration.hInstance,nullptr);
        if (!activation_) throw std::runtime_error("Cannot create settings activation window");
        SetWindowLongPtrW(activation_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(native));
        auto dispatcher=window_.DispatcherQueue();
        session_=std::make_unique<settings::Session>(options_,[weak,dispatcher](settings::ConnectionState state) {
            dispatcher.TryEnqueue([weak,state=std::move(state)] { if (auto app=weak.get()) app->Render(state); });
        });
        pause_.IsEnabled(false); width_.IsEnabled(false); window_.Activate();
    }
    void Change() {
        if (updating_ || !session_) return;
        width_label_.Text(L"Border width: "+to_hstring(width_.Value())+L" DIP");
        status_.Text(L"Saving change…");
        session_->Change(displayed_,pause_.IsOn(),static_cast<float>(width_.Value()));
    }
    fire_and_forget ConfirmReset() {
        auto lifetime=get_strong();
        ContentDialog dialog;
        dialog.XamlRoot(window_.Content().XamlRoot()); dialog.Title(box_value(L"Reset configuration?"));
        dialog.Content(box_value(L"The original file will be preserved. Width and pause will reset to defaults."));
        dialog.PrimaryButtonText(L"Preserve and reset"); dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Close);
        auto displayed=displayed_;
        if (co_await dialog.ShowAsync()==ContentDialogResult::Primary) session_->Reset(displayed);
    }
    void Render(settings::ConnectionState const& state) {
        if (closed_) return;
        updating_=true;
        if (!state.pending) {
            displayed_=state;
            pause_.IsOn(state.configuration.paused); width_.Value(state.configuration.border_width_dip);
            width_label_.Text(L"Border width: "+to_hstring(state.configuration.border_width_dip)+L" DIP");
        }
        bool recovery=state.status==contracts::Status::recovery_required;
        pause_.IsEnabled(state.connected&&!recovery); width_.IsEnabled(state.connected&&!recovery);
        reset_.Visibility(recovery?Visibility::Visible:Visibility::Collapsed); reset_.IsEnabled(state.connected);
        std::wstring text=state.detail;
        if (state.connected) {
            switch (state.status) {
            case contracts::Status::saved:
                text=state.pending?L"Saving…":L"Connected · Saved revision "+std::to_wstring(state.configuration.revision); break;
            case contracts::Status::conflict: text=L"Another change won. Refreshed from the host; your change was not saved."; break;
            case contracts::Status::storage_error: text=L"Save failed. The last committed settings remain active."; break;
            case contracts::Status::recovery_required: text=L"Configuration is corrupt or newer. Original data is preserved; reset is explicit."; break;
            case contracts::Status::wrong_instance: text=L"Host restarted. Old requests were discarded."; break;
            case contracts::Status::client_expired: text=L"Connection expired. Pending changes were discarded; reconnecting."; break;
            default: text=L"Change was rejected; saved revision "+std::to_wstring(state.configuration.revision)+L" remains active."; break;
            }
        }
        status_.Text(text); updating_=false;
    }
    settings::Options options_;
    Window window_{nullptr};
    ToggleSwitch pause_{nullptr};
    Slider width_{nullptr};
    TextBlock width_label_{nullptr},status_{nullptr};
    Button reset_{nullptr};
    HWND activation_=nullptr;
    bool updating_=false;
    bool closed_=false;
    std::unique_ptr<settings::Session> session_;
    settings::ConnectionState displayed_;
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        auto options=settings::ParseOptions();
        bool existing=false; auto singleton=OwnSettingsInstance(options,existing);
        if (existing) return ActivateExisting(options)?0:2;
        init_apartment(apartment_type::single_threaded);
        Application::Start([options=std::move(options)](auto&&) { make<SettingsApp>(options); });
        return 0;
    } catch (hresult_error const& error) {
        MessageBoxW(nullptr,error.message().c_str(),L"Window Sweaters settings failed",MB_ICONERROR); return 1;
    } catch (std::exception const&) {
        MessageBoxW(nullptr,L"Settings could not start. Check the native dependencies and companion installation.",
                    L"Window Sweaters",MB_ICONERROR); return 1;
    }
}
