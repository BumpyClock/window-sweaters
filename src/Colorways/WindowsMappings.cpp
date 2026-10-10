// SPDX-License-Identifier: GPL-3.0-only
#include "WindowsMappings.h"

#include <Windows.h>
#include <ShlObj.h>

#include <array>

namespace window_sweaters::colorways {
namespace {
// Public documentation/source reviewed 2026-10-09. These facts do not establish
// which applications are installed, nor demonstrate their running windows.
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
});

constexpr auto coverage = std::to_array<ThemeCoverage>({
    {"finder", WindowsCoverage::ManualOnly, "No Finder-to-Explorer substitution."},
    {"microsoft-teams", WindowsCoverage::Mapped, "New Teams exact Microsoft package AUMID; classic documented per-user path only. No bootstrapper/helper mapping."},
    {"claude", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"codex", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"spotify", WindowsCoverage::Unresolved, "Spotify.exe alone is insufficient; traditional path and Store publisher/AUMID rule not verified."},
    {"notion", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
    {"whatsapp", WindowsCoverage::Unresolved, "Windows package/AUMID rule not verified."},
    {"figma", WindowsCoverage::Unresolved, "Windows installation/publisher rule not verified."},
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
});

std::wstring_view RootPath(WindowsRoot root, const WindowsInstallRoots& roots) noexcept {
    switch (root) {
    case WindowsRoot::ProgramFiles: return roots.program_files;
    case WindowsRoot::ProgramFilesX86: return roots.program_files_x86;
    case WindowsRoot::LocalAppData: return roots.local_app_data;
    default: return {};
    }
}

bool PathEquals(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() && left.size() <= 32767 &&
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                             right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    const auto result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value);
    std::wstring path;
    if (SUCCEEDED(result) && value) path = value;
    CoTaskMemFree(value);
    return path;
}
} // namespace

WindowsInstallRoots QueryWindowsInstallRoots() {
    return {KnownFolder(FOLDERID_ProgramFiles), KnownFolder(FOLDERID_ProgramFilesX86),
            KnownFolder(FOLDERID_LocalAppData)};
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
        if (root.empty() || key.size() != root.size() + 1 + mapping.identity.size()) continue;
        if (key[root.size()] == L'\\' && PathEquals(key.substr(0, root.size()), root) &&
            PathEquals(key.substr(root.size() + 1), mapping.identity)) return &mapping;
    }
    return nullptr;
}
} // namespace window_sweaters::colorways
