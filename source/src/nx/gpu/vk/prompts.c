/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 K0NL (B2recomp) and the Witch Time authors: ported from Witch Time v0.4.5 runtime/prompts.c
   (cells, pictures, the ASTC block and the order of painting are theirs). Copyright 2026 the B3-Express contributors:
   the texture cache hook, the frame tick and the choice from gamepad.c. See source/docs/CREDITS.md */
/* PlayStation / Xbox button prompts, no mod needed. Every prompt of the game comes from one atlas, core_sys_btn
   (ui/ui_core*.dtt, texture 0 of core.wtp): 256 x 512 ASTC 4x4, 40 x 40 cells. A cell holds two pictures: alpha is the
   button, RGB its backing (the same outline with its holes filled, 2 texels wider), which the action guides ("Climax")
   draw in black behind the button. Ten cells are the Switch's buttons. When the texture cache first makes that texture
   (texture.c tex_from_tic) the atlas is found by its hash and remembered; vk_prompts_frame paints those ten cells over
   it in guest memory, in its own format, whenever the choice changes, and the cache reads it again like any texture the
   game wrote. A mod's atlas has another hash and stays as it is.

   Face buttons: by position (gamepad_swap_ab=0, the default) Switch A, the right button, shows Circle / Xbox B; by printed
   letter (gamepad_swap_ab=1) A is the bottom button, and the pictures of where a button sits (four dots) swap too.
   Witch Time's keyboard set and its BC3 atlas (ui_core_gb) aren't ported: our keyboard keeps the Switch set. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/prompts.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/log.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function" /* the keyboard's pictures in prompt_art.h */
#endif
#include "nx/gpu/vk/prompt_art.h"

#define ATLAS_W    256
#define ATLAS_H    512
#define ATLAS_SIZE 0x20000
#define ATLAS_HASH 0xf1e63c42819024aull /* FNV-1a 64 of the game's ASTC atlas, as Witch Time found it */
#define ATLAS_BH   4                    /* its block height, log2 of GOBs */

enum { WMAX = 52 }; /* the widest cell */
typedef struct {
    int x, y, w; /* w: 0 = CELL */
} Cell;
static const Cell MINUS = {160, 0}, PLUS = {0, 132}, A = {204, 0}, B = {160, 44}, Y = {204, 44}, X = {0, 88};
static const Cell L1 = {56, 88}, R1 = {112, 88}, L2 = {160, 88}, R2 = {204, 88};
static const Cell DOTS[][2] = {{{160, 308}, {56, 352}},   /* right <-> bottom */
                               {{0, 352}, {112, 352}},    /* top <-> left */
                               {{112, 132}, {204, 352}}}; /* left + right <-> top + bottom */

/* ---- one block */
static void put(uint8_t* b, int pos, int n, unsigned v) {
    for (int i = 0; i < n; i++)
        if (v >> i & 1) b[(pos + i) / 8] |= (uint8_t)(1 << ((pos + i) & 7));
}
/* ASTC 4 x 4, LDR: one partition, luminance + alpha endpoints (CEM 4) fixed at transparent black and opaque white, two
   planes of 16 weights of 3 bits: luminance and alpha (ccs 3). Block mode 0x453: weight grid 4 x 4, dual plane, 0..7 */
void vk_prompts_astc_block(const uint8_t* rgba, int stride, uint8_t* out) {
    memset(out, 0, 16);
    put(out, 0, 11, 0x453);
    put(out, 13, 4, 4); /* bits 11-12 stay 0: one partition */
    put(out, 20, 3, 7);
    put(out, 26, 3, 7);
    put(out, 30, 2, 3); /* endpoints of 3 bits at 17: 0, 7, 0, 7; ccs */
    for (int i = 0; i < 32; i++) {
        int w = (rgba[((i >> 3) * stride + (i >> 1 & 3)) * 4 + (i & 1) * 3] * 7 + 127) / 255;
        for (int j = 0; j < 3; j++)
            if (w >> j & 1) { /* weights run from the top, bit-reversed */
                int p = 127 - (i * 3 + j);
                out[p / 8] |= (uint8_t)(1 << (p & 7));
            }
    }
}

