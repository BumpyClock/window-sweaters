#include "Session.h"
#include "Preview.h"
#include "..\Configuration\Pipe.h"
#include "..\Configuration\Storage.h"
#include <microsoft.ui.xaml.window.h>
#include <sddl.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <cstring>
#include <limits>
#include <optional>
#include <cwchar>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Automation;
using namespace window_sweaters;
namespace {
std::filesystem::path startup_log;
void StartupDiagnostic(wchar_t const* stage,HRESULT code,std::wstring_view message) noexcept {
    try {
        wchar_t header[160]{};
        swprintf_s(header,L"WindowSweaters.Settings pid=%lu stage=%s HRESULT=0x%08X ",
                   GetCurrentProcessId(),stage,static_cast<unsigned>(code));
        std::wstring line(header); line.append(message.substr(0,4096)); line+=L"\r\n";
        OutputDebugStringW(line.c_str());
        auto utf8=to_string(line);
        auto stderr_handle=GetStdHandle(STD_ERROR_HANDLE);
        DWORD written=0;
        if (stderr_handle && stderr_handle!=INVALID_HANDLE_VALUE)
            WriteFile(stderr_handle,utf8.data(),static_cast<DWORD>(utf8.size()),&written,nullptr);
        if (!startup_log.empty()) {
            configuration::Handle file(CreateFileW(startup_log.c_str(),FILE_APPEND_DATA,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
            if (file) WriteFile(file.get(),utf8.data(),static_cast<DWORD>(utf8.size()),&written,nullptr);
        }
    } catch (...) { OutputDebugStringW(L"WindowSweaters.Settings: startup diagnostic could not be recorded.\n"); }
}
void InitializeStartupDiagnostic(settings::Options const& options) noexcept {
    try {
        auto directory=options.test_mode?options.storage_directory:configuration::DefaultStorageDirectory();
        std::error_code error;
        std::filesystem::create_directories(directory,error);
        if (error) return;
        startup_log=directory/L"settings-startup.log";
        configuration::Handle file(CreateFileW(startup_log.c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        if (!file) startup_log.clear();
    } catch (...) { startup_log.clear(); }
}
struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferAccess : IUnknown {
    virtual HRESULT __stdcall Buffer(BYTE** value)=0;
};
Windows::UI::Color ToColor(std::uint32_t argb) {
    return {static_cast<std::uint8_t>(argb>>24),static_cast<std::uint8_t>(argb>>16),
        static_cast<std::uint8_t>(argb>>8),static_cast<std::uint8_t>(argb)};
}
std::uint32_t ToArgb(Windows::UI::Color const& color) {
    return (std::uint32_t(color.A)<<24)|(std::uint32_t(color.R)<<16)|(std::uint32_t(color.G)<<8)|color.B;
}
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
struct SettingsApp : ApplicationT<SettingsApp,Markup::IXamlMetadataProvider> {
    explicit SettingsApp(settings::Options options) : options_(std::move(options)) {
        UnhandledException([](auto const&,UnhandledExceptionEventArgs const& args) {
            StartupDiagnostic(L"unhandled-xaml",args.Exception(),args.Message());
            args.Handled(false);
        });
        StartupDiagnostic(L"application",S_OK,L"Native application and XAML metadata provider created.");
    }
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& type) {
        return MetadataProvider().GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(hstring const& name) {
        return MetadataProvider().GetXamlType(name);
    }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return MetadataProvider().GetXmlnsDefinitions();
    }
    XamlTypeInfo::XamlControlsXamlMetaDataProvider MetadataProvider() {
        if (!metadata_) metadata_=XamlTypeInfo::XamlControlsXamlMetaDataProvider{};
        return metadata_;
    }
    void OnLaunched(LaunchActivatedEventArgs const&) {
        try { Launch(); }
        catch (hresult_error const& error) {
            StartupDiagnostic(L"launch-failed",error.code(),error.message()); throw;
        } catch (std::exception const& error) {
            StartupDiagnostic(L"launch-failed",E_FAIL,to_hstring(error.what())); throw;
        }
    }
    void Launch() {
        StartupDiagnostic(L"launch-entered",S_OK,L"Application constructor completed; entering OnLaunched.");
        window_=Window{}; window_.Title(L"Window Sweaters — Settings");
        StartupDiagnostic(L"first-window-created",S_OK,L"Native WinUI window created before control resources.");
        if (options_.startup_probe) {
            window_.Content(Grid{}); window_.Activate();
            StartupDiagnostic(L"first-window-activated",S_OK,L"Minimal native window activated before loading control resources.");
        }
        MetadataProvider();
        StartupDiagnostic(L"metadata-ready",S_OK,L"WinUI control metadata provider instantiated.");
        if (options_.startup_probe) {
            auto provider=get_strong().as<Markup::IXamlMetadataProvider>();
            for (auto name:{L"Microsoft.UI.Xaml.Controls.ColorPicker",L"Microsoft.UI.Xaml.Controls.ComboBox",
                           L"Microsoft.UI.Xaml.Controls.Slider",L"Microsoft.UI.Xaml.Controls.ToggleSwitch"}) {
                if (!provider.GetXamlType(name)) throw hresult_error(E_FAIL,L"Required WinUI control metadata is missing.");
            }
            StartupDiagnostic(L"metadata-probe",S_OK,L"ColorPicker, ComboBox, Slider and ToggleSwitch metadata resolved.");
        }
        // Code-only App has no generated InitializeComponent host. Its resource
        // dictionaries are usable only after composition and OnLaunched begin.
        StartupDiagnostic(L"resource-construction",S_OK,L"Constructing native XamlControlsResources.");
        auto controls_resources=XamlControlsResources{};
        StartupDiagnostic(L"resource-merge",S_OK,L"Merging native XamlControlsResources into Application resources.");
        Resources().MergedDictionaries().Append(controls_resources);
        StartupDiagnostic(L"resources",S_OK,L"WinUI control resources loaded.");
        auto layout=StackPanel{}; layout.Spacing(16); layout.Margin(Thickness{24});
        auto heading=TextBlock{}; heading.Text(L"Window Sweaters"); heading.FontSize(28);
        layout.Children().Append(heading);
        auto description=TextBlock{}; description.Text(L"Native knitted borders. Closing settings keeps decoration running.");
        description.TextWrapping(TextWrapping::Wrap); layout.Children().Append(description);
        status_=TextBlock{}; status_.Text(L"Connecting to host…"); status_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(status_,L"Connection and saved revision status");
        AutomationProperties::SetLiveSetting(status_,Automation::Peers::AutomationLiveSetting::Polite);
        layout.Children().Append(status_);
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
        preview_status_=TextBlock{}; preview_status_.Text(L"Production knit preview");
        layout.Children().Append(preview_status_);
        preview_image_=Image{}; preview_image_.MaxWidth(528); preview_image_.Height(268);
        preview_image_.Stretch(Media::Stretch::Uniform);
        AutomationProperties::SetName(preview_image_,L"Production knitted border preview");
        layout.Children().Append(preview_image_);
        design_=ComboBox{}; design_.Header(box_value(L"Original design")); design_.HorizontalAlignment(HorizontalAlignment::Stretch);
        AutomationProperties::SetName(design_,L"Original design");
        auto add_design=[&](wchar_t const* name,char const* id) {
            ComboBoxItem item; item.Content(box_value(name)); item.Tag(box_value(to_hstring(id)));
            design_.Items().Append(item);
        };
        add_design(L"Plain knit","");
        add_design(L"By app (preview: Finder example)","@by-app");
        for (std::size_t i=0;i<designs::ws_design_theme_count();++i) {
            auto theme=designs::ws_design_theme_at(i);
            auto name=to_hstring(theme->name); add_design(name.c_str(),theme->id);
        }
        design_.SelectedIndex(0); layout.Children().Append(design_);
        auto mode_help=TextBlock{};
        mode_help.Text(L"Original charts keep their authored stitches, material, corners and cuffs. Raised material uses stockinette; Pattern knit restores the selected plain stitch. By app shows a Finder example; app resolution is supplied by the host.");
        mode_help.TextWrapping(TextWrapping::Wrap); layout.Children().Append(mode_help);
        stitch_=ComboBox{}; stitch_.Header(box_value(L"Plain stitch")); stitch_.HorizontalAlignment(HorizontalAlignment::Stretch);
        for (auto name:{L"Stockinette",L"Rib",L"Garter"}) stitch_.Items().Append(box_value(name));
        stitch_.SelectedIndex(0); AutomationProperties::SetName(stitch_,L"Plain stitch"); layout.Children().Append(stitch_);
        material_=ComboBox{}; material_.Header(box_value(L"Plain material")); material_.HorizontalAlignment(HorizontalAlignment::Stretch);
        material_.Items().Append(box_value(L"Pattern knit")); material_.Items().Append(box_value(L"Raised stockinette"));
        material_.SelectedIndex(static_cast<int>(contracts::Configuration{}.material_style));
        AutomationProperties::SetName(material_,L"Plain material"); layout.Children().Append(material_);
        rows_label_=TextBlock{}; rows_label_.Text(L"Stitch size: 6 rows across the border");
        layout.Children().Append(rows_label_);
        rows_=Slider{}; rows_.Minimum(contracts::minimum_stitch_rows); rows_.Maximum(contracts::maximum_stitch_rows);
        rows_.StepFrequency(0.5); rows_.Value(6);
        AutomationProperties::SetName(rows_,L"Stitch rows across border");
        AutomationProperties::SetHelpText(rows_,L"1.5 to 12 rows across the band. More rows make smaller stitches.");
        layout.Children().Append(rows_);
        auto color_label=TextBlock{}; color_label.Text(L"Base yarn color (ARGB; knit output is opaque)");
        layout.Children().Append(color_label);
        color_=ColorPicker{}; color_.Color(ToColor(contracts::Configuration{}.yarn_argb)); color_.IsAlphaEnabled(false);
        color_.IsMoreButtonVisible(false); color_.MaxWidth(480);
        AutomationProperties::SetName(color_,L"Base yarn color"); layout.Children().Append(color_);
        reset_=Button{}; reset_.Content(box_value(L"Reset preserved configuration…")); reset_.TabIndex(2);
        reset_.Visibility(Visibility::Collapsed); AutomationProperties::SetName(reset_,L"Reset preserved configuration");
        layout.Children().Append(reset_);
        auto scroll=ScrollViewer{}; scroll.Content(layout); window_.Content(scroll);
        StartupDiagnostic(L"controls",S_OK,L"All native appearance controls and preview view constructed.");
        auto weak=get_weak();
        pause_.Toggled([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        width_.ValueChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        design_.SelectionChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->DesignChanged(); });
        stitch_.SelectionChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        material_.SelectionChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        rows_.ValueChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        color_.ColorChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        reset_.Click([weak](auto const&,auto const&) { if (auto app=weak.get()) app->ConfirmReset(); });
        window_.Closed([weak](auto const&,auto const&) {
            if (auto app=weak.get()) {
                app->closed_=true;
                app->session_->Stop();
                app->preview_->Stop();
                if (app->activation_) { DestroyWindow(app->activation_); app->activation_=nullptr; }
            }
        });
        HWND native=nullptr; window_.as<IWindowNative>()->get_WindowHandle(&native);
        SetWindowPos(native,nullptr,0,0,660,920,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        auto class_name=ActivationClass(options_);
        WNDCLASSW registration{}; registration.lpfnWndProc=ActivationProcedure;
        registration.hInstance=GetModuleHandleW(nullptr); registration.lpszClassName=class_name.c_str();
        if (!RegisterClassW(&registration)) throw std::runtime_error("Cannot register settings activation window");
        activation_=CreateWindowExW(0,class_name.c_str(),L"",0,0,0,0,0,HWND_MESSAGE,nullptr,registration.hInstance,nullptr);
        if (!activation_) throw std::runtime_error("Cannot create settings activation window");
        SetWindowLongPtrW(activation_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(native));
        auto dispatcher=make_weak(window_.DispatcherQueue());
        session_=std::make_unique<settings::Session>(options_,[weak,dispatcher](settings::ConnectionState state) {
            if (auto queue=dispatcher.get())
                queue.TryEnqueue([weak,state=std::move(state)] { if (auto app=weak.get()) app->Render(state); });
        });
        preview_=std::make_unique<settings::Preview>([weak,dispatcher](std::shared_ptr<settings::PreviewResult const> result) {
            if (auto queue=dispatcher.get()) queue.TryEnqueue([weak,result=std::move(result)] {
                if (auto app=weak.get()) app->RenderPreview(*result);
            });
        });
        EnableControls(false); window_.Activate();
        StartupDiagnostic(L"window-activated",S_OK,L"Native settings window activated; awaiting host snapshot.");
    }
    void EnableControls(bool enabled) {
        pause_.IsEnabled(enabled); width_.IsEnabled(enabled); design_.IsEnabled(enabled);
        rows_.IsEnabled(enabled); color_.IsEnabled(enabled);
        bool plain=design_.SelectedIndex()==0;
        material_.IsEnabled(enabled&&plain);
        stitch_.IsEnabled(enabled&&plain&&material_.SelectedIndex()==0);
    }
    contracts::Configuration Inputs() const {
        auto configuration=draft_.Displayed().configuration;
        configuration.paused=pause_.IsOn(); configuration.border_width_dip=static_cast<float>(width_.Value());
        auto tag=to_string(unbox_value<hstring>(design_.SelectedItem().as<ComboBoxItem>().Tag()));
        configuration.design_selection=tag.empty()?contracts::DesignSelection::plain:
            tag=="@by-app"?contracts::DesignSelection::by_app:contracts::DesignSelection::original;
        configuration.design_id=configuration.design_selection==contracts::DesignSelection::original?tag:"";
        configuration.stitch_style=static_cast<contracts::StitchStyle>(stitch_.SelectedIndex());
        configuration.material_style=static_cast<contracts::MaterialStyle>(material_.SelectedIndex());
        configuration.stitch_rows=static_cast<float>(rows_.Value()); configuration.yarn_argb=ToArgb(color_.Color());
        return configuration;
    }
    void DesignChanged() {
        if (updating_ || !session_) return;
        auto tag=to_string(unbox_value<hstring>(design_.SelectedItem().as<ComboBoxItem>().Tag()));
        if (auto theme=designs::ws_design_theme_by_id(tag.c_str())) {
            updating_=true; color_.Color(ToColor(theme->base_argb)); updating_=false;
        }
        Change();
    }
    void Change() {
        if (updating_ || !session_) return;
        width_label_.Text(L"Border width: "+to_hstring(width_.Value())+L" DIP");
        rows_label_.Text(L"Stitch size: "+to_hstring(rows_.Value())+L" rows across the border");
        EnableControls(draft_.Current().connected&&draft_.Current().status!=contracts::Status::recovery_required);
        auto configuration=Inputs();
        auto generation=session_->Change(draft_.Displayed(),configuration);
        if (!generation) return;
        draft_.Begin(generation,configuration);
        status_.Text(L"Saving change…");
        preview_configuration_.reset();
        preview_generation_=preview_->Submit(configuration);
        preview_status_.Text(L"Unsaved preview · awaiting host validation and save");
    }
    fire_and_forget ConfirmReset() {
        auto lifetime=get_strong();
        ContentDialog dialog;
        dialog.XamlRoot(window_.Content().XamlRoot()); dialog.Title(box_value(L"Reset configuration?"));
        dialog.Content(box_value(L"The original file will be preserved. Appearance, width and pause will reset to defaults."));
        dialog.PrimaryButtonText(L"Preserve and reset"); dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Close);
        auto displayed=draft_.Displayed();
        if (co_await dialog.ShowAsync()==ContentDialogResult::Primary && !closed_) {
            auto generation=session_->Reset(displayed);
            draft_.Begin(generation,draft_.Controls());
        }
    }
    void Render(settings::ConnectionState const& state) {
        if (closed_) return;
        auto update=draft_.Apply(state);
        if (!update.accepted) return;
        updating_=true;
        if (update.controls_changed) {
            auto const& configuration=draft_.Controls();
            pause_.IsOn(configuration.paused); width_.Value(configuration.border_width_dip);
            width_label_.Text(L"Border width: "+to_hstring(configuration.border_width_dip)+L" DIP");
            int selected=configuration.design_selection==contracts::DesignSelection::by_app?1:0;
            if (configuration.design_selection==contracts::DesignSelection::original) {
                for (std::uint32_t i=2;i<design_.Items().Size();++i) {
                    auto item=design_.Items().GetAt(i).as<ComboBoxItem>();
                    if (to_string(unbox_value<hstring>(item.Tag()))==configuration.design_id) {
                        selected=static_cast<int>(i); break;
                    }
                }
            }
            design_.SelectedIndex(selected);
            stitch_.SelectedIndex(static_cast<int>(configuration.stitch_style));
            material_.SelectedIndex(static_cast<int>(configuration.material_style));
            rows_.Value(configuration.stitch_rows); color_.Color(ToColor(configuration.yarn_argb));
            rows_label_.Text(L"Stitch size: "+to_hstring(configuration.stitch_rows)+L" rows across the border");
            if (!preview_configuration_ || *preview_configuration_!=configuration) {
                preview_configuration_=configuration;
                preview_generation_=preview_->Submit(configuration);
            }
        }
        auto const& configuration=draft_.Controls();
        hstring preview_label;
        if (draft_.Pending()) {
            preview_label=state.connected?L"Unsaved preview · awaiting host validation and save":
                L"Unsaved preview · host disconnected";
        } else if (!state.connected) {
            preview_label=draft_.LastConfirmed()
                ? L"Last confirmed preview · revision "+to_hstring(configuration.revision)+L" · host disconnected"
                : L"Default preview · host disconnected";
        } else if (state.status==contracts::Status::recovery_required || !draft_.LastConfirmed()) {
            preview_label=L"Default preview · host configuration unavailable";
        } else {
            preview_label=L"Committed preview · revision "+to_hstring(configuration.revision);
        }
        if (configuration.design_selection==contracts::DesignSelection::by_app) preview_label=preview_label+L" · Finder example";
        preview_status_.Text(preview_label);
        bool recovery=state.status==contracts::Status::recovery_required;
        EnableControls(state.connected&&!recovery);
        reset_.Visibility(recovery?Visibility::Visible:Visibility::Collapsed); reset_.IsEnabled(state.connected);
        std::wstring text=state.detail;
        if (state.connected) {
            switch (state.status) {
            case contracts::Status::saved:
                text=(state.pending||draft_.Pending())?L"Saving…":L"Connected · Saved revision "+std::to_wstring(state.configuration.revision); break;
            case contracts::Status::conflict: text=L"Another change won. Refreshed from the host; your change was not saved."; break;
            case contracts::Status::storage_error:
                text=L"Save failed (Win32 error "+std::to_wstring(state.error)+L"). The last committed settings remain active."; break;
            case contracts::Status::recovery_required: text=L"Configuration is corrupt or newer. Original data is preserved; reset is explicit."; break;
            case contracts::Status::wrong_instance: text=L"Host restarted. Old requests were discarded."; break;
            case contracts::Status::client_expired: text=L"Connection expired. Pending changes were discarded; reconnecting."; break;
            default: text=L"Change was rejected; saved revision "+std::to_wstring(state.configuration.revision)+L" remains active."; break;
            }
        }
        status_.Text(text); updating_=false;
    }
    void RenderPreview(settings::PreviewResult const& result) {
        if (closed_ || result.generation!=preview_generation_) return;
        if (!result.error.empty()) {
            preview_image_.Source(nullptr); preview_status_.Text(L"Preview failed: "+result.error); return;
        }
        try {
            auto const& image=result.image;
            if (!image.width || !image.height || image.width>528 || image.height>268 ||
                image.stride!=image.width*4 || image.pixels.size()!=std::size_t(image.width)*image.height)
                throw std::runtime_error("Unexpected production preview dimensions");
            Media::Imaging::WriteableBitmap bitmap(static_cast<int>(image.width),static_cast<int>(image.height));
            BYTE* bytes=nullptr; check_hresult(bitmap.PixelBuffer().as<BufferAccess>()->Buffer(&bytes));
            std::memcpy(bytes,image.pixels.data(),image.ByteSize()); bitmap.Invalidate();
            preview_image_.Source(bitmap);
        } catch (hresult_error const& error) {
            preview_status_.Text(L"Preview bitmap failed: "+error.message());
        } catch (std::exception const&) { preview_status_.Text(L"Preview bitmap dimensions were rejected."); }
    }
    settings::Options options_;
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata_{nullptr};
    Window window_{nullptr};
    ToggleSwitch pause_{nullptr};
    Slider width_{nullptr},rows_{nullptr};
    ComboBox design_{nullptr},stitch_{nullptr},material_{nullptr};
    ColorPicker color_{nullptr};
    Image preview_image_{nullptr};
    TextBlock preview_status_{nullptr},rows_label_{nullptr};
    TextBlock width_label_{nullptr},status_{nullptr};
    Button reset_{nullptr};
    HWND activation_=nullptr;
    bool updating_=false;
    bool closed_=false;
    std::unique_ptr<settings::Session> session_;
    std::unique_ptr<settings::Preview> preview_;
    std::uint64_t preview_generation_=0;
    std::optional<contracts::Configuration> preview_configuration_;
    settings::DraftState draft_;
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        auto options=settings::ParseOptions();
        bool existing=false; auto singleton=OwnSettingsInstance(options,existing);
        if (existing) return ActivateExisting(options)?0:2;
        InitializeStartupDiagnostic(options);
        StartupDiagnostic(L"start",S_OK,L"Starting native settings.");
        init_apartment(apartment_type::single_threaded);
        Application::Start([options=std::move(options)](auto&&) { make<SettingsApp>(options); });
        return 0;
    } catch (hresult_error const& error) {
        StartupDiagnostic(L"startup-failed",error.code(),error.message());
        MessageBoxW(nullptr,error.message().c_str(),L"Window Sweaters settings failed",MB_ICONERROR); return 1;
    } catch (std::exception const& error) {
        StartupDiagnostic(L"startup-failed",E_FAIL,to_hstring(error.what()));
        MessageBoxW(nullptr,L"Settings could not start. Check the native dependencies and companion installation.",
                    L"Window Sweaters",MB_ICONERROR); return 1;
    }
}
