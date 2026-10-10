#pragma once

#include "Preparation.h"
#include <windows.h>

namespace window_sweaters::icon_yarn {

// Only HICONs owned by this process. No HWND lookup, messages, screen/DC capture or app contents.
// Input bitmap dimensions are capped at 256; output is exactly 48x48 premultiplied sRGB RGBA.
PixelResult PixelsFromOwnedIcon(HICON icon);

// Query-compatible native boundary: trusted full executable path from host-owned metadata.
// Rejects network/device/relative paths and reparse-point files; caps input to 64MiB,
// resource directories to 1MiB and individual icon images to 256KiB/256x256.
// Reads PE resources as data; never loads/executes an executable or invokes shell handlers.
// Reads only bounded ranges (headers, section table, resource directory, selected icon data).
PaletteResult SampleExecutableIcon(const IconInput& input, std::stop_token stop,
                                  std::chrono::steady_clock::time_point deadline);

// Bytes read from disk by the last SampleExecutableIcon on this thread (test seam).
std::uint64_t ExecutableBytesRead() noexcept;

} // namespace window_sweaters::icon_yarn
