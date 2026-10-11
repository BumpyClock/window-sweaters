#include "Session.h"
#include "Preview.h"
#include "AppsPresentation.h"
#include "..\Configuration\Pipe.h"
#include "..\Configuration\Storage.h"
#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>
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
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
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
        BuildLayout();
        StartupDiagnostic(L"controls",S_OK,L"All native appearance controls and preview view constructed.");
        auto weak=get_weak();
        scroll_.Loaded([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->Dispatch(L"apps-loaded",[&]{app->QueuePendingApps();});
        });
        scroll_.LayoutUpdated([weak](auto const&,auto const&) {
            if (auto app=weak.get(); app && app->pending_apps_state_)
                app->Dispatch(L"apps-layout-ready",[&]{app->QueuePendingApps();});
        });
        scroll_.Unloaded([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->presentation_ready_=false;
        });
        apps_all_on_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->Dispatch(L"apps-all-on",[&]{app->ChangeApps({true,true,{}});});
        });
        apps_all_off_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->Dispatch(L"apps-all-off",[&]{app->ChangeApps({false,true,{}});});
        });
        apps_default_.Toggled([weak](auto const&,auto const&) {
            if (auto app=weak.get(); app && !app->updating_apps_)
                app->Dispatch(L"apps-default-change",[&]{app->ChangeApps({app->apps_default_.IsOn(),false,{}});});
        });
        pause_.Toggled([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        width_.ValueChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        design_.SelectionChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->DesignChanged(); });
        stitch_.SelectionChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        material_.SelectionChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        rows_.ValueChanged([weak](auto const&,auto const&) { if (auto app=weak.get()) app->Change(); });
        color_.ColorChanged([weak](auto const&,auto const&) {
            if (auto app=weak.get()) { app->UpdateYarnSummary(); app->Change(); }
        });
        root_.SizeChanged([weak](auto const&,SizeChangedEventArgs const& args) {
            if (auto app=weak.get()) app->Dispatch(L"layout-class",[&]{app->ApplyLayoutClass(args.NewSize().Width);});
        });
        reset_.Click([weak](auto const&,auto const&) { if (auto app=weak.get()) app->ConfirmReset(); });
        window_.Closed([weak](auto const&,auto const&) {
            if (auto app=weak.get()) {
                app->closed_=true;
                app->session_->RequestStop();
                if (app->apps_session_) app->apps_session_->RequestStop();
                app->session_->Stop();
                if (app->apps_session_) app->apps_session_->Stop();
                app->preview_->Stop();
                if (app->activation_) { DestroyWindow(app->activation_); app->activation_=nullptr; }
            }
        });
        HWND native=nullptr; window_.as<IWindowNative>()->get_WindowHandle(&native);
        PlaceWindow(native);
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
                queue.TryEnqueue([weak,state=std::move(state)] {
                    if (auto app=weak.get()) app->Dispatch(L"appearance-dispatch",[&]{app->Render(state);});
                });
        });
        apps_session_=std::make_unique<settings::AppsSession>(options_,[weak,dispatcher](settings::AppsConnectionState state) {
            if (auto queue=dispatcher.get())
                queue.TryEnqueue([weak,state=std::move(state)] {
                    if (auto app=weak.get()) app->Dispatch(L"apps-dispatch",[&]{app->RenderApps(state);});
                });
        });
        preview_=std::make_unique<settings::Preview>([weak,dispatcher](std::shared_ptr<settings::PreviewResult const> result) {
            if (auto queue=dispatcher.get()) queue.TryEnqueue([weak,result=std::move(result)] {
                if (auto app=weak.get()) app->Dispatch(L"preview-dispatch",[&]{app->RenderPreview(*result);});
            });
        },settings::Preview::Render{},options_.storage_directory);
        EnableControls(false); EnableApps(false); EnableImports(false); window_.Activate();
        StartupDiagnostic(L"window-activated",S_OK,L"Native settings window activated; awaiting host snapshot.");
    }
    void EnableControls(bool enabled) {
        pause_.IsEnabled(enabled); width_.IsEnabled(enabled); design_.IsEnabled(enabled);
        rows_.IsEnabled(enabled); color_.IsEnabled(enabled);
        bool plain=design_.SelectedIndex()==0;
        plain_group_.Visibility(plain?Visibility::Visible:Visibility::Collapsed);
        material_.IsEnabled(enabled&&plain);
        stitch_.IsEnabled(enabled&&plain&&material_.SelectedIndex()==0);
    }
    void LoadStyles() {
        try {
            styles_=Markup::XamlReader::Load(LR"(<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
<Style x:Key="WsPage" TargetType="Grid"><Setter Property="Background" Value="{ThemeResource ApplicationPageBackgroundThemeBrush}"/></Style>
<Style x:Key="WsCard" TargetType="Border">
<Setter Property="Background" Value="{ThemeResource CardBackgroundFillColorDefaultBrush}"/>
<Setter Property="BorderBrush" Value="{ThemeResource CardStrokeColorDefaultBrush}"/>
<Setter Property="BorderThickness" Value="1"/><Setter Property="CornerRadius" Value="8"/><Setter Property="Padding" Value="16"/></Style>
<Style x:Key="WsSwatch" TargetType="Border">
<Setter Property="BorderBrush" Value="{ThemeResource ControlStrongStrokeColorDefaultBrush}"/>
<Setter Property="BorderThickness" Value="1"/><Setter Property="CornerRadius" Value="4"/>
<Setter Property="Width" Value="20"/><Setter Property="Height" Value="20"/></Style>
<Style x:Key="WsSecondary" TargetType="TextBlock">
<Setter Property="Foreground" Value="{ThemeResource TextFillColorSecondaryBrush}"/>
<Setter Property="FontSize" Value="12"/><Setter Property="TextWrapping" Value="Wrap"/></Style>
</ResourceDictionary>)").as<ResourceDictionary>();
        } catch (hresult_error const& error) {
            StartupDiagnostic(L"style-load-failed",error.code(),error.message()); throw;
        }
        for (auto key:{L"WsPage",L"WsCard",L"WsSwatch",L"WsSecondary"}) {
            if (!styles_.HasKey(box_value(key))) {
                StartupDiagnostic(L"style-load-failed",E_FAIL,key);
                throw hresult_error(E_FAIL,L"Required native theme style is missing.");
            }
        }
    }
    template<class Target> void ApplyStyle(Target const& target,wchar_t const* key) const {
        target.Style(styles_.Lookup(box_value(key)).template as<Style>());
    }
    void StyleSecondary(TextBlock const& text) const { ApplyStyle(text,L"WsSecondary"); }
    Style Builtin(wchar_t const* key) { return Resources().Lookup(box_value(key)).as<Style>(); }
    TextBlock Heading(wchar_t const* text,wchar_t const* style,Automation::Peers::AutomationHeadingLevel level) {
        TextBlock heading; heading.Text(text); heading.Style(Builtin(style)); heading.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetHeadingLevel(heading,level); return heading;
    }
    TextBlock Section(wchar_t const* text) {
        return Heading(text,L"SubtitleTextBlockStyle",Automation::Peers::AutomationHeadingLevel::Level2);
    }
    TextBlock Group(wchar_t const* text) {
        return Heading(text,L"BodyStrongTextBlockStyle",Automation::Peers::AutomationHeadingLevel::Level3);
    }
    TextBlock Caption(wchar_t const* text) {
        TextBlock caption; caption.Text(text); StyleSecondary(caption); return caption;
    }
    Border Card(UIElement const& child) {
        Border card; ApplyStyle(card,L"WsCard"); card.Child(child); return card;
    }
    Grid ButtonGrid(std::initializer_list<Button> buttons) {
        Grid grid; grid.ColumnSpacing(8); grid.RowSpacing(8);
        for (int column=0;column<2;++column) grid.ColumnDefinitions().Append(ColumnDefinition{});
        int index=0;
        for (auto const& button:buttons) {
            if (index%2==0) {
                RowDefinition row; row.Height(GridLength{1,GridUnitType::Auto}); grid.RowDefinitions().Append(row);
            }
            button.HorizontalAlignment(HorizontalAlignment::Stretch); button.MinHeight(44);
            Grid::SetRow(button,index/2); Grid::SetColumn(button,index%2);
            grid.Children().Append(button); ++index;
        }
        return grid;
    }
    static StackPanel Stack(double spacing) { StackPanel panel; panel.Spacing(spacing); return panel; }
    void BuildLayout() {
        LoadStyles();
        status_=TextBlock{}; status_.Text(L"Connecting to host…"); status_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(status_,L"Connection and saved revision status");
        AutomationProperties::SetLiveSetting(status_,Automation::Peers::AutomationLiveSetting::Polite);
        reset_=Button{}; reset_.Content(box_value(L"Reset preserved configuration…")); reset_.MinHeight(44);
        reset_.Visibility(Visibility::Collapsed); AutomationProperties::SetName(reset_,L"Reset preserved configuration");
        auto status_panel=Stack(8); status_panel.Children().Append(status_); status_panel.Children().Append(reset_);

        preview_image_=Image{}; preview_image_.MaxWidth(528); preview_image_.MaxHeight(268); preview_image_.MinHeight(160);
        preview_image_.Stretch(Media::Stretch::Uniform); preview_image_.HorizontalAlignment(HorizontalAlignment::Center);
        AutomationProperties::SetName(preview_image_,L"Production knitted border preview");
        preview_status_=TextBlock{}; preview_status_.Text(L"Production knit preview"); preview_status_.TextWrapping(TextWrapping::Wrap);
        StyleSecondary(preview_status_);
        pause_=ToggleSwitch{}; pause_.Header(box_value(L"Pause decoration"));
        pause_.OnContent(box_value(L"Paused")); pause_.OffContent(box_value(L"Enabled"));
        AutomationProperties::SetName(pause_,L"Pause decoration");
        width_label_=TextBlock{}; width_label_.Text(L"Border width: 16 DIP");
        width_=Slider{}; width_.Minimum(contracts::minimum_width_dip); width_.Maximum(contracts::maximum_width_dip);
        width_.StepFrequency(0.5); width_.Value(contracts::default_width_dip);
        AutomationProperties::SetName(width_,L"Border width");
        AutomationProperties::SetHelpText(width_,L"Exterior border width, from 2 to 64 device-independent pixels.");
        auto preview_panel=Stack(12);
        for (auto const& child:std::initializer_list<UIElement>{Section(L"Preview"),preview_image_,preview_status_,pause_,width_label_,width_})
            preview_panel.Children().Append(child);

        design_=ComboBox{}; design_.Header(box_value(L"Original design")); design_.HorizontalAlignment(HorizontalAlignment::Stretch);
        AutomationProperties::SetName(design_,L"Original design");
        auto add_design=[&](wchar_t const* name,char const* id) {
            ComboBoxItem item; item.Content(box_value(name)); item.Tag(box_value(to_hstring(id)));
            design_.Items().Append(item);
        };
        add_design(L"Plain knit","");
        add_design(L"By app (authored or icon-derived)","@by-app");
        add_design(L"Shared Zigzag (each app's colors)","@shared-zigzag");
        for (std::size_t i=0;i<designs::ws_design_theme_count();++i) {
            auto theme=designs::ws_design_theme_at(i);
            auto name=to_hstring(theme->name); add_design(name.c_str(),theme->id);
        }
        design_.SelectedIndex(0);
        material_=ComboBox{}; material_.Header(box_value(L"Plain material")); material_.HorizontalAlignment(HorizontalAlignment::Stretch);
        material_.Items().Append(box_value(L"Pattern knit")); material_.Items().Append(box_value(L"Raised stockinette"));
        material_.SelectedIndex(static_cast<int>(contracts::Configuration{}.material_style));
        AutomationProperties::SetName(material_,L"Plain material");
        stitch_=ComboBox{}; stitch_.Header(box_value(L"Plain stitch")); stitch_.HorizontalAlignment(HorizontalAlignment::Stretch);
        for (auto name:{L"Stockinette",L"Rib",L"Garter"}) stitch_.Items().Append(box_value(name));
        stitch_.SelectedIndex(0); AutomationProperties::SetName(stitch_,L"Plain stitch");
        plain_group_=Stack(12);
        plain_group_.Children().Append(material_); plain_group_.Children().Append(stitch_);
        plain_group_.Children().Append(Caption(L"Raised material uses stockinette; Pattern knit restores the selected plain stitch."));
        rows_label_=TextBlock{}; rows_label_.Text(L"Stitch size: 6 rows across the border");
        rows_=Slider{}; rows_.Minimum(contracts::minimum_stitch_rows); rows_.Maximum(contracts::maximum_stitch_rows);
        rows_.StepFrequency(0.5); rows_.Value(6);
        AutomationProperties::SetName(rows_,L"Stitch rows across border");
        AutomationProperties::SetHelpText(rows_,L"1.5 to 12 rows across the band. More rows make smaller stitches.");
        color_=ColorPicker{}; color_.Color(ToColor(contracts::Configuration{}.yarn_argb)); color_.IsAlphaEnabled(false);
        color_.IsMoreButtonVisible(false); color_.MaxWidth(480);
        AutomationProperties::SetName(color_,L"Base yarn color");
        yarn_swatch_=Border{}; ApplyStyle(yarn_swatch_,L"WsSwatch");
        yarn_hex_=TextBlock{}; yarn_hex_.VerticalAlignment(VerticalAlignment::Center); StyleSecondary(yarn_hex_);
        TextBlock yarn_title; yarn_title.Text(L"Base yarn color (opaque)"); yarn_title.VerticalAlignment(VerticalAlignment::Center);
        auto yarn_header=StackPanel{}; yarn_header.Orientation(Orientation::Horizontal); yarn_header.Spacing(12);
        yarn_header.Children().Append(yarn_swatch_); yarn_header.Children().Append(yarn_title); yarn_header.Children().Append(yarn_hex_);
        yarn_expander_=Expander{}; yarn_expander_.HorizontalAlignment(HorizontalAlignment::Stretch);
        yarn_expander_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        yarn_expander_.Header(yarn_header); yarn_expander_.Content(color_); UpdateYarnSummary();
        auto appearance=Stack(12);
        appearance.Children().Append(Section(L"Appearance")); appearance.Children().Append(design_);
        appearance.Children().Append(Caption(L"Original charts keep their authored stitches, material, corners and cuffs. By app keeps mapped palettes and samples unknown app icons. Shared Zigzag samples each app's colors. Automatic previews are representative, not a selected app."));
        appearance.Children().Append(plain_group_); appearance.Children().Append(rows_label_); appearance.Children().Append(rows_);
        appearance.Children().Append(yarn_expander_);

        apps_status_=TextBlock{}; apps_status_.Text(L"Loading host-owned application list…");
        apps_status_.TextWrapping(TextWrapping::Wrap); StyleSecondary(apps_status_);
        AutomationProperties::SetName(apps_status_,L"Application choices status");
        AutomationProperties::SetLiveSetting(apps_status_,Automation::Peers::AutomationLiveSetting::Polite);
        apps_all_on_=Button{}; apps_all_on_.Content(box_value(L"All apps on"));
        apps_all_off_=Button{}; apps_all_off_.Content(box_value(L"All apps off"));
        AutomationProperties::SetName(apps_all_on_,L"All applications on");
        AutomationProperties::SetName(apps_all_off_,L"All applications off");
        apps_default_=ToggleSwitch{}; apps_default_.Header(box_value(L"Default for newly discovered apps"));
        apps_default_.OnContent(box_value(L"Enabled")); apps_default_.OffContent(box_value(L"Disabled"));
        AutomationProperties::SetName(apps_default_,L"Default for new applications");
        apps_list_=ListView{}; apps_list_.MaxHeight(340);
        apps_list_.SelectionMode(ListViewSelectionMode::None);
        AutomationProperties::SetName(apps_list_,L"Running applications and saved non-running choices");
        auto apps=Stack(12);
        apps.Children().Append(Section(L"Applications")); apps.Children().Append(apps_status_);
        apps.Children().Append(ButtonGrid({apps_all_on_,apps_all_off_})); apps.Children().Append(apps_default_);
        apps.Children().Append(Caption(L"All on/off also clears saved choices. Changing only the default preserves explicit choices. Identity comes from supported Windows properties, not publisher verification."));
        apps.Children().Append(apps_list_);

        BuildImports();
        rules_save_.Style(Builtin(L"AccentButtonStyle"));
        personal_summary_=Caption(L"Connecting to host…");
        auto personal_header=Stack(2);
        personal_header.Children().Append(Group(L"Personal designs")); personal_header.Children().Append(personal_summary_);
        auto personal=Stack(16);
        personal.Children().Append(Caption(L"Rules: executable prefix = #RRGGBB [chart name]. Only verified Windows executable metadata binds rules; unmatched and ambiguous rules remain unbound. No chart means plain knit. PNGs retain their exact yarn colors."));
        personal.Children().Append(rules_); personal.Children().Append(ButtonGrid({rules_save_,rules_import_}));
        personal.Children().Append(Group(L"Rule diagnostics (a matched identity may still lose to a longer prefix)"));
        personal.Children().Append(rule_diagnostics_);
        personal.Children().Append(Group(L"Saved bindings (applied)")); personal.Children().Append(saved_bindings_);
        personal.Children().Append(Group(L"PNG charts")); personal.Children().Append(chart_name_); personal.Children().Append(charts_);
        personal.Children().Append(chart_summary_);
        personal.Children().Append(ButtonGrid({chart_select_,chart_plain_,chart_add_,chart_replace_,chart_remove_}));
        personal.Children().Append(Group(L"Precise application color")); personal.Children().Append(color_app_);
        personal.Children().Append(app_rgb_); personal.Children().Append(app_color_save_);
        personal_expander_=Expander{}; personal_expander_.HorizontalAlignment(HorizontalAlignment::Stretch);
        personal_expander_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        personal_expander_.Header(personal_header); personal_expander_.Content(personal);
        AutomationProperties::SetName(personal_expander_,L"Personal designs");
        auto personal_region=Stack(8);
        personal_region.Children().Append(personal_expander_); personal_region.Children().Append(import_status_);

        auto heading=Heading(L"Window Sweaters",L"TitleTextBlockStyle",Automation::Peers::AutomationHeadingLevel::Level1);
        auto header=Stack(4); header.Children().Append(heading);
        header.Children().Append(Caption(L"Native knitted borders. Closing settings keeps decoration running."));
        left_=Stack(16); left_.Children().Append(header);
        left_.Children().Append(Card(status_panel)); left_.Children().Append(Card(preview_panel));
        right_=Stack(16); right_.Children().Append(Card(appearance)); right_.Children().Append(Card(apps));
        right_.Children().Append(personal_region);

        root_=Grid{}; root_.Margin(Thickness{24}); root_.ColumnSpacing(24);
        ColumnDefinition preview_column; preview_column.Width(GridLength{1.15,GridUnitType::Star}); preview_column.MaxWidth(560);
        ColumnDefinition controls_column; controls_column.Width(GridLength{1,GridUnitType::Star});
        root_.ColumnDefinitions().Append(preview_column); root_.ColumnDefinitions().Append(controls_column);
        for (int row=0;row<2;++row) {
            RowDefinition definition; definition.Height(GridLength{1,GridUnitType::Auto}); root_.RowDefinitions().Append(definition);
        }
        Grid::SetRow(left_,0); Grid::SetColumn(left_,0); Grid::SetRow(right_,0); Grid::SetColumn(right_,1);
        root_.Children().Append(left_); root_.Children().Append(right_);
        scroll_=ScrollViewer{}; scroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll_.HorizontalScrollMode(ScrollMode::Disabled); scroll_.Content(root_);
        page_=Grid{}; ApplyStyle(page_,L"WsPage"); page_.Children().Append(scroll_); window_.Content(page_);
    }
    void ApplyLayoutClass(double width) {
        bool wide=wide_?width>=820:width>=860;
        if (wide==wide_) return;
        wide_=wide;
        auto columns=root_.ColumnDefinitions();
        columns.GetAt(0).MaxWidth(wide?560:std::numeric_limits<double>::infinity());
        columns.GetAt(1).Width(wide?GridLength{1,GridUnitType::Star}:GridLength{0,GridUnitType::Pixel});
        root_.ColumnSpacing(wide?24:0); root_.RowSpacing(wide?0:24);
        Grid::SetRow(right_,wide?0:1); Grid::SetColumn(right_,wide?1:0);
    }
    void PlaceWindow(HWND window) {
        auto monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor,&info)) return;
        double scale=GetDpiForWindow(window)/96.0;
        int available_width=info.rcWork.right-info.rcWork.left, available_height=info.rcWork.bottom-info.rcWork.top;
        int margin=static_cast<int>(16*scale);
        int width=std::max(1,std::min(static_cast<int>(1000*scale),available_width-2*margin));
        int height=std::max(1,std::min(static_cast<int>(720*scale),available_height-2*margin));
        SetWindowPos(window,nullptr,info.rcWork.left+(available_width-width)/2,info.rcWork.top+(available_height-height)/2,
                     width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void UpdateYarnSummary() {
        auto color=color_.Color(); wchar_t hex[8]{};
        swprintf_s(hex,L"#%02X%02X%02X",color.R,color.G,color.B);
        yarn_hex_.Text(hex); yarn_swatch_.Background(Media::SolidColorBrush{color});
        AutomationProperties::SetName(yarn_expander_,hstring(L"Base yarn color ")+hex+L", expand to edit");
    }
    void BuildImports() {
        import_status_=TextBlock{}; import_status_.Text(L"Connecting to host…");
        import_status_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(import_status_,L"Personal import saved status and errors");
        AutomationProperties::SetLiveSetting(import_status_,Automation::Peers::AutomationLiveSetting::Polite);
        rules_=TextBox{}; rules_.Header(box_value(L"Declarative colorway rules (UTF-8)"));
        rules_.AcceptsReturn(true); rules_.TextWrapping(TextWrapping::Wrap); rules_.MaxLength(65536);
        rules_.MinHeight(120); rules_.MaxHeight(260);
        AutomationProperties::SetName(rules_,L"Personal colorway rules editor");
        rules_save_=Button{}; rules_save_.Content(box_value(L"Save rules"));
        rules_import_=Button{}; rules_import_.Content(box_value(L"Import local rules file"));
        rule_diagnostics_=TextBlock{}; rule_diagnostics_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(rule_diagnostics_,L"Per-rule binding diagnostics");
        saved_bindings_=TextBlock{}; saved_bindings_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(saved_bindings_,L"Saved personal application bindings");
        chart_name_=TextBox{}; chart_name_.Header(box_value(L"PNG semantic name")); chart_name_.MaxLength(63);
        AutomationProperties::SetName(chart_name_,L"PNG semantic name");
        charts_=ComboBox{}; charts_.Header(box_value(L"Imported charts and revisions"));
        charts_.HorizontalAlignment(HorizontalAlignment::Stretch); charts_.PlaceholderText(L"Choose an imported chart");
        AutomationProperties::SetName(charts_,L"Imported charts and revisions");
        chart_summary_=TextBlock{}; chart_summary_.TextWrapping(TextWrapping::Wrap);
        AutomationProperties::SetName(chart_summary_,L"Chosen and active imported chart");
        AutomationProperties::SetLiveSetting(chart_summary_,Automation::Peers::AutomationLiveSetting::Polite);
        chart_add_=Button{}; chart_add_.Content(box_value(L"Import PNG"));
        chart_select_=Button{}; chart_select_.Content(box_value(L"Use selected chart"));
        AutomationProperties::SetName(chart_select_,L"Use selected chart");
        chart_plain_=Button{}; chart_plain_.Content(box_value(L"Clear imported chart"));
        AutomationProperties::SetName(chart_plain_,L"Clear imported chart");
        chart_plain_.Visibility(Visibility::Collapsed);
        chart_replace_=Button{}; chart_replace_.Content(box_value(L"Replace PNG"));
        chart_remove_=Button{}; chart_remove_.Content(box_value(L"Remove PNG"));
        color_app_=ComboBox{}; color_app_.Header(box_value(L"Application identity for precise color edit"));
        color_app_.HorizontalAlignment(HorizontalAlignment::Stretch);
        app_rgb_=TextBox{}; app_rgb_.Header(box_value(L"Application RGB (six hexadecimal digits)")); app_rgb_.MaxLength(6);
        app_color_save_=Button{}; app_color_save_.Content(box_value(L"Save application color (chosen chart, or plain if none)"));
        app_color_save_.HorizontalAlignment(HorizontalAlignment::Stretch);
        auto weak=get_weak();
        charts_.SelectionChanged([weak](auto const&,auto const&) {
            if (auto app=weak.get()) {
                if (!app->updating_imports_) ++app->chart_choice_revision_;
                app->RefreshChartActions();
            }
        });
        rules_.TextChanged([weak](auto const&,auto const&) {
            if (auto app=weak.get(); app && !app->updating_imports_) app->rules_draft_.Edit();
        });
        rules_save_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->Dispatch(L"rules-save",[&] {
                app->ChangeImport(contracts::SetRulesText{to_string(app->rules_.Text())});
            });
        });
        rules_import_.Click([weak](auto const&,auto const&) { if (auto app=weak.get()) app->PickImport(0); });
        chart_add_.Click([weak](auto const&,auto const&) { if (auto app=weak.get()) app->PickImport(1); });
        chart_replace_.Click([weak](auto const&,auto const&) { if (auto app=weak.get()) app->PickImport(2); });
        chart_select_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) {
                auto id=app->SelectedChart();
                if (id.empty() || !app->ChartById(id)) {
                    app->import_status_.Text(L"Choose an imported chart first, then select Use selected chart."); return;
                }
                app->ChangeImport(contracts::SelectPng{std::move(id)});
            }
        });
        chart_plain_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->ChangeImport(contracts::SelectPng{});
        });
        chart_remove_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) {
                auto id=app->SelectedChart();
                if (id.empty() || !app->ChartById(id)) {
                    app->import_status_.Text(L"Choose an imported chart to remove."); return;
                }
                app->ChangeImport(contracts::RemovePng{std::move(id)});
            }
        });
        app_color_save_.Click([weak](auto const&,auto const&) {
            if (auto app=weak.get()) app->Dispatch(L"application-color-save",[&] {
                auto index=app->color_app_.SelectedIndex();
                auto text=to_string(app->app_rgb_.Text()); std::uint32_t rgb=0;
                if (index<0 || static_cast<std::size_t>(index)>=app->color_identities_.size() || text.size()!=6) {
                    app->import_status_.Text(L"Choose a typed application identity and enter exactly six RGB digits."); return;
                }
                for (char c:text) {
                    int digit=c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1;
                    if (digit<0) { app->import_status_.Text(L"Application RGB needs six hexadecimal digits."); return; }
                    rgb=(rgb<<4)|static_cast<std::uint32_t>(digit);
                }
                auto chart=app->SelectedChart();
                app->ChangeImport(contracts::SetAppColor{app->color_identities_[index],rgb|0xff000000u,
                    chart.empty()?std::nullopt:std::optional{std::move(chart)}});
            });
        });
    }
    contracts::ChartMetadata const* ChartById(std::string const& id) const {
        if (!rendered_imports_) return nullptr;
        auto found=std::ranges::find(rendered_imports_->charts,id,&contracts::ChartMetadata::id);
        return found==rendered_imports_->charts.end()?nullptr:&*found;
    }
    static hstring ChartLabel(contracts::ChartMetadata const& chart) {
        return to_hstring(chart.name)+L" · revision "+to_hstring(chart.revision);
    }
    void RefreshChartActions() {
        auto chosen=SelectedChart();
        auto const* chosen_chart=chosen.empty()?nullptr:ChartById(chosen);
        std::string active=rendered_imports_?rendered_imports_->selected_id:std::string{};
        auto const* active_chart=active.empty()?nullptr:ChartById(active);
        auto actions=settings::ChartActions(imports_enabled_,chosen_chart!=nullptr,chosen==active,!active.empty());
        chart_select_.IsEnabled(actions.use_chart);
        chart_replace_.IsEnabled(actions.manage_chart);
        chart_remove_.IsEnabled(actions.manage_chart);
        chart_plain_.Visibility(active.empty()?Visibility::Collapsed:Visibility::Visible);
        chart_plain_.IsEnabled(actions.clear_chart);
        hstring summary=!rendered_imports_?hstring{L"Imported chart state is unavailable until the host connects."}:
            active_chart?L"Active imported chart: "+ChartLabel(*active_chart):
            active.empty()?hstring{L"No imported chart is active. Plain knit or the current design is in use."}:
            hstring{L"The active imported chart is unavailable."};
        if (chosen_chart && chosen!=active)
            summary=summary+L"\nChosen: "+ChartLabel(*chosen_chart)+L" — not applied yet. Select “Use selected chart” to apply it.";
        if (chart_summary_.Text()!=summary) chart_summary_.Text(summary);
    }
    void SelectChart(std::string const& id) {
        auto count=charts_.Items().Size();
        for (std::uint32_t index=0;index<count;++index)
            if (to_string(unbox_value<hstring>(charts_.Items().GetAt(index).as<ComboBoxItem>().Tag()))==id) {
                charts_.SelectedIndex(static_cast<int>(index)); return;
            }
    }
    std::string SelectedChart() const {
        auto item=charts_.SelectedItem();
        return item?to_string(unbox_value<hstring>(item.as<ComboBoxItem>().Tag())):"";
    }
    void ChangeImport(contracts::ImportMutation mutation) {
        if (closed_ || !session_) return;
        if (draft_.Pending() || draft_.Current().pending) {
            import_status_.Text(L"Another change is still saving. Wait for it to finish, then try again."); return;
        }
        auto kind=settings::RulesDraft::KindOf(mutation);
        PendingChart chart{settings::ClassifyChartMutation(mutation)};
        if (auto add=std::get_if<contracts::AddPng>(&mutation)) chart.name=add->name;
        else if (auto replace=std::get_if<contracts::ReplacePng>(&mutation)) chart.id=replace->id;
        else if (auto remove=std::get_if<contracts::RemovePng>(&mutation)) chart.id=remove->id;
        else if (auto select=std::get_if<contracts::SelectPng>(&mutation)) chart.id=select->id;
        if (rendered_imports_) chart.before=rendered_imports_->charts;
        chart.choice_revision=chart_choice_revision_;
        auto generation=session_->Import(draft_.Displayed(),std::move(mutation));
        if (!generation) { import_status_.Text(L"Import not queued. Refresh the host connection before editing."); return; }
        import_generation_=generation; rules_draft_.Begin(kind); pending_chart_=std::move(chart);
        draft_.Begin(generation,draft_.Controls());
        import_status_.Text(L"Validating and saving on the host…");
        EnableImports(false);
    }
    fire_and_forget PickImport(unsigned kind) {
        auto lifetime=get_strong();
        try {
            if (closed_ || picker_pending_ || draft_.Pending() || !draft_.Current().connected) co_return;
            auto displayed=draft_.Displayed();
            auto id=SelectedChart(); auto name=to_string(chart_name_.Text());
            if (kind==2 && id.empty()) { import_status_.Text(L"Select an imported chart to replace explicitly."); co_return; }
            if (kind==1 && name.empty()) { import_status_.Text(L"Enter a semantic chart name before importing."); co_return; }
            picker_pending_=true; EnableImports(false);
            Windows::Storage::Pickers::FileOpenPicker picker;
            HWND owner=nullptr; check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&owner));
            check_hresult(picker.as<IInitializeWithWindow>()->Initialize(owner));
            picker.FileTypeFilter().Append(kind==0?L".txt":L".png");
            auto file=co_await picker.PickSingleFileAsync();
            picker_pending_=false;
            if (closed_) co_return;
            EnableImports(draft_.Current().connected && !draft_.Pending());
            if (!file) co_return;
            if (displayed.server_instance!=draft_.Displayed().server_instance ||
                displayed.configuration.revision!=draft_.Displayed().configuration.revision) {
                import_status_.Text(L"Host changed while the picker was open. Select the file again."); co_return;
            }
            auto path=std::wstring(file.Path());
            if (kind==0) ChangeImport(contracts::ReadRulesFile{std::move(path)});
            else if (kind==1) ChangeImport(contracts::AddPng{std::move(name),std::move(path)});
            else ChangeImport(contracts::ReplacePng{std::move(id),std::move(path),
                name.empty()?std::nullopt:std::optional{std::move(name)}});
        } catch (hresult_error const& error) {
            picker_pending_=false;
            if (!closed_) { import_status_.Text(L"Local file picker failed: "+error.message()); EnableImports(false); }
        }
    }
    void EnableImports(bool enabled) {
        enabled=enabled && !picker_pending_ && !presentation_failed_;
        imports_enabled_=enabled;
        for (auto const& button:{rules_save_,rules_import_,chart_add_,app_color_save_})
            button.IsEnabled(enabled);
        rules_.IsEnabled(enabled); charts_.IsEnabled(enabled); chart_name_.IsEnabled(enabled);
        color_app_.IsEnabled(enabled); app_rgb_.IsEnabled(enabled);
        RefreshChartActions();
    }
    struct PendingChart {
        settings::ChartMutation kind=settings::ChartMutation::none;
        std::string name,id;
        std::vector<contracts::ChartMetadata> before;
        std::uint64_t choice_revision{};
    };
    hstring ChartResult(PendingChart const& pending,contracts::ImportPreferences const& p,std::string const& highlighted,
        hstring const& design) const {
        auto find=[&](std::vector<contracts::ChartMetadata> const& charts,std::string const& id) {
            auto found=std::ranges::find(charts,id,&contracts::ChartMetadata::id);
            return found==charts.end()?nullptr:&*found;
        };
        using Kind=settings::ChartMutation;
        auto apply=L" Select “Use selected chart” to apply it.";
        if (pending.kind==Kind::add) {
            auto const* chart=highlighted.empty()?nullptr:find(p.charts,highlighted);
            return chart?L"Imported "+ChartLabel(*chart)+L". It is highlighted, not applied."+apply:
                L"Imported "+to_hstring(pending.name)+L". Choose it in the list, then select “Use selected chart”.";
        }
        if (pending.kind==Kind::replace) {
            auto const* chart=find(p.charts,pending.id);
            if (!chart) return {};
            return L"Replaced "+ChartLabel(*chart)+(p.selected_id==pending.id?hstring{L". It is active in the preview."}:
                hstring{L". It is not applied."}+apply);
        }
        if (pending.kind==Kind::remove) {
            auto const* chart=find(pending.before,pending.id);
            return L"Removed "+(chart?to_hstring(chart->name):hstring{L"the imported chart"})+L".";
        }
        if (pending.kind==Kind::select) {
            auto const* chart=find(p.charts,pending.id);
            return p.selected_id==pending.id&&chart?L"Using imported chart "+ChartLabel(*chart)+L" in the preview.":
                hstring{L"The imported chart selection was not retained by the host."};
        }
        if (pending.kind==Kind::clear)
            return p.selected_id.empty()?L"Imported chart cleared. The selected design resumes: "+design+L".":
                hstring{L"The host kept an imported chart selected."};
        return {};
    }
    static hstring DesignName(contracts::Configuration const& configuration) {
        switch (configuration.design_selection) {
        case contracts::DesignSelection::by_app: return L"By app";
        case contracts::DesignSelection::shared_zigzag: return L"Shared Zigzag";
        case contracts::DesignSelection::original: {
            auto theme=designs::ws_design_theme_by_id(configuration.design_id.c_str());
            return theme?to_hstring(theme->name):hstring{L"Original design"};
        }
        default: return L"Plain knit";
        }
    }
    void RenderImports(settings::ConnectionState const& state) {
        bool settled=import_generation_ && state.settled_edit_generation>=import_generation_;
        std::string highlight;
        if (settled) {
            import_generation_=0;
            bool saved=state.status==contracts::Status::saved;
            rules_draft_.Settle(saved);
            auto pending=std::exchange(pending_chart_,PendingChart{});
            if (saved && pending.kind!=settings::ChartMutation::none) {
                auto const& after=state.configuration.imports;
                highlight=settings::ChartToHighlight(pending.kind,pending.name,pending.id,pending.before,after.charts,
                    chart_choice_revision_!=pending.choice_revision,saved);
                import_result_=ChartResult(pending,after,highlight,DesignName(state.configuration));
                import_result_revision_=state.configuration.revision;
            }
        }
        auto const& p=state.configuration.imports;
        if (!rendered_imports_ || *rendered_imports_!=p) {
            updating_imports_=true;
            if (!rules_draft_.Dirty()) rules_.Text(to_hstring(p.document));
            auto selected=SelectedChart();
            charts_.Items().Clear();
            int selectedIndex=-1;
            for (auto const& c:p.charts) {
                ComboBoxItem item;
                item.Content(box_value(ChartLabel(c)+(c.id==p.selected_id?L" · active":L"")));
                item.Tag(box_value(to_hstring(c.id))); charts_.Items().Append(item);
                if (c.id==selected) selectedIndex=static_cast<int>(charts_.Items().Size()-1);
            }
            charts_.SelectedIndex(selectedIndex);
            std::wstring diagnostics;
            for (std::size_t i=0;i<p.diagnostics.size();++i) {
                auto const& d=p.diagnostics[i];
                diagnostics+=L"Rule "+std::to_wstring(i+1)+L": "+
                    (d.match==contracts::RuleMatch::matched?L"matched identity":d.match==contracts::RuleMatch::ambiguous?L"ambiguous — not bound":L"unmatched — not bound");
                for (auto const& candidate:d.candidates) diagnostics+=L"\n  "+candidate.Key();
                diagnostics+=L"\n";
            }
            rule_diagnostics_.Text(diagnostics.empty()?L"No imported rules.":diagnostics);
            std::wstring bindings;
            for (auto const& b:p.bindings) {
                wchar_t rgb[8]{}; swprintf_s(rgb,L"#%06X",b.rule.base_argb&0xffffffu);
                std::wstring source=b.explicit_edit?std::wstring(L"explicit edit"):
                    L"prefix "+std::wstring(to_hstring(b.rule.original_prefix));
                std::wstring chart=b.rule.chart_name?std::wstring(to_hstring(*b.rule.chart_name)):std::wstring(L"plain");
                bindings+=b.application.Key()+L"\n  "+source+L" · "+rgb+L" · "+chart+L"\n";
            }
            saved_bindings_.Text(bindings.empty()?L"No saved personal bindings.":bindings);
            personal_summary_.Text(std::to_wstring(p.charts.size())+(p.charts.size()==1?L" chart · ":L" charts · ")+
                std::to_wstring(p.bindings.size())+(p.bindings.size()==1?L" saved binding":L" saved bindings"));
            rendered_imports_=p; updating_imports_=false;
        } else if (settled && !rules_draft_.Dirty()) {
            updating_imports_=true; rules_.Text(to_hstring(p.document)); updating_imports_=false;
        }
        if (!highlight.empty()) { updating_imports_=true; SelectChart(highlight); updating_imports_=false; }
        EnableImports(state.connected && state.configuration_authoritative && !draft_.Pending() && !state.pending);
        if (import_generation_ || state.pending) import_status_.Text(L"Validating and saving on the host…");
        else if (!state.import_failure.reason.empty())
            import_status_.Text(L"Import rejected: "+to_hstring(state.import_failure.reason)+
                (state.import_failure.line?L" · line "+std::to_wstring(state.import_failure.line):L"")+
                L". Previous committed charts and colors remain active.");
        else if (state.status==contracts::Status::saved) {
            auto saved=L"Host-owned imports · saved revision "+std::to_wstring(state.configuration.revision);
            if (!import_result_.empty() && import_result_revision_==state.configuration.revision)
                import_status_.Text(import_result_+L" ("+hstring(saved)+L")");
            else import_status_.Text(saved);
        }
        else import_status_.Text(state.detail);
    }
    void EnableApps(bool enabled) {
        enabled=enabled && presentation_ready_ && !presentation_failed_;
        apps_all_on_.IsEnabled(enabled); apps_all_off_.IsEnabled(enabled); apps_default_.IsEnabled(enabled);
        for (auto const& [identity,row]:app_rows_) {
            (void)identity;
            row.toggle.IsEnabled(enabled); row.remove.IsEnabled(enabled);
        }
    }
    void ChangeApps(applications::EligibilityMutation mutation) {
        if (updating_apps_ || !apps_session_ || !presentation_ready_ || presentation_failed_) return;
        if (!apps_draft_.CanEdit()) {
            DrawApps(apps_draft_.Controls()); return;
        }
        auto generation=apps_session_->Change(apps_draft_.Displayed(),mutation);
        if (!generation) { DrawApps(apps_draft_.Controls()); return; }
        apps_draft_.Begin(generation,mutation);
        DrawApps(apps_draft_.Controls());
        EnableApps(apps_draft_.CanInteract()); apps_status_.Text(L"Saving application choice…");
    }
    struct AppRow {
        StackPanel panel{nullptr};
        ToggleSwitch toggle{nullptr};
        TextBlock presence{nullptr};
        Button remove{nullptr};
    };
    static ScrollViewer FindScrollViewer(DependencyObject const& root) {
        if (!root) return nullptr;
        if (auto viewer=root.try_as<ScrollViewer>()) return viewer;
        auto count=Media::VisualTreeHelper::GetChildrenCount(root);
        for (int index=0;index<count;++index)
            if (auto viewer=FindScrollViewer(Media::VisualTreeHelper::GetChild(root,index))) return viewer;
        return nullptr;
    }
    void DrawApps(contracts::ApplicationsSnapshot const& snapshot) {
        auto root=scroll_.XamlRoot();
        if (!presentation_ready_ || !root) throw hresult_error(E_ILLEGAL_METHOD_CALL,L"Apps presentation requires an attached loaded XamlRoot.");
        const bool preserveContext=rendered_apps_.has_value();
        presentation_stage_=L"apps-focus-capture";
        Windows::Foundation::IInspectable focused{nullptr};
        if (preserveContext) focused=Input::FocusManager::GetFocusedElement(root);
        auto focusState=focused.try_as<Control>()?focused.as<Control>().FocusState():FocusState::Unfocused;
        std::optional<applications::AppIdentity> focusedOwner;
        auto focusedPart=settings::AppFocusControl::toggle;
        for (auto const& [identity,row]:app_rows_) {
            if (focused==row.toggle || focused==row.remove) {
                focusedOwner=identity;
                focusedPart=focused==row.remove?settings::AppFocusControl::remove_choice:settings::AppFocusControl::toggle;
                break;
            }
        }
        auto outerOffset=preserveContext?scroll_.VerticalOffset():0;
        presentation_stage_=L"apps-viewport-capture";
        auto inner=preserveContext?FindScrollViewer(apps_list_):ScrollViewer{nullptr};
        auto innerOffset=inner?inner.VerticalOffset():0;
        updating_apps_=true;
        if (apps_default_.IsOn()!=snapshot.default_enabled) apps_default_.IsOn(snapshot.default_enabled);
        auto weak=get_weak();
        presentation_stage_=L"apps-row-construction";
        bool layoutChanged=false;
        for (auto const& row:snapshot.applications) {
            if (app_rows_.contains(row.identity)) continue;
            layoutChanged=true;
            AppRow controls;
            auto panel=StackPanel{}; panel.Spacing(4); panel.Margin(Thickness{0,8,0,8});
            auto label=row.display_name.empty()?row.identity.Key():row.display_name;
            auto toggle=ToggleSwitch{}; toggle.Header(box_value(label));
            toggle.OnContent(box_value(L"Sweater enabled")); toggle.OffContent(box_value(L"Sweater disabled"));
            AutomationProperties::SetName(toggle,label+L" sweater enabled");
            AutomationProperties::SetHelpText(toggle,row.identity.Key());
            panel.Children().Append(toggle);
            auto presence=TextBlock{}; StyleSecondary(presence);
            panel.Children().Append(presence);
            auto identity=TextBlock{}; identity.Text(row.identity.Key()); identity.TextWrapping(TextWrapping::Wrap);
            StyleSecondary(identity); panel.Children().Append(identity);
            auto remove=Button{}; remove.Content(box_value(L"Remove saved choice")); remove.MinHeight(44);
            AutomationProperties::SetName(remove,L"Remove saved choice for "+label);
            remove.Visibility(Visibility::Collapsed); panel.Children().Append(remove);
            remove.Click([weak,key=row.identity](auto const&,auto const&) {
                if (auto app=weak.get()) {
                    applications::EligibilityMutation mutation;
                    mutation.edits.emplace_back(applications::RemoveApplicationPreference{key});
                    app->Dispatch(L"apps-remove-choice",[&]{app->ChangeApps(std::move(mutation));});
                }
            });
            toggle.Toggled([weak,key=row.identity](auto const& sender,auto const&) {
                if (auto app=weak.get(); app && !app->updating_apps_) {
                    applications::EligibilityMutation mutation;
                    mutation.edits.emplace_back(applications::SetApplicationPreference{
                        key,sender.template as<ToggleSwitch>().IsOn()});
                    app->Dispatch(L"apps-toggle-change",[&]{app->ChangeApps(std::move(mutation));});
                }
            });
            controls.panel=panel; controls.toggle=toggle; controls.presence=presence; controls.remove=remove;
            app_rows_.emplace(row.identity,std::move(controls));
        }
        presentation_stage_=L"apps-items-reconcile";
        auto edits=settings::ReconcileApplicationItems(app_item_order_,snapshot.applications);
        layoutChanged|=!edits.empty();
        for (auto const& edit:edits) {
            auto index=static_cast<std::uint32_t>(edit.index);
            if (edit.change==settings::AppItemChange::remove) {
                apps_list_.Items().RemoveAt(index);
                app_rows_.erase(app_item_order_[edit.index]);
                app_item_order_.erase(app_item_order_.begin()+edit.index);
            } else if (edit.change==settings::AppItemChange::insert) {
                auto const& key=snapshot.applications[edit.index].identity;
                apps_list_.Items().InsertAt(index,app_rows_.at(key).panel);
                app_item_order_.insert(app_item_order_.begin()+edit.index,key);
            } else {
                auto item=apps_list_.Items().GetAt(index);
                auto key=app_item_order_[edit.index];
                apps_list_.Items().RemoveAt(index); app_item_order_.erase(app_item_order_.begin()+edit.index);
                apps_list_.Items().InsertAt(static_cast<std::uint32_t>(edit.destination),item);
                app_item_order_.insert(app_item_order_.begin()+edit.destination,std::move(key));
            }
        }
        presentation_stage_=L"apps-row-properties";
        for (auto const& row:snapshot.applications) {
            auto& controls=app_rows_.at(row.identity);
            auto label=row.display_name.empty()?row.identity.Key():row.display_name;
            if (unbox_value<hstring>(controls.toggle.Header())!=label) {
                layoutChanged=true;
                controls.toggle.Header(box_value(label));
                AutomationProperties::SetName(controls.toggle,label+L" sweater enabled");
                AutomationProperties::SetName(controls.remove,L"Remove saved choice for "+label);
            }
            if (controls.toggle.IsOn()!=row.enabled) controls.toggle.IsOn(row.enabled);
            auto presence=row.presence==applications::ApplicationPresence::Running?
                L"Running · "+to_hstring(row.window_count)+L" windows · "+to_hstring(row.process_count)+L" processes":
                hstring{L"Saved choice · not running"};
            if (controls.presence.Text()!=presence) { layoutChanged=true; controls.presence.Text(presence); }
            auto visibility=row.explicit_preference?Visibility::Visible:Visibility::Collapsed;
            if (controls.remove.Visibility()!=visibility) {
                layoutChanged=true; controls.remove.Visibility(visibility);
            }
        }
        layoutChanged|=app_diagnostics_.size()!=snapshot.unavailable.size();
        while (app_diagnostics_.size()>snapshot.unavailable.size()) {
            apps_list_.Items().RemoveAtEnd(); app_diagnostics_.pop_back();
        }
        while (app_diagnostics_.size()<snapshot.unavailable.size()) {
            auto diagnostic=TextBlock{}; diagnostic.TextWrapping(TextWrapping::Wrap); StyleSecondary(diagnostic);
            apps_list_.Items().Append(diagnostic); app_diagnostics_.push_back(diagnostic);
        }
        for (std::size_t index=0;index<snapshot.unavailable.size();++index) {
            auto const& row=snapshot.unavailable[index];
            auto diagnostic=app_diagnostics_[index];
            auto text=L"Owner metadata unavailable · "+to_hstring(row.window_count)+
                L" windows · reason "+to_hstring(static_cast<unsigned>(row.unavailable.reason))+
                L" · Win32 "+to_hstring(row.unavailable.nativeError)+L" · no mutable application identity";
            if (diagnostic.Text()!=text) { layoutChanged=true; diagnostic.Text(text); }
        }
        rendered_apps_=snapshot; updating_apps_=false;
        presentation_stage_=L"apps-focus-restore";
        if (focusedOwner) {
            auto owner=app_rows_.find(*focusedOwner);
            auto part=settings::FocusAfterApplicationUpdate(focusedPart,owner!=app_rows_.end(),
                owner!=app_rows_.end() && owner->second.remove.Visibility()==Visibility::Visible);
            Control retainedFocus=part==settings::AppFocusControl::defaults?apps_default_.as<Control>():
                part==settings::AppFocusControl::remove_choice?owner->second.remove.as<Control>():owner->second.toggle.as<Control>();
            auto currentFocus=Input::FocusManager::GetFocusedElement(root);
            if (retainedFocus.IsEnabled() && currentFocus!=retainedFocus)
                retainedFocus.Focus(focusState==FocusState::Unfocused?FocusState::Programmatic:focusState);
        }
        presentation_stage_=L"apps-layout-restore";
        if (preserveContext && layoutChanged) scroll_.UpdateLayout();
        presentation_stage_=L"apps-inner-viewport-restore";
        if (inner && inner.VerticalOffset()!=innerOffset)
            inner.ChangeView(nullptr,box_value(settings::PreservedViewportOffset(innerOffset,inner.ScrollableHeight())).as<Windows::Foundation::IReference<double>>(),nullptr,true);
        presentation_stage_=L"apps-outer-viewport-restore";
        if (preserveContext && scroll_.VerticalOffset()!=outerOffset)
            scroll_.ChangeView(nullptr,box_value(settings::PreservedViewportOffset(outerOffset,scroll_.ScrollableHeight())).as<Windows::Foundation::IReference<double>>(),nullptr,true);
        presentation_stage_=L"apps-presented";
        if (!presentation_success_logged_) {
            presentation_success_logged_=true;
            StartupDiagnostic(presentation_stage_,S_OK,L"First host application snapshot applied to attached native controls.");
        }
    }
    void RenderApps(settings::AppsConnectionState const& state) {
        if (presentation_failed_ || closed_) return;
        if (!presentation_ready_) {
            settings::RetainApplicationPublication(pending_apps_state_,state);
            QueuePendingApps(); return;
        }
        if (closed_ || state.connection.publication_sequence<=apps_draft_.Current().connection.publication_sequence) return;
        bool changed=apps_draft_.Apply(state);
        if (changed && (!rendered_apps_ || *rendered_apps_!=apps_draft_.Controls())) DrawApps(apps_draft_.Controls());
        auto const& connection=apps_draft_.Current().connection;
        if (changed) {
            auto selected=color_app_.SelectedIndex();
            std::optional<applications::AppIdentity> identity;
            if (selected>=0 && static_cast<std::size_t>(selected)<color_identities_.size()) identity=color_identities_[selected];
            std::vector<applications::AppIdentity> next;
            for (auto const& row:apps_draft_.Controls().applications) next.push_back(row.identity);
            if (next!=color_identities_) {
                color_app_.Items().Clear(); color_identities_=std::move(next);
                int index=0;
                for (auto const& row:apps_draft_.Controls().applications) {
                    color_app_.Items().Append(box_value(row.identity.Key()));
                    if (identity && *identity==row.identity) color_app_.SelectedIndex(index);
                    ++index;
                }
            }
        }
        EnableApps(apps_draft_.CanInteract());
        std::wstring status=connection.detail;
        if (connection.connected) {
            if (connection.pending || apps_draft_.Pending()) status=L"Saving application choice…";
            else if (connection.status==contracts::Status::saved)
                status=L"Host application list · saved revision "+std::to_wstring(connection.configuration.revision);
            else if (connection.status==contracts::Status::conflict)
                status=L"Another change won. Application choice was not saved; list refreshed.";
            else if (connection.status==contracts::Status::storage_error)
                status=L"Application save failed. Committed choices remain active (Win32 "+std::to_wstring(connection.error)+L").";
            else if (connection.status==contracts::Status::recovery_required)
                status=L"Preserved configuration requires explicit recovery below.";
        }
        apps_status_.Text(status);
    }
    void ApplyPendingApps() {
        if (closed_ || presentation_failed_ || !pending_apps_state_) return;
        auto root=scroll_.XamlRoot();
        if (!settings::ApplicationPresentationReady(scroll_.IsLoaded(),bool(root),scroll_.ActualWidth(),scroll_.ActualHeight())) return;
        presentation_ready_=true;
        auto state=std::move(*pending_apps_state_);
        pending_apps_state_.reset();
        if (!presentation_admission_logged_) {
            presentation_admission_logged_=true;
            StartupDiagnostic(L"apps-presentation-ready",S_OK,L"Loaded XamlRoot and nonzero native layout accept retained host snapshot.");
        }
        RenderApps(state);
    }
    void QueuePendingApps() {
        if (closed_ || presentation_failed_ || apps_presentation_queued_ || !pending_apps_state_) return;
        auto root=scroll_.XamlRoot();
        if (!settings::ApplicationPresentationReady(scroll_.IsLoaded(),bool(root),scroll_.ActualWidth(),scroll_.ActualHeight())) return;
        apps_presentation_queued_=true;
        auto weak=get_weak();
        if (!window_.DispatcherQueue().TryEnqueue([weak] {
            if (auto app=weak.get()) {
                app->apps_presentation_queued_=false;
                app->Dispatch(L"apps-post-layout-dispatch",[&]{app->ApplyPendingApps();});
            }
        })) {
            apps_presentation_queued_=false;
            throw hresult_error(E_ABORT,L"Native dispatcher rejected retained Apps presentation.");
        }
    }
    template<class Callback> void Dispatch(wchar_t const* stage,Callback&& callback) noexcept {
        if (closed_ || presentation_failed_) return;
        presentation_stage_=stage;
        try { callback(); }
        catch (hresult_error const& error) { PresentationFailure(error.code(),error.message()); }
        catch (std::exception const& error) { PresentationFailure(E_FAIL,to_hstring(error.what())); }
    }
    void PresentationFailure(HRESULT code,std::wstring_view message) noexcept {
        StartupDiagnostic(presentation_stage_,code,message);
        presentation_failed_=true; updating_apps_=false;
        pending_apps_state_.reset();
        try {
            EnableApps(false); EnableControls(false);
            auto text=L"Native UI update failed · HRESULT "+to_hstring(static_cast<std::uint32_t>(code))+
                L" · see settings-startup.log for the exact stage and error.";
            apps_status_.Text(text); status_.Text(text);
        } catch (hresult_error const& error) {
            StartupDiagnostic(L"ui-error-display-failed",error.code(),error.message());
        } catch (std::exception const& error) {
            StartupDiagnostic(L"ui-error-display-failed",E_FAIL,to_hstring(error.what()));
        }
    }
    contracts::Configuration Inputs() const {
        auto configuration=draft_.Displayed().configuration;
        configuration.paused=pause_.IsOn(); configuration.border_width_dip=static_cast<float>(width_.Value());
        auto tag=to_string(unbox_value<hstring>(design_.SelectedItem().as<ComboBoxItem>().Tag()));
        configuration.design_selection=tag.empty()?contracts::DesignSelection::plain:
            tag=="@by-app"?contracts::DesignSelection::by_app:
            tag=="@shared-zigzag"?contracts::DesignSelection::shared_zigzag:contracts::DesignSelection::original;
        configuration.design_id=configuration.design_selection==contracts::DesignSelection::original?tag:"";
        configuration.stitch_style=static_cast<contracts::StitchStyle>(stitch_.SelectedIndex());
        configuration.material_style=static_cast<contracts::MaterialStyle>(material_.SelectedIndex());
        configuration.stitch_rows=static_cast<float>(rows_.Value()); configuration.yarn_argb=ToArgb(color_.Color());
        return configuration;
    }
    void DesignChanged() {
        if (updating_ || !session_ || presentation_failed_) return;
        auto tag=to_string(unbox_value<hstring>(design_.SelectedItem().as<ComboBoxItem>().Tag()));
        if (auto theme=designs::ws_design_theme_by_id(tag.c_str())) {
            updating_=true; color_.Color(ToColor(theme->base_argb)); updating_=false;
        }
        Change();
    }
    void Change() {
        if (updating_ || !session_ || presentation_failed_) return;
        width_label_.Text(L"Border width: "+to_hstring(width_.Value())+L" DIP");
        rows_label_.Text(L"Stitch size: "+to_hstring(rows_.Value())+L" rows across the border");
        EnableControls(draft_.Current().connected&&draft_.Current().status!=contracts::Status::recovery_required);
        auto configuration=Inputs();
        auto generation=session_->Change(draft_.Displayed(),configuration);
        if (!generation) return;
        draft_.Begin(generation,configuration);
        rendered_appearance_=configuration;
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
        RenderImports(state);
        updating_=true;
        if (update.controls_changed && (!rendered_appearance_ ||
            !settings::SameAppearance(*rendered_appearance_,draft_.Controls()))) {
            auto const& configuration=draft_.Controls();
            rendered_appearance_=configuration;
            if (pause_.IsOn()!=configuration.paused) pause_.IsOn(configuration.paused);
            if (width_.Value()!=configuration.border_width_dip) width_.Value(configuration.border_width_dip);
            width_label_.Text(L"Border width: "+to_hstring(configuration.border_width_dip)+L" DIP");
            int selected=configuration.design_selection==contracts::DesignSelection::by_app?1:
                configuration.design_selection==contracts::DesignSelection::shared_zigzag?2:0;
            if (configuration.design_selection==contracts::DesignSelection::original) {
                for (std::uint32_t i=3;i<design_.Items().Size();++i) {
                    auto item=design_.Items().GetAt(i).as<ComboBoxItem>();
                    if (to_string(unbox_value<hstring>(item.Tag()))==configuration.design_id) {
                        selected=static_cast<int>(i); break;
                    }
                }
            }
            if (design_.SelectedIndex()!=selected) design_.SelectedIndex(selected);
            if (stitch_.SelectedIndex()!=static_cast<int>(configuration.stitch_style))
                stitch_.SelectedIndex(static_cast<int>(configuration.stitch_style));
            if (material_.SelectedIndex()!=static_cast<int>(configuration.material_style))
                material_.SelectedIndex(static_cast<int>(configuration.material_style));
            if (rows_.Value()!=configuration.stitch_rows) rows_.Value(configuration.stitch_rows);
            if (ToArgb(color_.Color())!=configuration.yarn_argb) color_.Color(ToColor(configuration.yarn_argb));
            rows_label_.Text(L"Stitch size: "+to_hstring(configuration.stitch_rows)+L" rows across the border");
            if (!preview_configuration_ || !settings::SameAppearance(*preview_configuration_,configuration)) {
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
        auto const& imported_selection=configuration.imports.selected_id;
        if (!imported_selection.empty()) {
            auto chart=std::ranges::find(configuration.imports.charts,imported_selection,&contracts::ChartMetadata::id);
            preview_label=chart==configuration.imports.charts.end()?preview_label+L" · selected imported chart metadata unavailable":
                L"Imported "+to_hstring(chart->name)+L" · revision "+to_hstring(chart->revision)+L" · "+preview_label;
        } else if (configuration.design_selection==contracts::DesignSelection::by_app)
            preview_label=preview_label+L" · representative Finder palette; actual apps differ";
        else if (configuration.design_selection==contracts::DesignSelection::shared_zigzag)
            preview_label=preview_label+L" · representative yarn; each app uses its own icon colors";
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
    ResourceDictionary styles_{nullptr};
    Grid page_{nullptr},root_{nullptr};
    StackPanel left_{nullptr},right_{nullptr},plain_group_{nullptr};
    Expander yarn_expander_{nullptr},personal_expander_{nullptr};
    Border yarn_swatch_{nullptr};
    TextBlock yarn_hex_{nullptr},saved_bindings_{nullptr},personal_summary_{nullptr};
    bool wide_{true};
    Button apps_all_on_{nullptr},apps_all_off_{nullptr};
    ToggleSwitch apps_default_{nullptr};
    ListView apps_list_{nullptr};
    ScrollViewer scroll_{nullptr};
    TextBlock apps_status_{nullptr};
    std::map<applications::AppIdentity,AppRow> app_rows_;
    std::vector<applications::AppIdentity> app_item_order_;
    std::vector<TextBlock> app_diagnostics_;
    std::optional<contracts::Configuration> rendered_appearance_;
    std::optional<contracts::ApplicationsSnapshot> rendered_apps_;
    settings::AppsDraftState apps_draft_;
    bool updating_apps_{};
    bool presentation_ready_{},presentation_failed_{},apps_presentation_queued_{};
    bool presentation_admission_logged_{},presentation_success_logged_{};
    wchar_t const* presentation_stage_=L"native-ui";
    std::optional<settings::AppsConnectionState> pending_apps_state_;
    HWND activation_=nullptr;
    bool updating_=false;
    bool closed_=false;
    std::unique_ptr<settings::Session> session_;
    std::unique_ptr<settings::AppsSession> apps_session_;
    std::unique_ptr<settings::Preview> preview_;
    std::uint64_t preview_generation_=0;
    std::optional<contracts::Configuration> preview_configuration_;
    settings::DraftState draft_;
    TextBox rules_{nullptr},chart_name_{nullptr},app_rgb_{nullptr};
    TextBlock import_status_{nullptr},rule_diagnostics_{nullptr};
    Button rules_save_{nullptr},rules_import_{nullptr},chart_add_{nullptr},chart_select_{nullptr},
        chart_replace_{nullptr},chart_remove_{nullptr},chart_plain_{nullptr},app_color_save_{nullptr};
    TextBlock chart_summary_{nullptr};
    PendingChart pending_chart_;
    hstring import_result_;
    std::uint64_t import_result_revision_{},chart_choice_revision_{};
    bool imports_enabled_{};
    ComboBox charts_{nullptr},color_app_{nullptr};
    std::vector<applications::AppIdentity> color_identities_;
    std::optional<contracts::ImportPreferences> rendered_imports_;
    std::uint64_t import_generation_{};
    bool updating_imports_{},picker_pending_{};
    settings::RulesDraft rules_draft_;
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
