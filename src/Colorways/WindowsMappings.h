// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "..\Applications\AppIdentity.h"

#include <span>
#include <string>
#include <string_view>

namespace window_sweaters::colorways {

enum class WindowsRoot { PackageIdentity, ProgramFiles, ProgramFilesX86, LocalAppData, Windows };
struct WindowsInstallRoots {
    std::wstring program_files;
    std::wstring program_files_x86;
    std::wstring local_app_data;
    std::wstring windows;
};
WindowsInstallRoots QueryWindowsInstallRoots();

enum class WindowsLayout { Exact, SquirrelVersioned };
struct WindowsMapping {
    std::string_view theme_id;
    WindowsRoot root;
    std::wstring_view identity;
    std::wstring_view executable;
    std::string_view provenance;
    WindowsLayout layout = WindowsLayout::Exact;
};
std::span<const WindowsMapping> WindowsMappings() noexcept;
// No basename, display-name, substring, shell AUMID, publisher-name or PID matching.
const WindowsMapping* MatchWindowsApplication(
    const applications::AppIdentity& application, const WindowsInstallRoots& roots) noexcept;

enum class WindowsCoverage { Mapped, Unresolved, ManualOnly };
struct ThemeCoverage {
    std::string_view theme_id;
    WindowsCoverage coverage;
    std::string_view limitation;
};
std::span<const ThemeCoverage> WindowsThemeCoverage() noexcept;

}
