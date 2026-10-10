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

int ws_knit_shade(const ws_knit_gauge* g, int raised, int scale,
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
            float ndl = (nx*lx + ny*ly + lz) * inv;
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
