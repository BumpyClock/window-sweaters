/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from Window Sweaters src/knit.c,
 * a49dff52483dafdbe27f972bd271cd28d37cef1c.
 * Window Sweaters derives from JankyBorders by Felix Kratz; see LICENSE.
 */
#include "knit_math.h"
#include <math.h>

const float ws_knit_ridge_widths[5] = {1.00f, 0.80f, 0.60f, 0.40f, 0.20f};
const float ws_knit_ridge_heights[5] = {0.22f, 0.48f, 0.70f, 0.88f, 1.00f};

ws_knit_gauge ws_knit_default_gauge(void) {
    ws_knit_gauge g = {
        .rows = 6.f, .aspect = 1.35f, .row_overlap = 0.12f,
        .yarn = 0.48f, .bow = 0.28f, .jitter = 0.035f,
        .ground = 0.98f, .shadow = 0.74f, .light = 1.18f, .depth = 1.f,
        .ambient = 0.94f, .relief = 0.70f, .sheen = 0.10f, .tuck = 14.f
    };
    return g;
}

ws_knit_gauge ws_knit_raised_material(ws_knit_gauge g) {
    g.row_overlap = 0.05f; g.yarn = 0.40f;
    g.ground = 0.94f; g.ambient = 0.84f;
    g.relief = 1.65f; g.sheen = 0.28f;
    return g;
}

int ws_knit_validate_gauge(const ws_knit_gauge* g) {
    if (!g) return 0;
    const float fields[] = {g->rows, g->aspect, g->row_overlap, g->yarn, g->bow,
        g->jitter, g->ground, g->shadow, g->light, g->ambient, g->depth,
        g->relief, g->sheen, g->tuck};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i)
        if (!isfinite(fields[i])) return 0;
    return g->rows > 0.f && g->rows <= 128.f && g->aspect > 0.f && g->aspect <= 8.f
        && g->row_overlap >= 0.f && g->row_overlap < 0.95f
        && g->yarn > 0.f && g->yarn <= 2.f && g->bow >= 0.f && g->bow <= 1.f
        && g->jitter >= 0.f && g->jitter <= 1.f
        && g->ground >= 0.f && g->ground <= 2.f
        && g->ambient >= 0.f && g->ambient <= 2.f
        && g->relief >= 0.f && g->relief <= 16.f
        && g->sheen >= 0.f && g->sheen <= 2.f;
}

int ws_knit_make_layout(const ws_knit_gauge* g, float band, int scale,
                       ws_knit_layout* out) {
    if (!out || !ws_knit_validate_gauge(g) || !isfinite(band) || band < 2.f
        || band > 256.f || scale < 0 || scale > 8) return 0;
    float step = band / fmaxf(1.5f, g->rows), sw = step * g->aspect;
    int tw = (int)lroundf(sw * 8), th = (int)lroundf(step * 8);
    if (tw < 8) tw = 8;
    if (th < 8) th = 8;
    sw = (float)tw / 8;
    step = (float)th / 8;
    if (!scale) scale = step < 4.f ? 4 : 2;
    if (tw > 4096 / scale || th > 4096 / scale) return 0;
    *out = (ws_knit_layout){8, 8, tw, th, scale, sw,
                           step / (1.f - g->row_overlap), step,
                           fmaxf(sw * g->yarn, 0.35f)};
    return 1;
}

