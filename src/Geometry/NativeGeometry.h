// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "Geometry.h"

namespace window_sweaters::geometry {

// Read-only, one bounded query. Requires a PMv2 caller and per-monitor-aware target.
// Acquires/revalidates corner eligibility facts; does not measure a pixel outline.
// The coordinator remains responsible for target lifetime/generation and reconciliation.
WindowQuery QueryWindowGeometry(HWND window) noexcept;

} // namespace window_sweaters::geometry
