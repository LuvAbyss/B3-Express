/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bayonetta 3 v1.2.0's own picture at another shape than 16:9 (aspect_ratio=, width / height; 0 the game's).

   Ported from Witch Time (github.com/Zexyen/Witch-Time v0.4.5, runtime/game_shim_eh.c from "aspect ratios" and
   r2_imports.c aspect_extra, GPL-3.0-or-later; docs/STATUS.md "Aspect ratios"): the functions, what each is given and
   the rules. Stage 1 of diagnostics/widescreen-20261107/README.md: the 3D view takes the screen's shape (wider shows
   more at the sides, narrower more above and below) and the interface stays a 16:9 block in the middle.

   - main+0x70a740 (int* w x0, int* h x1) answers the docked screen size, 1600x900, and main+0x70a760 the handheld one,
     1280x720. Their callers make the game's window textures that size. The height stays; the width follows the ratio.
   - main+0xd01380 (config x0): the graphics set-up. config + 0x90 is the heap the render targets and window textures
     are placed in, 0x10400000, sized for 16:9: wider, it grows with the width, and the memory the game is told it has
     grows as much (mem_total), since the game sizes its own heap from that.
   - main+0xcba90c (camera x0, fov s0, near s1, far s2, width w1, height w2): a camera's perspective, its aspect
     width / height (camera + 0x3d8). A 16:9 size (within 1 %) gets the screen's shape; narrower than 16:9 the vertical
     angle grows so the horizontal one stays (Vert+). main+0xcba97c (camera x0, fov s0) sets the angle alone: the same
     there on a camera already of the screen's shape.
   - main+0xd07610 (slot w0, left s0, right s1, top s2, bottom s3, ...): the interface's flat projection, 0 to 1280 and
     0 to 720 whatever the screen. It gets the 1280x720 in the middle, unstretched, and more around it.

   The game's scene targets keep their 16:9 sizes, and its last pass stretches them into the window: the shapes come out
   right. Like scene60.c, the direct calls go through g_disp only in a game module compiled with these addresses in its
   .tablecalls file. */
#include "nx/hle/aspect.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#include <math.h>
#include <string.h>

#define MAIN_FROM_LOW  0x5000 /* main loads right after rtld */
#define FN_SCREEN_DOCK 0x70a740
#define FN_SCREEN_HAND 0x70a760
#define FN_GFX_SETUP   0xd01380
#define FN_PERSPECTIVE 0xcba90c
#define FN_FOV         0xcba97c
#define FN_UI_ORTHO    0xd07610
#define GFX_HEAP       0x90 /* in the set-up's config, u64 */
#define GFX_HEAP_16_9  0x10400000ull
#define CAMERA_ASPECT  0x3d8 /* in a camera, f32 */
#define WIDE           (16.0f / 9.0f)

static u64 g_main;
static GuestFn g_dock, g_hand, g_setup, g_persp, g_fov, g_ortho;
static float g_a; /* aspect_ratio, 0: off */

float aspect_ratio(void) { return g_a; }
u64 aspect_extra_memory(void) {
    float a = g_opt_aspect >= 0.5f && g_opt_aspect <= 4.0f && fabsf(g_opt_aspect - WIDE) > 0.001f ? g_opt_aspect : 0.0f;
    if (a <= WIDE) return 0;
    return ((u64)((double)GFX_HEAP_16_9 * ((double)a * 9.0 / 16.0 - 1.0)) + 0x1fffff) & ~(u64)0x1fffff;
}

static GuestFn take(u64 offset, GuestFn host) {
    GuestFn* slot = &g_disp[(g_main + offset - g_code_lo) / 4];
    GuestFn guest = *slot ? *slot : snx_interp_entry;
    *slot = host;
    return guest;
}
static u32 word_at(u64 offset) {
    u32 w = 0;
    memcpy(&w, GP(g_main + offset), sizeof w);
    return w;
}
static i32 even_width(i32 h) { return 2 * (i32)((float)h * g_a / 2.0f + 0.5f); }
static float vert_plus(float fov) { return 2.0f * atanf(tanf(fov * 0.5f) * WIDE / g_a); }

