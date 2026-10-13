// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitPreparation.h"
#include <d2d1.h>
#include <wrl/client.h>

namespace window_sweaters::rendering {
using KnitTileBrush = std::function<Microsoft::WRL::ComPtr<ID2D1BitmapBrush>(int role, const knitting::Tile& tile,
    bool heights, float px, float py, float repeatWidth)>;
void DrawKnitStrip(ID2D1RenderTarget& target, const KnitAssembly& assembly, const knitting::Tile& tile,
    const KnitPiece& piece, int side, bool heights, const KnitTileBrush& brush);
}
