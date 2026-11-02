/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/fermi_2d.cpp, src/video_core/engines/fermi_2d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 2D engine (FERMI_TWOD_A, 0x902D): a blit from one surface to another, scaled, started by the last word of the
   source origin (0x237). The renderer does it on the GPU when it can; otherwise it is done here in guest memory,
   nearest sampled */
#include "nx/gpu/maxwell/engines_int.h"

/* a surface's registers from b: format, layout, block size, depth, layer, pitch, width, height, address */
static void f2d_surface(const u32* r, u32 b, Surf2D* s) {
    s->format = r[b];
    s->linear = r[b + 1] & 1;
    s->bh = (r[b + 2] >> 4) & 15;
    s->depth = r[b + 3];
    s->layer = r[b + 4];
    s->pitch = r[b + 5];
    s->width = r[b + 6];
    s->height = r[b + 7];
    s->addr = ((u64)(r[b + 8] & 0xFF) << 32) | r[b + 9];
}
/* a surface's slice as an address and a z: a 3D surface's layer is its z, with the block depth of its block size
   register; a 2D array's layer is a whole number of block rows further */
typedef struct {
    u64 base, span;
    u32 wb, z, bd, bpp;
    u8* p; /* the span's host pointer when it is contiguous */
} F2dPlane;
static void f2d_plane(const Surf2D* s, u32 blk, u32 bpp, F2dPlane* pl, int write) {
    pl->bpp = bpp;
    pl->wb = s->linear ? s->pitch : s->width * bpp;
    pl->base = s->addr;
    pl->z = 0;
    pl->bd = 0;
    if (s->linear) {
        pl->span = (u64)s->pitch * (s->height ? s->height - 1 : 0) + (u64)s->width * bpp;
        pl->p = gmem_ptr(pl->base, pl->span);
    } else {
        if (s->depth > 1) {
            pl->z = s->layer;
            pl->bd = (blk >> 8) & 15;
        } else if (s->layer)
            pl->base += (((u64)pl->wb + 63) & ~(u64)63) * ((((u64)s->height + (8u << s->bh) - 1) >> (3 + s->bh))
                                                            << (3 + s->bh)) * s->layer;
        pl->p = bl_span_ptr(pl->base, pl->wb, s->height, s->bh, pl->z, pl->bd, &pl->span);
    }
    if (!pl->p && !write) gmem_need(pl->base, pl->span); /* the hooks once over the span, then raw runs */
}
/* the byte offset of column x (in pixels) and of row y within a plane */
static u64 f2d_col(const Surf2D* s, const F2dPlane* pl, u32 x) {
    return s->linear ? (u64)x * pl->bpp : bl_offset(x * pl->bpp, 0, 0, pl->wb, s->height, s->bh, pl->bd);
}
static u64 f2d_row(const Surf2D* s, const F2dPlane* pl, u32 y) {
    return s->linear ? (u64)y * s->pitch : bl_offset(0, y, pl->z, pl->wb, s->height, s->bh, pl->bd);
}
/* the blit in guest memory: each destination row from its source row, column offsets computed once, runs that stay
   contiguous on both sides moved as one */
