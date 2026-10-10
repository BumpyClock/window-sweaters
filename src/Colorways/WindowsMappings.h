// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "..\Applications\AppIdentity.h"

#include <span>
#include <string>
#include <string_view>

namespace window_sweaters::colorways {

enum class WindowsRoot { PackageIdentity, ProgramFiles, ProgramFilesX86, LocalAppData };
struct WindowsInstallRoots {
    std::wstring program_files;
    std::wstring program_files_x86;
    std::wstring local_app_data;
};
// OS Known Folder queries only; cache outside the resolver. Failed roots remain empty.
WindowsInstallRoots QueryWindowsInstallRoots();

struct WindowsMapping {
    std::string_view theme_id;
    WindowsRoot root;
    // Exact package AUMID, or exact path relative to a trusted OS Known Folder.
    std::wstring_view identity;
    std::wstring_view executable;
    std::string_view provenance;
};
std::span<const WindowsMapping> WindowsMappings() noexcept;
// Matches only process-derived AppIdentity and exact documented installation locations.
// No basename, display-name, substring, shell AUMID, publisher-name or PID matching.
// Path recognition is not Authenticode attestation; custom/portable installs are unresolved.
const WindowsMapping* MatchWindowsApplication(
    const applications::AppIdentity& application, const WindowsInstallRoots& roots) noexcept;

enum class WindowsCoverage { Mapped, Unresolved, ManualOnly };
struct ThemeCoverage {
    std::string_view theme_id;
    WindowsCoverage coverage;
    std::string_view limitation;
};
// All 37 original themes remain manually selectable, independent of this coverage table.
std::span<const ThemeCoverage> WindowsThemeCoverage() noexcept;

} // namespace window_sweaters::colorways