uint32_t ws_knit_mix(uint32_t h) {
    h ^= h >> 16; h *= 0x85ebca6b;
    h ^= h >> 13; h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

float ws_knit_noise(int a, int b) {
    float s = sinf(a * 12.9898f + b * 78.233f) * 43758.5453f;
    return s - floorf(s);
}

static int valid_path(const ws_knit_gauge* g, const ws_knit_layout* l,
                      ws_knit_path_writer w) {
    return ws_knit_validate_gauge(g) && l && l->columns > 0 && l->columns <= 4096
        && l->rows > 0 && l->rows <= 4096
        && isfinite(l->stitch_width) && l->stitch_width > 0.f
        && isfinite(l->stitch_height) && l->stitch_height > 0.f
        && isfinite(l->row_step) && l->row_step > 0.f && w.move && w.quadratic;
}

int ws_knit_emit_cell(const ws_knit_gauge* g, const ws_knit_layout* l,
                     int i, int j, ws_knit_path_writer w) {
    if (!valid_path(g, l, w) || i < -2 || i > l->columns + 1
        || j < -2 || j > l->rows + 1) return 0;
    float sw = l->stitch_width, sh = l->stitch_height;
    float bx = sw * 0.5f * g->bow, x = i * sw, y = j * l->row_step;
    w.move(w.context, x, y);
    w.quadratic(w.context, x + bx, y + sh * 0.55f, x + sw * 0.5f, y + sh);
    w.quadratic(w.context, x + sw - bx, y + sh * 0.55f, x + sw, y);
    return 1;
}

int ws_knit_emit_stockinette(const ws_knit_gauge* g, const ws_knit_layout* l,
                            int raised, ws_knit_path_writer w) {
    if (!valid_path(g, l, w)) return 0;
    float sw = l->stitch_width, sh = l->stitch_height;
    float bx = sw * 0.5f * g->bow, jit = sw * g->jitter;
    for (int j = -2; j <= l->rows + 1; ++j) {
        if (raised) {
            for (int i = -2; i <= l->columns + 1; ++i)
                ws_knit_emit_cell(g, l, i, j, w);
            continue;
        }
        float y = j * l->row_step;
        float jy = (ws_knit_noise((j + l->rows) % l->rows, 3) - 0.5f) * jit * 2.f;
        w.move(w.context, -2 * sw, y + jy);
        for (int i = -2; i <= l->columns + 1; ++i) {
            float x = i * sw;
            float jx = (ws_knit_noise((i + l->columns) % l->columns,
                                     (j + l->rows) % l->rows) - 0.5f) * jit * 2.f;
            w.quadratic(w.context, x + bx + jx, y + sh * 0.55f + jy,
                        x + sw * 0.5f + jx, y + sh + jy);
            w.quadratic(w.context, x + sw - bx + jx, y + sh * 0.55f + jy,
                        x + sw + jx, y + jy);
        }
    }
    return 1;
}

int ws_knit_emit_plain(const ws_knit_gauge* g, const ws_knit_layout* l,
                      int stitch, ws_knit_path_writer w) {
    if (!valid_path(g, l, w) || stitch < 0 || stitch > 2) return 0;
    if (!stitch) return ws_knit_emit_stockinette(g, l, 0, w);
    float sw = l->stitch_width, sh = l->stitch_height;
    if (stitch == 1) {
        float h = (l->rows + 4) * l->row_step;
        for (int i = -2; i <= l->columns + 1; ++i) {
            float x = i * sw + sw * 0.5f;
            float purl = (i & 1) ? sw * 0.16f : 0.f;
            float start = -2 * l->row_step;
            w.move(w.context, x + purl, start);
            /* A collinear quadratic is the same straight rib as CGPath's line. */
            w.quadratic(w.context, x + purl, (start + h) * 0.5f, x + purl, h);
        }
        return 1;
    }
    for (int j = -2; j <= l->rows + 1; ++j) {
        float y = j * l->row_step;
        float phase = (j & 1) ? sw * 0.5f : 0.f;
        w.move(w.context, -2 * sw, y);
        for (int i = -2; i <= l->columns + 1; ++i) {
            float x = i * sw + phase;
            w.quadratic(w.context, x + sw * 0.25f, y - sh * 0.30f,
                        x + sw * 0.5f, y);
            w.quadratic(w.context, x + sw * 0.75f, y + sh * 0.30f, x + sw, y);
        }
    }
    return 1;
}

int ws_knit_blur(float* a, int w, int h, float* tmp) {
    if (!a || !tmp || a == tmp || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int l = (x - 1 + w) % w, r = (x + 1) % w;
            tmp[y*w + x] = (a[y*w + l] + 2.f*a[y*w + x] + a[y*w + r]) * 0.25f;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int u = (y - 1 + h) % h, d = (y + 1) % h;
            a[y*w + x] = (tmp[u*w + x] + 2.f*tmp[y*w + x] + tmp[d*w + x]) * 0.25f;
        }
    return 1;
}

/* lit=0 bakes albedo only: occlusion and fibre, with the light direction left to
 * the screen-space pass (flat normal, so no stitch faces a tile-space light). */
static int shade_tile(const ws_knit_gauge* g, int raised, int scale, int lit,
                      const float* hf, const uint32_t* flat, int w, int h, uint32_t* out) {
    if (!ws_knit_validate_gauge(g) || !hf || !flat || !out || scale <= 0 || scale > 8
        || w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    for (int i = 0; i < w*h; ++i)
        if (!isfinite(hf[i]) || hf[i] < 0.f || hf[i] > 1.f) return 0;
    const float lx = -0.45f, ly = -0.55f, lz = 0.70f;
    float relief = scale * g->relief;
    for (int y = 0; y < h; ++y) {
        int up = (y - 1 + h) % h, dn = (y + 1) % h;
        for (int x = 0; x < w; ++x) {
            int lft = (x - 1 + w) % w, rgt = (x + 1) % w;
            float dzdx = (hf[y*w + rgt] - hf[y*w + lft]) * relief;
            float dzdy = (hf[dn*w + x] - hf[up*w + x]) * relief;
            float nx = -dzdx, ny = -dzdy;
            float inv = 1.f / sqrtf(nx*nx + ny*ny + 1.f);
            float ndl = lit ? (nx*lx + ny*ly + lz) * inv : lz;
            if (ndl < 0.f) ndl = 0.f;
            float occ = raised ? 0.74f + 0.26f * hf[y*w + x] : 0.86f + 0.14f * hf[y*w + x];
            uint32_t grain = ws_knit_mix((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u);
            float fibre = raised ? 1.f : 0.993f + (grain & 255u) * (0.014f / 255.f);
            float shade = (g->ambient + g->sheen * ndl) * occ * fibre;
            uint32_t p = flat[y*w + x];
            float peak = fmaxf((float)(p & 255), fmaxf((float)((p >> 8) & 255), (float)((p >> 16) & 255)));
            float lift = raised ? fmaxf(0.f, 1.f - peak / 48.f) * hf[y*w + x] * 10.f : 0.f;
            float cb = fminf(255.f, (p & 255) * shade + lift);
            float cg = fminf(255.f, ((p >> 8) & 255) * shade + lift);
            float cr = fminf(255.f, ((p >> 16) & 255) * shade + lift);
            out[y*w + x] = 0xff000000u | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb;
        }
    }
    return 1;
}

int ws_knit_shade(const ws_knit_gauge* g, int raised, int scale,
                  const float* hf, const uint32_t* flat, int w, int h, uint32_t* out) {
    return shade_tile(g, raised, scale, 1, hf, flat, w, h, out);
}

int ws_knit_albedo(const ws_knit_gauge* g, int raised, int scale,
                   const float* hf, const uint32_t* flat, int w, int h, uint32_t* out) {
    return shade_tile(g, raised, scale, 0, hf, flat, w, h, out);
}

/* Signed distance to a rounded rectangle at (x0, y0) and its outward normal. */
static float rounded_box(float px, float py, float x0, float y0, float w, float h,
                         float r, float* gx, float* gy) {
    float hx = w * 0.5f, hy = h * 0.5f, cx = px - x0 - hx, cy = py - y0 - hy;
    float sx = cx < 0.f ? -1.f : 1.f, sy = cy < 0.f ? -1.f : 1.f;
    float qx = fabsf(cx) - hx + r, qy = fabsf(cy) - hy + r;
    if (qx > 0.f && qy > 0.f) {
        float len = sqrtf(qx * qx + qy * qy);
        *gx = sx * qx / len; *gy = sy * qy / len;
        return len - r;
    }
    if (qx > qy) { *gx = sx; *gy = 0.f; return qx - r; }
    *gx = 0.f; *gy = sy;
    return qy - r;
}

/* Stitch height at (x, y) from a premultiplied grey map; pixels the map does not
 * cover (alpha 0) or beyond the piece take the centre height, so edges add no slope. */
static float stitch_height(const uint32_t* m, int w, int h, int x, int y, float centre) {
    if (x < 0 || y < 0 || x >= w || y >= h) return centre;
    uint32_t q = m[(size_t)y * w + x], a = q >> 24;
    return a ? (float)(q & 255u) / (float)a : centre;
}

int ws_knit_shade_ring_relief(const ws_knit_ring* k, float ox, float oy, float scale,
                              uint32_t* p, int w, int h, const uint32_t* hm, float relief) {
    if (hm && (!isfinite(relief) || relief < 0.f)) return 0;
    if (!k || !p || w <= 0 || h <= 0 || w > 32768 || h > 32768
        || !isfinite(ox) || !isfinite(oy) || !isfinite(scale) || scale <= 0.f
        || !isfinite(k->width) || !isfinite(k->height) || !isfinite(k->inset)
        || !isfinite(k->outer_radius) || !isfinite(k->inner_radius)
        || k->width <= 0.f || k->height <= 0.f
        || k->outer_radius < 0.f || k->inner_radius < 0.f) return 0;
    /* Frontal key light from the upper left: both tube flanks fall off, the
     * lower/right flanks more, matching a window lit from above. */
    const float lx = -0.26f, ly = -0.42f, lz = 0.87f;
    const float ambient = 0.56f, tilt = 1.05f, crease = 0.22f;
    int hole = k->inset > 0.f && k->width > 2.f * k->inset && k->height > 2.f * k->inset;
    float half = fminf(k->width, k->height) * 0.5f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t px = p[(size_t)y * w + x], a = px >> 24;
            if (!a) continue;
            float fx = ox + (x + 0.5f) / scale, fy = oy + (y + 0.5f) / scale, gx, gy, ix, iy;
            float out = -rounded_box(fx, fy, 0.f, 0.f, k->width, k->height,
                                     k->outer_radius, &gx, &gy);
            float in = hole ? rounded_box(fx, fy, k->inset, k->inset, k->width - 2.f * k->inset,
                                          k->height - 2.f * k->inset, k->inner_radius, &ix, &iy)
                            : half - out;
            out = fmaxf(0.f, out); in = fmaxf(0.f, in);
            /* u: -1 at the outer silhouette, +1 where yarn tucks under the window. */
            float u = 2.f * out / fmaxf(1e-4f, out + in) - 1.f;
            float bend = u < 0.f ? -powf(-u, 1.6f) : powf(u, 1.6f);
            float s = sinf(bend * tilt), c = cosf(bend * tilt);
            /* The outer flank faces away from the hole (+g), the inner one toward it. */
            float slope = 0.f;
            if (hm) {
                /* Stitch slope in piece pixels (already screen space) tilts the tube normal. */
                float h0 = stitch_height(hm, w, h, x, y, 0.f);
                if (hm[(size_t)y * w + x] >> 24) {
                    float k2 = relief * scale * 0.6f;
                    slope += (-lx) * (stitch_height(hm, w, h, x + 1, y, h0) - stitch_height(hm, w, h, x - 1, y, h0)) * k2 / lz;
                    slope += (-ly) * (stitch_height(hm, w, h, x, y + 1, h0) - stitch_height(hm, w, h, x, y - 1, h0)) * k2 / lz;
                }
            }
            float ndl = fmaxf(0.f, (-gx * s * lx - gy * s * ly + c * lz) / lz + slope);
            float e2 = u * u;
            float shade = (ambient + (1.f - ambient) * ndl) * (1.f - crease * e2 * e2);
            uint32_t result = a << 24;
            for (int shift = 0; shift < 24; shift += 8) {
                float v = ((px >> shift) & 255u) * shade;
                result |= (uint32_t)fminf((float)a, v + 0.5f) << shift;
            }
            p[(size_t)y * w + x] = result;
        }
    return 1;
}

int ws_knit_shade_ring(const ws_knit_ring* k, float ox, float oy, float scale,
                       uint32_t* p, int w, int h) {
    return ws_knit_shade_ring_relief(k, ox, oy, scale, p, w, h, NULL, 0.f);
}

float ws_knit_fray_depth(float band) { return band * 0.07f; }

int ws_knit_fray_ring(const ws_knit_ring* k, int side, float depth, float pitch, float phase,
                      float ox, float oy, float scale, uint32_t* p, int w, int h) {
    if (!k || !p || side < 0 || side > 3 || w <= 0 || h <= 0 || w > 32768 || h > 32768
        || !isfinite(depth) || !isfinite(pitch) || !isfinite(phase) || !isfinite(ox)
        || !isfinite(oy) || !isfinite(scale) || scale <= 0.f || depth < 0.f || pitch <= 0.f
        || !isfinite(k->width) || !isfinite(k->height) || !isfinite(k->outer_radius)
        || k->width <= 0.f || k->height <= 0.f || k->outer_radius < 0.f) return 0;
    if (depth * scale < 0.25f) return 1;
    const float tau = 6.28318530718f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t px = p[(size_t)y * w + x];
            if (!(px >> 24)) continue;
            float fx = ox + (x + 0.5f) / scale, fy = oy + (y + 0.5f) / scale, gx, gy;
            float out = -rounded_box(fx, fy, 0.f, 0.f, k->width, k->height, k->outer_radius, &gx, &gy);
            if (out >= depth + 1.f / scale) continue;
            /* Stitches bulge between columns and part at column joins; loose
             * fibre roughens the outermost half. */
            float along = side == 0 ? fx : side == 1 ? fy : side == 2 ? k->width - fx : k->height - fy;
            float bump = 0.5f - 0.5f * cosf(tau * (along - phase) / pitch);
            uint32_t grain = ws_knit_mix((uint32_t)(int)floorf(fx * scale) * 73856093u
                                         ^ (uint32_t)(int)floorf(fy * scale) * 19349663u);
            float fibre = ((grain & 255u) / 255.f - 0.5f) * 0.6f;
            float erode = depth * fminf(1.f, fmaxf(0.f, 0.85f * (1.f - bump) + fibre));
            float c = fminf(1.f, fmaxf(0.f, (out - erode) * scale + 1.f));
            if (c >= 1.f) continue;
            uint32_t result = 0;
            for (int shift = 0; shift < 32; shift += 8)
                result |= (uint32_t)(((px >> shift) & 255u) * c + 0.5f) << shift;
            p[(size_t)y * w + x] = result;
        }
    return 1;
}

int ws_knit_ring_drop_shadow(const ws_knit_ring* k, int side, float band,
                             float ox, float oy, float scale, uint32_t* p, int w, int h) {
    if (!k || !p || side < 0 || side > 3 || w <= 0 || h <= 0 || w > 32768 || h > 32768
        || !isfinite(band) || band <= 0.f || !isfinite(ox) || !isfinite(oy)
        || !isfinite(scale) || scale <= 0.f
        || !isfinite(k->width) || !isfinite(k->height) || !isfinite(k->outer_radius)
        || k->width <= 0.f || k->height <= 0.f || k->outer_radius < 0.f) return 0;
    /* Cast away from the upper-left ring light; the furthest tail (offset plus
     * spread) stays within half a band, the margin the overlay reserves. */
    const float peak = 0.3f, dx = 0.07f * band, dy = 0.12f * band;
    const float core = 0.1f * band, spread = 0.36f * band;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t px = p[(size_t)y * w + x], a = px >> 24;
            if (a == 255u) continue;
            float fx = ox + (x + 0.5f) / scale, fy = oy + (y + 0.5f) / scale, gx, gy;
            /* Mitre partition: the nearest outer edge owns a pixel (ties go to the
             * earlier side), so overlapping piece rectangles never double it. */
            float edge[4] = {fy, k->width - fx, k->height - fy, fx};
            int owner = 0;
            for (int i = 1; i < 4; ++i) if (edge[i] < edge[owner]) owner = i;
            if (owner != side) continue;
            /* Only the outer half of the band and beyond: never the window hole. */
            if (rounded_box(fx, fy, 0.f, 0.f, k->width, k->height, k->outer_radius, &gx, &gy)
                < -0.5f * band) continue;
            float d = rounded_box(fx - dx, fy - dy, 0.f, 0.f, k->width, k->height,
                                  k->outer_radius, &gx, &gy);
            float t = fminf(1.f, fmaxf(0.f, (d + core) / (core + spread)));
            float s = 1.f - t * t * (3.f - 2.f * t);
            /* Black shadow under premultiplied yarn: only alpha grows. */
            uint32_t add = (uint32_t)(peak * s * s * (float)(255u - a) + 0.5f);
            if (add) p[(size_t)y * w + x] = (px & 0x00ffffffu) | ((a + add) << 24);
        }
    return 1;
}