static void f2d_blit_memory(const u32* r, const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx,
                            double sy, double du, double dv) {
    u32 bs = gpu_color_bpp(s->format), bd = gpu_color_bpp(d->format), n, i, y, x0, x1, y0, y1;
    F2dPlane ps, pd;
    u64 *scol, *dcol;
    u8* row;
    if (bs != bd) {
        WARN_LIMIT(4, "gpu: a 2D blit from format %x to %x isn't done\n", s->format, d->format);
        return;
    }
    if (s->format != d->format)
        LOG_LIMIT(4, "gpu: a 2D blit from format %x to %x copies the bytes\n", s->format, d->format);
    if (!s->width || !s->height) return;
    x0 = dx > 0 ? (u32)dx : 0;
    y0 = dy > 0 ? (u32)dy : 0;
    x1 = (i64)dx + dw < (i64)d->width ? (u32)((i64)dx + dw) : d->width;
    y1 = (i64)dy + dh < (i64)d->height ? (u32)((i64)dy + dh) : d->height;
    if (x0 >= x1 || y0 >= y1) return;
    n = x1 - x0;
    scol = (u64*)malloc((size_t)n * (16 + bs));
    if (!scol) {
        WARN_LIMIT(1, "gpu: no memory for a 2D blit %u pixels wide\n", n);
        return;
    }
    dcol = scol + n;
    row = (u8*)(dcol + n);
    if (g_renderer_up && g_renderer->rt_flush) g_renderer->rt_flush(s->addr);
    f2d_plane(s, r[0x8E], bs, &ps, 0);
    f2d_plane(d, r[0x82], bd, &pd, 1);
    for (i = 0; i < n; i++) { /* the source texel under each destination pixel's center, clamped to the surface */
        double fx = sx + ((double)(x0 + i - dx) + 0.5) * du;
        u32 cx = fx <= 0 ? 0 : fx >= (double)s->width ? s->width - 1 : (u32)fx;
        scol[i] = f2d_col(s, &ps, cx);
        dcol[i] = f2d_col(d, &pd, x0 + i);
    }
    gmem_before_write(pd.base, pd.span);
    for (y = y0; y < y1; y++) {
        double fy = sy + ((double)(y - dy) + 0.5) * dv;
        u32 cy = fy <= 0 ? 0 : fy >= (double)s->height ? s->height - 1 : (u32)fy, k;
        u64 sr = f2d_row(s, &ps, cy), dr = f2d_row(d, &pd, y);
        for (i = 0; i < n; i = k) { /* gather the row */
            k = i + 1;
            while (k < n && scol[k] == scol[k - 1] + bs) k++;
            if (ps.p)
                memcpy(row + (u64)i * bs, ps.p + sr + scol[i], (size_t)(k - i) * bs);
            else
                gmem_read_raw(ps.base + sr + scol[i], row + (u64)i * bs, (u64)(k - i) * bs);
        }
        for (i = 0; i < n; i = k) { /* and scatter it */
            k = i + 1;
            while (k < n && dcol[k] == dcol[k - 1] + bd) k++;
            if (pd.p)
                memcpy(pd.p + dr + dcol[i], row + (u64)i * bd, (size_t)(k - i) * bd);
            else
                gmem_write_raw(pd.base + dr + dcol[i], row + (u64)i * bd, (u64)(k - i) * bd);
        }
    }
    gpu_mem_written(pd.base, pd.span);
    if (g_renderer_up && g_renderer->rt_reload) g_renderer->rt_reload(d->addr);
    free(scol);
}
static double f2d_fixed(const u32* r, u32 lo) { /* a signed 32.32 value from two registers, the fraction first */
    return (double)(i64)(((u64)r[lo + 1] << 32) | r[lo]) / 4294967296.0;
}
static void f2d_blit(GpuChannel* ch) {
    const u32* r = ch->r2d;
    Surf2D s, d;
    int dx = (int)r[0x22C], dy = (int)r[0x22D], dw = (int)r[0x22E], dh = (int)r[0x22F];
    double du = f2d_fixed(r, 0x230), dv = f2d_fixed(r, 0x232), sx = f2d_fixed(r, 0x234), sy = f2d_fixed(r, 0x236);
    f2d_surface(r, 0x80, &d);
    f2d_surface(r, 0x8C, &s);
    if (dw <= 0 || dh <= 0) return;
    /* CORNER origin (0x223 bit 0) samples a step's start, CENTER its middle, as the renderer does: a corner's start is
       half a step back (Eden's Fermi2D::Blit) */
    if (r[0x223] & 1) {
        sx -= du / 2;
        sy -= dv / 2;
    }
    if (r[0x2AC] != 3 && r[0x2AC]) LOG_LIMIT(4, "gpu: a 2D blit's operation %u is done as a copy\n", r[0x2AC]);
    if (r3d_blit(&s, &d, dx, dy, dw, dh, sx, sy, du, dv)) return;
    f2d_blit_memory(r, &s, &d, dx, dy, dw, dh, sx, sy, du, dv);
}
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= 0x400) return;
    ch->r2d[m] = v;
    if (m == 0x237) {
        GPU_RELEASE_NOTE(ch, KR_2D, 0);
        f2d_blit(ch);
    }
}