/* the atlas's 16-byte block holding texel (x, y), block linear: one GOB is 64 bytes x 8 rows */
static uint8_t* block_at(uint8_t* mem, int x, int y) {
    const int wbytes = ATLAS_W / 4 * 16, bx = x / 4 * 16, by = y / 4;
    const size_t gob_block = (size_t)512 << ATLAS_BH, row = gob_block * (size_t)((wbytes + 63) / 64);
    const int gy = by >> 3, gx = bx & 63, yy = by & 7;
    return mem + (size_t)(gy >> ATLAS_BH) * row + (size_t)(bx >> 6) * gob_block +
           (size_t)(gy & ((1 << ATLAS_BH) - 1)) * 512 +
           (size_t)(((gx & 32) << 3) | ((yy & 6) << 5) | ((gx & 16) << 1) | ((yy & 1) << 4) | (gx & 15));
}
static void cell_swap(uint8_t* mem, Cell a, Cell b) {
    for (int y = 0; y < CELL; y += 4)
        for (int x = 0; x < CELL; x += 4) {
            uint8_t *p = block_at(mem, a.x + x, a.y + y), *q = block_at(mem, b.x + x, b.y + y), t[16];
            memcpy(t, p, 16);
            memcpy(p, q, 16);
            memcpy(q, t, 16);
        }
}
/* rgba holds the picture in alpha: RGB becomes its backing (top of this file) */
static void cell_backing(uint8_t* rgba, int w) {
    uint8_t v[WMAX * CELL];
    int x0[CELL], x1[CELL], y0[WMAX], y1[WMAX]; /* where the outline starts and ends on a row, in a column */
    for (int i = 0; i < CELL; i++) {
        x0[i] = w;
        x1[i] = -1;
    }
    for (int i = 0; i < w; i++) {
        y0[i] = CELL;
        y1[i] = -1;
    }
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < w; x++) {
            int m = 0;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++)
                    if (i * i + j * j <= 5 && y + j >= 0 && y + j < CELL && x + i >= 0 && x + i < w) {
                        int a = rgba[((y + j) * w + x + i) * 4 + 3];
                        m = a > m ? a : m;
                    }
            v[y * w + x] = (uint8_t)m;
            if (m > 127) {
                x0[y] = x < x0[y] ? x : x0[y];
                x1[y] = x;
                y0[x] = y < y0[x] ? y : y0[x];
                y1[x] = y;
            }
        }
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < w; x++)
            memset(rgba + (y * w + x) * 4, x > x0[y] && x < x1[y] && y > y0[x] && y < y1[x] ? 255 : v[y * w + x], 3);
}
static void cell_draw(uint8_t* mem, Cell c, const uint8_t* s) {
    int w = c.w ? c.w : CELL, bw = (w + 3) / 4, ox = (CELL - w) / 2; /* a picture is centred in its cell */
    uint8_t rgba[WMAX * CELL * 4];
    memset(rgba, 0, sizeof rgba);
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < bw * 4; x++) {
            int n = 0, sx = x + ox;
            if (sx >= 0 && sx < CELL)
                for (int j = 0; j < SS; j++)
                    for (int i = 0; i < SS; i++) n += s[(y * SS + j) * S + sx * SS + i];
            rgba[(y * bw * 4 + x) * 4 + 3] = (uint8_t)(n * 255 / (SS * SS));
        }
    cell_backing(rgba, bw * 4);
    for (int y = 0; y < CELL; y += 4)
        for (int x = 0; x < bw * 4; x += 4)
            vk_prompts_astc_block(rgba + (y * bw * 4 + x) * 4, bw * 4, block_at(mem, c.x + x, c.y + y));
}

