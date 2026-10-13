// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ws_knit_ring {
    float width, height, outer_radius, inset, inner_radius;
} ws_knit_ring;
/* Pixel centre (x+.5, y+.5)/scale + origin is ring DIP; shades premultiplied BGRA in place. */
int ws_knit_shade_ring(const ws_knit_ring* ring, float origin_x, float origin_y,
                       float scale, uint32_t* pbgra, int width, int rows);
int ws_knit_shade_ring_relief(const ws_knit_ring* ring, float origin_x, float origin_y,
                              float scale, uint32_t* pbgra, int width, int rows,
                              const uint32_t* height_bgra, float relief);
float ws_knit_fray_depth(float band);
int ws_knit_fray_ring(const ws_knit_ring* ring, int side, float depth, float pitch, float phase,
                      float origin_x, float origin_y, float scale, uint32_t* pbgra, int width, int rows);
/* side: 0 top, 1 right, 2 bottom, 3 left. */
int ws_knit_ring_drop_shadow(const ws_knit_ring* ring, int side, float band,
                             float origin_x, float origin_y, float scale,
                             uint32_t* pbgra, int width, int rows);
#ifdef __cplusplus
}
#endif
