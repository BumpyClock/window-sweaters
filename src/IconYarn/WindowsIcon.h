#pragma once

#include "Preparation.h"
#include <windows.h>

namespace window_sweaters::icon_yarn {

// Owned HICON only; input capped at 256px; output 48x48 premultiplied sRGB RGBA.
PixelResult PixelsFromOwnedIcon(HICON icon);

// Trusted full local executable path; PE resources read as data, never executed; reparse points rejected.
PaletteResult SampleExecutableIcon(const IconInput& input, std::stop_token stop,
                                  std::chrono::steady_clock::time_point deadline);

std::uint64_t ExecutableBytesRead() noexcept;

}
