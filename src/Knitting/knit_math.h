/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from Window Sweaters src/knit.c and src/misc/knit.h,
 * a49dff52483dafdbe27f972bd271cd28d37cef1c.
 * Window Sweaters derives from JankyBorders by Felix Kratz; see LICENSE.
 */
#ifndef WS_KNIT_MATH_H
#define WS_KNIT_MATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_knit_gauge {
    float rows, aspect, row_overlap, yarn, bow, jitter;
    float ground, shadow, light, depth, ambient, relief, sheen, tuck;
} ws_knit_gauge;

typedef struct ws_knit_layout {
    /* Plain layout uses 8x8; chart adapters supply their authored repeat counts. */
    int columns, rows, width_dip, height_dip, raster_scale;
    float stitch_width, stitch_height, row_step, yarn_width;
} ws_knit_layout;

typedef struct ws_knit_path_writer {
    void* context;
    void (*move)(void* context, float x, float y);
    void (*quadratic)(void* context, float cx, float cy, float x, float y);
} ws_knit_path_writer;

ws_knit_gauge ws_knit_default_gauge(void);
ws_knit_gauge ws_knit_raised_material(ws_knit_gauge gauge);
int ws_knit_validate_gauge(const ws_knit_gauge* gauge);
/* band is logical DIP; raster_scale=0 selects the upstream 4x/2x rule. */
int ws_knit_make_layout(const ws_knit_gauge* gauge, float band,
                       int raster_scale, ws_knit_layout* layout);
uint32_t ws_knit_mix(uint32_t value);
float ws_knit_noise(int column, int row);
/* Logical +x follows an edge; +y goes inward. Each path has a two-cell margin. */
int ws_knit_emit_stockinette(const ws_knit_gauge* gauge, const ws_knit_layout* layout,
                            int raised, ws_knit_path_writer writer);
/* Real raised-yarn cell primitive for later per-colour chart path grouping. */
int ws_knit_emit_cell(const ws_knit_gauge* gauge, const ws_knit_layout* layout,
                     int column, int row, ws_knit_path_writer writer);
/* Height and colour buffers share the upstream bottom-up bitmap row order.
 * Native adapters convert both before shading and output afterwards.
 * Colours are little-endian BGRA, opaque. Blur and normal lookups wrap. */
int ws_knit_blur(float* height, int width, int rows, float* scratch);
int ws_knit_shade(const ws_knit_gauge* gauge, int raised, int raster_scale,
                  const float* height, const uint32_t* flat_bgra,
                  int width, int rows, uint32_t* output_bgra);
extern const float ws_knit_ridge_widths[5];
extern const float ws_knit_ridge_heights[5];

#ifdef __cplusplus
}
#endif
#endif
