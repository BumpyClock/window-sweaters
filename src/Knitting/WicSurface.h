// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitTile.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace window_sweaters::knitting::detail {
using Microsoft::WRL::ComPtr;
void Check(HRESULT result, const char* operation);

class WicSurface {
public:
    WicSurface(std::uint32_t width, std::uint32_t height, float scale, ID2D1Factory* sharedFactory = nullptr);
    RasterImage Read() const;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
private:
    std::uint32_t width_, height_;
};

D2D1_COLOR_F OpaqueColor(std::uint32_t argb, float multiplier = 1.f);
struct YarnCell { int column, row; std::uint32_t argb; };
RasterImage RasterizeTile(const TileSpec& spec, const ws_knit_layout& layout,
                          const std::vector<YarnCell>& chartCells = {},
                          RasterImage* diagnosticHeight = nullptr);
}
