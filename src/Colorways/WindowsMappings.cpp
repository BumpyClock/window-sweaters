// SPDX-License-Identifier: GPL-3.0-only
#include "WindowsMappings.h"

#include <Windows.h>
#include <ShlObj.h>

#include <array>

namespace window_sweaters::colorways {
namespace {
// Public documentation reviewed 2026-10-09; facts do not establish installation or running windows.
constexpr std::string_view teams_package =
    "https://learn.microsoft.com/en-us/microsoftteams/teams-client-vdi-requirements-deploy#remoteapp ; "
    "https://learn.microsoft.com/en-us/troubleshoot/microsoftteams/teams-administration/include-exclude-teams-from-antivirus-dlp";
constexpr std::string_view teams_classic =
    "https://learn.microsoft.com/en-us/troubleshoot/microsoftteams/teams-administration/include-exclude-teams-from-antivirus-dlp#classic-teams";
constexpr std::string_view vscode =
    "https://code.visualstudio.com/docs/setup/windows ; "
    "https://code.visualstudio.com/docs/editor/portable";
constexpr std::string_view chrome =
    "https://raw.githubusercontent.com/chromium/chromium/main/chrome/install_static/google_chrome_install_modes.h ; "
    "https://raw.githubusercontent.com/chromium/chromium/main/chrome/installer/util/util_constants.h ; "
    "https://raw.githubusercontent.com/chromium/chromium/main/chrome/installer/util/helper.cc";
constexpr std::string_view firefox =
    "https://raw.githubusercontent.com/mozilla-firefox/firefox/main/browser/installer/windows/nsis/installer.nsi ; "
    "https://raw.githubusercontent.com/mozilla-firefox/firefox/main/browser/branding/official/branding.nsi ; "
    "https://raw.githubusercontent.com/mozilla-firefox/firefox/main/browser/installer/windows/nsis/defines.nsi.in";
constexpr std::string_view office =
    "https://support.microsoft.com/en-us/office/lifecycle/command-line-switches-for-microsoft-office-products";
constexpr std::string_view edge =
    "https://learn.microsoft.com/en-us/windows/configuration/assigned-access/configuration-file ; "
    "https://learn.microsoft.com/en-us/windows/configuration/assigned-access/examples";
constexpr std::string_view store =
    "https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/troubleshooting-microsoft-store-apps-download-failure ; "
    "https://learn.microsoft.com/en-us/windows/configuration/store/find-aumid";
constexpr std::string_view copilot =
    "https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-windowsai#setcopilothardwarekey ; "
    "https://learn.microsoft.com/en-us/windows/client-management/manage-windows-copilot";
constexpr std::string_view github_copilot =
    "Measured 2026-10-10 from the running standalone GitHub Copilot process image; "
    "exact per-user installation only, not publisher attestation; "
    "known-folder root: https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid";
constexpr std::string_view steam =
    "Measured 2026-10-10: the running Steam client's main window (class SDL_app) is owned by this "
    "steamwebhelper.exe, not Steam.exe; exact default installation only, not publisher attestation; "
    "known-folder root: https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid";
constexpr std::string_view figma =
    "Measured 2026-10-10: the running Figma window owner is %LOCALAPPDATA%\\Figma\\app-<version>\\Figma.exe; "
    "layout: https://github.com/Squirrel/Squirrel.Windows/blob/develop/docs/using/install-process.md ; "
    "not publisher attestation";
constexpr std::string_view zed =
    "https://raw.githubusercontent.com/zed-industries/zed/main/crates/zed/resources/windows/zed.iss ; "
    "https://raw.githubusercontent.com/zed-industries/zed/main/script/bundle-windows.ps1 ; "
    "https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm";
constexpr std::string_view explorer =
    "https://learn.microsoft.com/en-us/windows/configuration/assigned-access/examples ; "
    "https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid";
constexpr std::string_view provisioned =
    "https://learn.microsoft.com/en-us/windows/application-management/remove-provisioned-apps-during-update";
constexpr std::string_view calculator =
    "https://learn.microsoft.com/en-us/windows/configuration/assigned-access/configuration-file ; "
    "https://learn.microsoft.com/en-us/windows/configuration/assigned-access/examples";
constexpr std::string_view weather =
    "https://learn.microsoft.com/en-us/windows/configuration/assigned-access/quickstart-restricted-user-experience ; "
    "https://learn.microsoft.com/en-us/windows/application-management/remove-provisioned-apps-during-update";
constexpr std::string_view measured_package =
    "Measured 2026-10-09 from the installed AppxManifest Application Id and Get-StartApps; "
    "AUMID form: https://learn.microsoft.com/en-us/windows/configuration/store/find-aumid";
constexpr std::string_view windows_app =
    "https://learn.microsoft.com/en-us/windows-app/troubleshoot-basic";
constexpr std::string_view windows_terminal =
    "https://learn.microsoft.com/en-us/windows/terminal/install ; "
    "https://raw.githubusercontent.com/microsoft/terminal/main/src/cascadia/CascadiaPackage/Package.appxmanifest";

constexpr std::string_view measured_owner =
    "Measured 2026-10-11 from the running window owner's QueryFullProcessImageName and GetApplicationUserModelId; "
    "AUMID form: https://learn.microsoft.com/en-us/windows/configuration/store/find-aumid";
constexpr std::string_view visual_studio =
    "Measured 2026-10-11: the running Visual Studio 2026 window owner is "
    "%ProgramFiles%\\Microsoft Visual Studio\\18\\Enterprise\\Common7\\IDE\\devenv.exe; "
    "default layout: https://learn.microsoft.com/en-us/visualstudio/install/change-installation-locations";

constexpr auto mappings = std::to_array<WindowsMapping>({
    {"microsoft-teams", WindowsRoot::PackageIdentity, L"MSTeams_8wekyb3d8bbwe!MSTeams", L"ms-teams.exe", teams_package},
    {"microsoft-teams", WindowsRoot::LocalAppData, L"Microsoft\\Teams\\current\\Teams.exe", L"Teams.exe", teams_classic},
    {"vs-code", WindowsRoot::LocalAppData, L"Programs\\Microsoft VS Code\\Code.exe", L"Code.exe", vscode},
    {"vs-code", WindowsRoot::ProgramFiles, L"Microsoft VS Code\\Code.exe", L"Code.exe", vscode},
    {"vs-code", WindowsRoot::ProgramFilesX86, L"Microsoft VS Code\\Code.exe", L"Code.exe", vscode},
    {"google-chrome", WindowsRoot::ProgramFiles, L"Google\\Chrome\\Application\\chrome.exe", L"chrome.exe", chrome},
    {"google-chrome", WindowsRoot::ProgramFilesX86, L"Google\\Chrome\\Application\\chrome.exe", L"chrome.exe", chrome},
    {"google-chrome", WindowsRoot::LocalAppData, L"Google\\Chrome\\Application\\chrome.exe", L"chrome.exe", chrome},
    {"firefox", WindowsRoot::ProgramFiles, L"Mozilla Firefox\\firefox.exe", L"firefox.exe", firefox},
    {"firefox", WindowsRoot::ProgramFilesX86, L"Mozilla Firefox\\firefox.exe", L"firefox.exe", firefox},
    {"microsoft-word", WindowsRoot::ProgramFiles, L"Microsoft Office\\root\\Office16\\WINWORD.EXE", L"WINWORD.EXE", office},
    {"microsoft-word", WindowsRoot::ProgramFilesX86, L"Microsoft Office\\root\\Office16\\WINWORD.EXE", L"WINWORD.EXE", office},
    {"microsoft-excel", WindowsRoot::ProgramFiles, L"Microsoft Office\\root\\Office16\\EXCEL.EXE", L"EXCEL.EXE", office},
    {"microsoft-excel", WindowsRoot::ProgramFilesX86, L"Microsoft Office\\root\\Office16\\EXCEL.EXE", L"EXCEL.EXE", office},
    {"microsoft-powerpoint", WindowsRoot::ProgramFiles, L"Microsoft Office\\root\\Office16\\POWERPNT.EXE", L"POWERPNT.EXE", office},
    {"microsoft-powerpoint", WindowsRoot::ProgramFilesX86, L"Microsoft Office\\root\\Office16\\POWERPNT.EXE", L"POWERPNT.EXE", office},
    {"microsoft-outlook", WindowsRoot::ProgramFiles, L"Microsoft Office\\root\\Office16\\OUTLOOK.EXE", L"OUTLOOK.EXE", office},
    {"microsoft-outlook", WindowsRoot::ProgramFilesX86, L"Microsoft Office\\root\\Office16\\OUTLOOK.EXE", L"OUTLOOK.EXE", office},
    {"microsoft-edge", WindowsRoot::ProgramFilesX86, L"Microsoft\\Edge\\Application\\msedge.exe", L"msedge.exe", edge},
    {"microsoft-store", WindowsRoot::PackageIdentity, L"Microsoft.WindowsStore_8wekyb3d8bbwe!App", L"WinStore.App.exe", store},
    {"microsoft-copilot", WindowsRoot::PackageIdentity, L"Microsoft.MicrosoftOfficeHub_8wekyb3d8bbwe!Microsoft.MicrosoftOfficeHub", L"copilotapp_proxy.exe", copilot},
    {"github-copilot", WindowsRoot::LocalAppData, L"Programs\\GitHub Copilot\\github.exe", L"github.exe", github_copilot},
    {"zed", WindowsRoot::LocalAppData, L"Programs\\Zed\\Zed.exe", L"Zed.exe", zed},
    {"figma", WindowsRoot::LocalAppData, L"Figma\\Figma.exe", L"Figma.exe", figma, WindowsLayout::SquirrelVersioned},
    {"steam", WindowsRoot::ProgramFilesX86, L"Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe", L"steamwebhelper.exe", steam},
    {"file-explorer", WindowsRoot::Windows, L"explorer.exe", L"explorer.exe", explorer},
    {"microsoft-solitaire", WindowsRoot::PackageIdentity, L"Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe!App", L"Solitaire.exe", provisioned},
    {"microsoft-photos", WindowsRoot::PackageIdentity, L"Microsoft.Windows.Photos_8wekyb3d8bbwe!App", L"Photos.exe", provisioned},
    {"microsoft-photos", WindowsRoot::PackageIdentity, L"Microsoft.OneDrive.Photos_8wekyb3d8bbwe!OneDrive.App", L"OneDrive.App.exe", measured_package},
    {"microsoft-to-do", WindowsRoot::PackageIdentity, L"Microsoft.Todos_8wekyb3d8bbwe!App", L"Todo.exe", measured_package},
    {"clipchamp", WindowsRoot::PackageIdentity, L"Clipchamp.Clipchamp_yxz26nhyzhsrt!App", L"Clipchamp.exe", measured_package},
    {"xbox", WindowsRoot::PackageIdentity, L"Microsoft.GamingApp_8wekyb3d8bbwe!Microsoft.Xbox.App", L"XboxPcAppCE.exe", measured_package},
    {"windows-calculator", WindowsRoot::PackageIdentity, L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"CalculatorApp.exe", calculator},
    {"windows-app", WindowsRoot::PackageIdentity, L"MicrosoftCorporationII.Windows365_8wekyb3d8bbwe!Windows365", L"Windows365.exe", windows_app},
    {"msn-weather", WindowsRoot::PackageIdentity, L"Microsoft.BingWeather_8wekyb3d8bbwe!App", L"Microsoft.Msn.Weather.exe", weather},
    {"windows-terminal", WindowsRoot::PackageIdentity, L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", L"WindowsTerminal.exe", windows_terminal},
    {"notepad", WindowsRoot::PackageIdentity, L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App", L"Notepad.exe", measured_owner},
    {"windows-settings", WindowsRoot::PackageIdentity, L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", L"SystemSettings.exe", measured_owner},
    {"task-manager", WindowsRoot::Windows, L"System32\\Taskmgr.exe", L"Taskmgr.exe", measured_owner},
    {"visual-studio", WindowsRoot::ProgramFiles, L"Microsoft Visual Studio\\18\\Enterprise\\Common7\\IDE\\devenv.exe", L"devenv.exe", visual_studio},
    {"visual-studio", WindowsRoot::ProgramFiles, L"Microsoft Visual Studio\\18\\Professional\\Common7\\IDE\\devenv.exe", L"devenv.exe", visual_studio},
    {"visual-studio", WindowsRoot::ProgramFiles, L"Microsoft Visual Studio\\18\\Community\\Common7\\IDE\\devenv.exe", L"devenv.exe", visual_studio},
    {"visual-studio", WindowsRoot::ProgramFiles, L"Microsoft Visual Studio\\2022\\Enterprise\\Common7\\IDE\\devenv.exe", L"devenv.exe", visual_studio},
    {"visual-studio", WindowsRoot::ProgramFiles, L"Microsoft Visual Studio\\2022\\Professional\\Common7\\IDE\\devenv.exe", L"devenv.exe", visual_studio},
    {"visual-studio", WindowsRoot::ProgramFiles, L"Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\devenv.exe", L"devenv.exe", visual_studio},
});

constexpr auto coverage = std::to_array<ThemeCoverage>({
    {"finder", WindowsCoverage::ManualOnly, "No Finder-to-Explorer substitution."},
    {"microsoft-teams", WindowsCoverage::Mapped, "New Teams exact Microsoft package AUMID; classic documented per-user path only. No bootstrapper/helper mapping."},
    {"claude", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"codex", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"spotify", WindowsCoverage::Unresolved, "Spotify.exe alone is insufficient; traditional path and Store publisher/AUMID rule not verified."},
    {"notion", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"whatsapp", WindowsCoverage::Unresolved, "Windows package/AUMID rule not verified."},
    {"figma", WindowsCoverage::Mapped, "Per-user Squirrel install (%LOCALAPPDATA%\\Figma\\app-<version>\\Figma.exe) measured locally; machine-wide/custom installs unmapped. No publisher-attestation claim."},
    {"google-chrome", WindowsCoverage::Mapped, "Stable default paths only; beta/dev/canary/Chromium/portable unresolved."},
    {"paper", WindowsCoverage::ManualOnly, "No automatic Windows counterpart assigned."},
    {"chatgpt", WindowsCoverage::Unresolved, "Windows package/AUMID rule not verified."},
    {"grok-bot", WindowsCoverage::Unresolved, "No verified Windows counterpart."},
    {"discord", WindowsCoverage::Unresolved, "Versioned Windows executable path rule not verified."},
    {"granola", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"ghostty", WindowsCoverage::ManualOnly, "No automatic Windows terminal substitution."},
    {"safari", WindowsCoverage::ManualOnly, "No current Windows Safari counterpart assigned."},
    {"firefox", WindowsCoverage::Mapped, "Official default Program Files locations only; portable/custom/Store/developer installs unresolved."},
    {"slack", WindowsCoverage::Unresolved, "Slack.exe alone is insufficient. Vendor documents com.tinyspeck.slackdesktop_8yrtsj140pw4g but relative application ID remains unverified: https://slack.com/help/articles/212475728-Deploy-Slack-for-Windows"},
    {"zoom", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"telegram", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"messages", WindowsCoverage::ManualOnly, "No Apple Messages-to-Windows substitution."},
    {"mail", WindowsCoverage::ManualOnly, "No Apple Mail-to-Windows substitution."},
    {"notes", WindowsCoverage::ManualOnly, "No Apple Notes-to-Windows substitution."},
    {"calendar", WindowsCoverage::ManualOnly, "No Apple Calendar-to-Windows substitution."},
    {"reminders", WindowsCoverage::ManualOnly, "No Apple Reminders-to-Windows substitution."},
    {"apple-music", WindowsCoverage::Unresolved, "Windows Apple Music package/AUMID rule not verified."},
    {"photos", WindowsCoverage::ManualOnly, "No Apple Photos-to-Windows substitution."},
    {"preview", WindowsCoverage::ManualOnly, "No Apple Preview-to-Windows substitution."},
    {"microsoft-word", WindowsCoverage::Mapped, "Documented Office16 default paths only."},
    {"microsoft-excel", WindowsCoverage::Mapped, "Documented Office16 default paths only."},
    {"microsoft-powerpoint", WindowsCoverage::Mapped, "Documented Office16 default paths only."},
    {"microsoft-outlook", WindowsCoverage::Mapped, "Classic OUTLOOK.EXE only. New olk.exe package/AUMID unresolved."},
    {"vs-code", WindowsCoverage::Mapped, "Stable User/System default paths only; Insiders/Code-OSS/portable/custom installs unresolved."},
    {"adobe-photoshop", WindowsCoverage::Unresolved, "Versioned Windows installation/publisher rule not verified."},
    {"adobe-illustrator", WindowsCoverage::Unresolved, "Versioned Windows installation/publisher rule not verified."},
    {"terminal", WindowsCoverage::ManualOnly, "No Apple Terminal-to-cmd/PowerShell/Windows Terminal substitution."},
    {"cursor", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"microsoft-edge", WindowsCoverage::Mapped, "Stable default Program Files (x86) path only; Beta/Dev/Canary, per-user and Store-stub AUMID unresolved."},
    {"microsoft-store", WindowsCoverage::Mapped, "Exact Store package AUMID (Application Id App) only; no StoreDesktopExtension/StoreCLI mapping."},
    {"microsoft-copilot", WindowsCoverage::Mapped, "Documented Copilot app AUMID (formerly the Microsoft 365 app) only; consumer Microsoft.Copilot Application Id is undocumented."},
    {"github-copilot", WindowsCoverage::Mapped, "Exact per-user standalone process path measured locally; other installs, Code.exe, Windows Terminal and the CLI remain unmapped. No publisher-attestation claim."},
    {"zed", WindowsCoverage::Mapped, "Stable per-user default path only; Preview/Nightly/dev/custom-directory installs unresolved."},
    {"file-explorer", WindowsCoverage::Mapped, "Exact %windir%\\explorer.exe only; captioned Explorer dialogs share the theme; taskbar/desktop stay excluded by tracking filters."},
    {"microsoft-solitaire", WindowsCoverage::Mapped, "Documented package family; Application Id App measured locally."},
    {"pale-blue-dot", WindowsCoverage::Unresolved, "Only a sideloaded side-by-side package is known; no published package family or install path."},
    {"get-started", WindowsCoverage::Unresolved, "Current builds host Get Started in the shared MicrosoftWindows.Client.CBS WebExperienceHost, not a unique package."},
    {"steam", WindowsCoverage::Mapped, "Exact default Program Files (x86) steamwebhelper.exe path measured locally as the main-window owner; custom library/install roots unmapped. No publisher-attestation claim."},
    {"clipchamp", WindowsCoverage::Mapped, "Exact package AUMID measured from the installed manifest; no official document names it."},
    {"microsoft-to-do", WindowsCoverage::Mapped, "Exact package AUMID measured from the installed manifest; no official document names it."},
    {"microsoft-photos", WindowsCoverage::Mapped, "Documented Windows Photos AUMID plus the measured OneDrive Photos AUMID (current Start menu Photos)."},
    {"windows-calculator", WindowsCoverage::Mapped, "Exact inbox/Store package AUMID only; Calculator.Dev and other builds unmapped."},
    {"xbox", WindowsCoverage::Mapped, "Main Xbox app AUMID measured from the installed manifest; Game Bar widgets and Microsoft.Xbox.AppL unmapped."},
    {"windows-app", WindowsCoverage::Mapped, "Windows App shell (Application Id Windows365) only; ConnectionShell and remote session windows unmapped."},
    {"msn-weather", WindowsCoverage::Mapped, "Exact documented package AUMID only."},
    {"windows-terminal", WindowsCoverage::Mapped, "Stable packaged Terminal only; Preview, Canary and unpackaged builds unmapped."},
    {"notepad", WindowsCoverage::Mapped, "Packaged Windows 11 Notepad AUMID only; legacy System32\\notepad.exe unmapped."},
    {"windows-settings", WindowsCoverage::Mapped, "Exact Settings package AUMID only; Control Panel and settings dialogs hosted by other processes unmapped."},
    {"task-manager", WindowsCoverage::Mapped, "Exact %windir%\\System32\\Taskmgr.exe only; an elevated Task Manager may refuse owner metadata."},
    {"visual-studio", WindowsCoverage::Mapped, "Default 2022 and 2026 (18) Community, Professional and Enterprise paths only; Preview, Build Tools and custom paths unmapped."},
});

std::wstring_view RootPath(WindowsRoot root, const WindowsInstallRoots& roots) noexcept {
    switch (root) {
    case WindowsRoot::ProgramFiles: return roots.program_files;
    case WindowsRoot::ProgramFilesX86: return roots.program_files_x86;
    case WindowsRoot::LocalAppData: return roots.local_app_data;
    case WindowsRoot::Windows: return roots.windows;
    default: return {};
    }
}

bool PathEquals(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() && left.size() <= 32767 &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                             right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool SquirrelPathEquals(std::wstring_view relative, std::wstring_view identity) noexcept {
    constexpr std::wstring_view app = L"\\app-";
    const auto split = identity.rfind(L'\\');
    if (split == std::wstring_view::npos) return false;
    const auto directory = identity.substr(0, split), executable = identity.substr(split + 1);
    if (relative.size() < directory.size() + app.size() + 2 + executable.size()) return false;
    const auto version = relative.substr(directory.size() + app.size(),
        relative.size() - directory.size() - app.size() - 1 - executable.size());
    return PathEquals(relative.substr(0, directory.size()), directory)
        && PathEquals(relative.substr(directory.size(), app.size()), app)
        && relative[relative.size() - executable.size() - 1] == L'\\'
        && PathEquals(relative.substr(relative.size() - executable.size()), executable)
        && version.front() >= L'0' && version.front() <= L'9'
        && version.find_first_not_of(L"0123456789.") == std::wstring_view::npos;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    const auto result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value);
    std::wstring path;
    if (SUCCEEDED(result) && value) path = value;
    CoTaskMemFree(value);
    return path;
}
}

WindowsInstallRoots QueryWindowsInstallRoots() {
    return {KnownFolder(FOLDERID_ProgramFiles), KnownFolder(FOLDERID_ProgramFilesX86),
            KnownFolder(FOLDERID_LocalAppData), KnownFolder(FOLDERID_Windows)};
}

std::span<const WindowsMapping> WindowsMappings() noexcept { return mappings; }
std::span<const ThemeCoverage> WindowsThemeCoverage() noexcept { return coverage; }

const WindowsMapping* MatchWindowsApplication(
    const applications::AppIdentity& application, const WindowsInstallRoots& roots) noexcept {
    const std::wstring_view key = application.Key();
    for (const auto& mapping : mappings) {
        if (mapping.root == WindowsRoot::PackageIdentity) {
            if (application.Kind() == applications::IdentityKind::PackagedAumid && key == mapping.identity)
                return &mapping;
            continue;
        }
        if (application.Kind() != applications::IdentityKind::NormalizedExecutablePath) continue;
        auto root = RootPath(mapping.root, roots);
        if (root.ends_with(L'\\')) root.remove_suffix(1);
        if (root.empty() || key.size() <= root.size() + 1 || key[root.size()] != L'\\' ||
            !PathEquals(key.substr(0, root.size()), root)) continue;
        const auto relative = key.substr(root.size() + 1);
        if (mapping.layout == WindowsLayout::SquirrelVersioned ? SquirrelPathEquals(relative, mapping.identity)
                                                               : PathEquals(relative, mapping.identity))
            return &mapping;
    }
    return nullptr;
}
}