static void screen_size(CPU* RESTRICT c) { /* the originals only store: x0 and x1 stay */
    i32 h, w;
    memcpy(&h, GP(c->x[1]), sizeof h);
    w = even_width(h);
    memcpy(GP(c->x[0]), &w, sizeof w);
}
static void screen_docked(CPU* RESTRICT c, u64 pc) {
    g_dock(c, pc);
    screen_size(c);
}
static void screen_handheld(CPU* RESTRICT c, u64 pc) {
    g_hand(c, pc);
    screen_size(c);
}
static void gfx_setup(CPU* RESTRICT c, u64 pc) {
    u64 size, extra = aspect_extra_memory();
    if (extra) {
        memcpy(&size, GP(c->x[0] + GFX_HEAP), sizeof size);
        size += extra;
        memcpy(GP(c->x[0] + GFX_HEAP), &size, sizeof size);
        LOG("aspect: the render-target heap is 0x%llx bytes\n", (unsigned long long)size);
    }
    g_setup(c, pc);
}
static void perspective(CPU* RESTRICT c, u64 pc) {
    i32 w = (i32)c->x[1], h = (i32)c->x[2];
    float d = h > 0 ? (float)w / (float)h - WIDE : 1.0f;
    if (d < 0.018f && d > -0.018f) {
        c->x[1] = (u32)(i32)((float)h * g_a + 0.5f);
        if (g_a < WIDE) c->v[0].f[0] = vert_plus(c->v[0].f[0]);
    }
    g_persp(c, pc);
}
static void fov(CPU* RESTRICT c, u64 pc) {
    u64 cam = 0;
    float ca = 0.0f;
    memcpy(&cam, GP(c->x[0]), sizeof cam);
    if (g_a < WIDE && cam) {
        memcpy(&ca, GP(cam + CAMERA_ASPECT), sizeof ca);
        if (ca - g_a < 0.001f && g_a - ca < 0.001f) c->v[0].f[0] = vert_plus(c->v[0].f[0]);
    }
    g_fov(c, pc);
}
static void ui_ortho(CPU* RESTRICT c, u64 pc) {
    if (g_a > WIDE) {
        float x0 = (1280.0f - (float)even_width(720)) / 2.0f;
        c->v[0].f[0] += x0;
        c->v[1].f[0] -= x0;
    } else {
        float k = 1280.0f / (720.0f * g_a), y0 = (720.0f - 720.0f * k) / 2.0f;
        c->v[2].f[0] += y0;
        c->v[3].f[0] -= y0;
    }
    g_ortho(c, pc);
}

void aspect_init(void) {
    float a = g_opt_aspect;
    if (!(a >= 0.5f && a <= 4.0f) || fabsf(a - WIDE) <= 0.001f) return;
    g_main = g_code_lo + MAIN_FROM_LOW;
    /* words of the functions in v1.2.0: the getters' mov w8, #900 / #720 (after storing the width); the set-up's
       mov x2, x1; the perspective's scvtf s3, w1; the projection's mov w0, wzr */
    if (!g_disp || g_main + FN_UI_ORTHO >= g_code_hi || word_at(FN_SCREEN_DOCK + 8) != 0x52807088u ||
        word_at(FN_SCREEN_HAND + 8) != 0x52805a08u || word_at(FN_GFX_SETUP + 8) != 0xaa0103e2u ||
        word_at(FN_PERSPECTIVE + 0x10) != 0x1e220023u || word_at(FN_UI_ORTHO + 4) != 0x2a1f03e0u) {
        LOG("aspect: this isn't the game it knows, the picture stays 16:9\n");
        return;
    }
    g_a = a;
    g_dock = take(FN_SCREEN_DOCK, screen_docked);
    g_hand = take(FN_SCREEN_HAND, screen_handheld);
    g_setup = take(FN_GFX_SETUP, gfx_setup);
    g_persp = take(FN_PERSPECTIVE, perspective);
    g_fov = take(FN_FOV, fov);
    g_ortho = take(FN_UI_ORTHO, ui_ortho);
    LOG("aspect: the game's picture at %.3f (docked %dx900), the interface a 16:9 block in the middle\n", (double)a,
        even_width(900));
}
