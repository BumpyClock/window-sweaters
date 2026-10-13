// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "KnitPreparation.h"
#include <d3d11.h>

namespace window_sweaters::rendering {
class GpuSnapshot final {
public:
    explicit GpuSnapshot(D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE);
    ~GpuSnapshot();
    GpuSnapshot(const GpuSnapshot&) = delete;
    GpuSnapshot& operator=(const GpuSnapshot&) = delete;
    knitting::RasterImage Render(const KnitPerimeter& perimeter);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