/* mem: a copy of the game's atlas, block linear. family: 2 PlayStation, 3 Xbox */
void vk_prompts_paint(uint8_t* mem, int family, int by_letter) {
    typedef void Face(uint8_t*);
    uint8_t* s = malloc(S * S);
    if (!s) return;
#define PIC(cell, draw) (memset(s, 0, S * S), draw, cell_draw(mem, cell, s))
    if (family == 2) {
        Face *a = by_letter ? ps_cross : ps_circle, *b = by_letter ? ps_circle : ps_cross;
        Face *x = by_letter ? ps_square : ps_triangle, *y = by_letter ? ps_triangle : ps_square;
        PIC(A, a(s));
        PIC(B, b(s));
        PIC(X, x(s));
        PIC(Y, y(s));
        PIC(PLUS, bars_across(s));
        PIC(MINUS, bars_up(s));
        PIC(L1, bumper(s, "L1"));
        PIC(R1, bumper(s, "R1"));
        PIC(L2, trigger(s, "L2"));
        PIC(R2, trigger(s, "R2"));
    } else {
        if (!by_letter) { /* the game's letters are already Xbox's: by position they trade places */
            cell_swap(mem, A, B);
            cell_swap(mem, X, Y);
        }
        PIC(PLUS, bars_across(s));
        PIC(MINUS, view(s));
        PIC(L1, bumper(s, "LB"));
        PIC(R1, bumper(s, "RB"));
        PIC(L2, trigger(s, "LT"));
        PIC(R2, trigger(s, "RT"));
    }
#undef PIC
    if (by_letter)
        for (int i = 0; i < 3; i++) cell_swap(mem, DOTS[i][0], DOTS[i][1]);
    free(s);
}

/* ---- the atlas in use. Both entry points run under vk_lock (the texture cache, the present) */
static u64 g_gva;                  /* the atlas's GPU address, 0 = none known */
static uint8_t* g_shown;           /* which of the sets below guest memory holds */
static uint8_t* g_pic[4];          /* by family; [1] the game's own */
static int g_pic_sig[4], g_family; /* a set's by_letter + 1 when painted; the family shown */

static u64 fnv(const uint8_t* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* texture.c tex_from_tic, a texture made from memory: is this the atlas? */
void vk_prompts_texture(u64 gva, const VkFmt* f, u32 w, u32 h, u32 bh_log2) {
    if (w != ATLAS_W || h != ATLAS_H || f->conv != VKC_ASTC || f->bw != 4 || f->bh != 4 || bh_log2 != ATLAS_BH) return;
    if (gva == g_gva) return; /* ours, made again (evicted, or another view) */
    uint8_t* mem = gmem_ptr(gva, ATLAS_SIZE);
    if (!mem || fnv(mem, ATLAS_SIZE) != ATLAS_HASH) return;
    if (!g_pic[1] && !(g_pic[1] = malloc(ATLAS_SIZE))) return;
    memcpy(g_pic[1], mem, ATLAS_SIZE);
    g_gva = gva;
    g_shown = g_pic[1];
    g_family = 1;
    LOG("button prompts: the game's atlas at %llx\n", (unsigned long long)gva);
    vk_prompts_frame(); /* painted before the cache hashes and uploads it */
}

/* once a present (vk_tex_frame), and when the atlas is found: guest memory gets the set wanted */
void vk_prompts_frame(void) {
    if (!g_gva) return;
    int by_letter = 0, family = gamepad_prompts(&by_letter);
    if (family < 1 || family > 3) family = 1;
    int sig = family == 1 ? 0 : by_letter + 1;
    if (family == g_family && sig == g_pic_sig[family]) return;
    uint8_t* mem = gmem_ptr(g_gva, ATLAS_SIZE);
    if (!mem || memcmp(mem, g_shown, ATLAS_SIZE)) { /* not what we left there: the game has reused the memory */
        g_gva = 0;
        LOG("button prompts: the atlas's memory changed; it is looked for again\n");
        return;
    }
    if (!g_pic[family] && !(g_pic[family] = malloc(ATLAS_SIZE))) return;
    if (family > 1 && g_pic_sig[family] != sig) {
        memcpy(g_pic[family], g_pic[1], ATLAS_SIZE);
        vk_prompts_paint(g_pic[family], family, by_letter);
    }
    g_pic_sig[family] = sig;
    memcpy(mem, g_pic[family], ATLAS_SIZE);
    g_shown = g_pic[family];
    g_family = family;
    vk_tex_invalidate_all(); /* the cache checks textures against memory again: this one changed */
    LOG("button prompts: %s%s\n",
        family == 1   ? "Switch"
        : family == 2 ? "PlayStation"
                      : "Xbox",
        family > 1 && by_letter ? ", face buttons by printed letter" : "");
}
