/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from Window Sweaters src/knit.c,
 * a49dff52483dafdbe27f972bd271cd28d37cef1c.
 * Window Sweaters derives from JankyBorders by Felix Kratz; see LICENSE.
 */
#include "BorderMath.h"
#include "../../src/Knitting/knit_math.h"
#include <math.h>

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
            float u = 2.f * out / fmaxf(1e-4f, out + in) - 1.f;
            float bend = u < 0.f ? -powf(-u, 1.6f) : powf(u, 1.6f);
            float s = sinf(bend * tilt), c = cosf(bend * tilt);
            float slope = 0.f;
            if (hm) {
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
    const float peak = 0.3f, dx = 0.07f * band, dy = 0.12f * band;
    const float core = 0.1f * band, spread = 0.36f * band;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t px = p[(size_t)y * w + x], a = px >> 24;
            if (a == 255u) continue;
            float fx = ox + (x + 0.5f) / scale, fy = oy + (y + 0.5f) / scale, gx, gy;
            float edge[4] = {fy, k->width - fx, k->height - fy, fx};
            int owner = 0;
            for (int i = 1; i < 4; ++i) if (edge[i] < edge[owner]) owner = i;
            if (owner != side) continue;
            if (rounded_box(fx, fy, 0.f, 0.f, k->width, k->height, k->outer_radius, &gx, &gy)
                < -0.5f * band) continue;
            float d = rounded_box(fx - dx, fy - dy, 0.f, 0.f, k->width, k->height,
                                  k->outer_radius, &gx, &gy);
            float t = fminf(1.f, fmaxf(0.f, (d + core) / (core + spread)));
            float s = 1.f - t * t * (3.f - 2.f * t);
            uint32_t add = (uint32_t)(peak * s * s * (float)(255u - a) + 0.5f);
            if (add) p[(size_t)y * w + x] = (px & 0x00ffffffu) | ((a + add) << 24);
        }
    return 1;
}
