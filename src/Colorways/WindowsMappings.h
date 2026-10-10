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
// OS Known Folder queries only; cache outside the resolver. Failed roots remain empty.
WindowsInstallRoots QueryWindowsInstallRoots();

// Exact: identity is the full relative path. SquirrelVersioned: identity is <dir>\<exe>, matched as
// <dir>\app-<digits and dots>\<exe> (Squirrel.Windows per-user layout).
enum class WindowsLayout { Exact, SquirrelVersioned };
struct WindowsMapping {
    std::string_view theme_id;
    WindowsRoot root;
    // Exact package AUMID, or exact path relative to a trusted OS Known Folder.
    std::wstring_view identity;
    std::wstring_view executable;
    std::string_view provenance; // Public document URL(s), or a dated local package measurement.
    WindowsLayout layout = WindowsLayout::Exact;
};
std::span<const WindowsMapping> WindowsMappings() noexcept;
// Matches only process-derived AppIdentity and exact documented installation locations,
// plus the Squirrel version segment for SquirrelVersioned mappings.
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
// One entry per catalogue theme; every theme remains manually selectable regardless of coverage.
std::span<const ThemeCoverage> WindowsThemeCoverage() noexcept;

} // namespace window_sweaters::colorways
