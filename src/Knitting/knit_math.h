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
/* stitch: 0 stockinette, 1 rib, 2 garter, matching the authored source modes. */
int ws_knit_emit_plain(const ws_knit_gauge* gauge, const ws_knit_layout* layout,
                      int stitch, ws_knit_path_writer writer);
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
/* Same inputs as ws_knit_shade, but bakes only occlusion and fibre. The light
 * is applied per screen pixel by ws_knit_shade_ring_relief. */
int ws_knit_albedo(const ws_knit_gauge* gauge, int raised, int raster_scale,
                   const float* height, const uint32_t* flat_bgra,
                   int width, int rows, uint32_t* output_bgra);
/* Rounded knit ring in logical DIP: outer rectangle at the origin, hole edge
 * `inset` inward (inset <= 0: no hole). Both contours share corner centres. */
typedef struct ws_knit_ring {
    float width, height, outer_radius, inset, inner_radius;
} ws_knit_ring;
/* Lights the ring as a padded tube under one screen-space light, so every
 * side and corner agrees. The overlay sits below its target window, so the
 * inner flank darkens where yarn tucks under it. Pixel centre (x+.5, y+.5)/scale
 * + origin is ring DIP. Shades premultiplied BGRA in place; transparent pixels
 * stay unchanged. */
int ws_knit_shade_ring(const ws_knit_ring* ring, float origin_x, float origin_y,
                       float scale, uint32_t* pbgra, int width, int rows);
/* As above, plus stitch relief: `height_bgra` is a premultiplied grey stitch height
 * (blue channel, 0..1) sampled through the same transforms as the colour, so its
 * pixel grid is screen space. Its gradient, scaled by relief * scale, tilts the
 * tube normal before the same light is applied. NULL height equals the call above. */
int ws_knit_shade_ring_relief(const ws_knit_ring* ring, float origin_x, float origin_y,
                              float scale, uint32_t* pbgra, int width, int rows,
                              const uint32_t* height_bgra, float relief);
/* Erodes up to `depth` DIP of the outer silhouette into stitch scallops of
 * `pitch` DIP (aligned to `phase` along the side's local axis) plus loose
 * fibre, so the edge reads as yarn rather than a cut. Run after lighting. */
float ws_knit_fray_depth(float band);
int ws_knit_fray_ring(const ws_knit_ring* ring, int side, float depth, float pitch, float phase,
                      float origin_x, float origin_y, float scale, uint32_t* pbgra, int width, int rows);
/* Soft black drop shadow the ring (a tube `band` DIP wide) casts down/right onto
 * the desktop, reaching at most band/2 beyond the outer contour. Composited under
 * existing premultiplied yarn, only for pixels whose nearest outer edge is `side`
 * (0 top, 1 right, 2 bottom, 3 left) and never inside the inner half of the band.
 * Run last, after lighting and fray. Coordinates as ws_knit_shade_ring. */
int ws_knit_ring_drop_shadow(const ws_knit_ring* ring, int side, float band,
                             float origin_x, float origin_y, float scale,
                             uint32_t* pbgra, int width, int rows);
extern const float ws_knit_ridge_widths[5];
extern const float ws_knit_ridge_heights[5];

#ifdef __cplusplus
}
#endif
#endif
