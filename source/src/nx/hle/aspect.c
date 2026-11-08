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

   Stage 2a, the interface's details (game_shim_eh.c h_screen_w to h_aim_pos). Witch Time tells callers apart by their
   return address, which our generated code sets the same way (x30), so its lists carry over as main's offsets. Where
   it hands a function a copy of the caller's data on its own stack, here the caller's data is corrected in place and
   put back after the call: guest addresses aren't host ones.

   - main+0xd02d70 answers the UI's width, 1280 (graphics object + 0x130). The calls that work in the camera's viewport
     (the chapter select's aim, the off-screen arrow, on-screen tests, target picks, the ray through the middle, the
     stick-aim cursor) get the viewport's width: the screen's height (+ 0x134) x the ratio.
   - A UI clip element (the gauges' fill is a scissor) turns its rectangle into the target's pixels as (x + 640) x the
     target's width (main+0xd02d90) / 1280. Wider, the width it reads is scaled by 1280 / the UI's width, and the
     rectangle at the caller's sp after main+0xd04414 moves right by half the extra width (one over the whole 1280
     becomes the whole screen). Taller, the same with the height (main+0xd02db0).
   - The capture effects (blur, colour shift, radial, the Yes / No dialog) draw the window back with u = UI x / 1280:
     main+0xc025a4 and main+0xc00b98 (quad at x2, u0 v0 u1 v1) get it in the widened UI's space.
   - HUD elements placed at a point of the 3D view (main+0xc1656c, the point at x1) get it mapped into the UI's space,
     as does the aim's ring (main+0xa6f5a0, s0 s1). Those testing the right or bottom edge with the UI's size
     (main+0xbfcf70, main+0xbfcf80) get the real one.

   The game's scene targets keep their 16:9 sizes, and its last pass stretches them into the window: the shapes come out
   right. Like scene60.c, the direct calls go through g_disp only in a game module compiled with these addresses in its
   .tablecalls file. */
#include "nx/hle/aspect.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/rtapi.h"
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
#define FN_UI_W        0xd02d70 /* stage 2 */
#define FN_TARGET_W    0xd02d90
#define FN_TARGET_H    0xd02db0
#define FN_CLIP_CMD    0xd04414
#define FN_CAPTURE     0xc025a4
#define FN_GAUSS       0xc00b98
#define FN_UI_POS      0xc1656c
#define FN_UI_RIGHT    0xbfcf70
#define FN_UI_BOTTOM   0xbfcf80
#define FN_AIM_POS     0xa6f5a0
#define GFX_OBJECT     0x24accc0 /* main's pointer to the graphics object, its screen height at + 0x134 */
#define GFX_HEAP       0x90      /* in the set-up's config, u64 */
#define GFX_HEAP_16_9  0x10400000ull
#define CAMERA_ASPECT  0x3d8 /* in a camera, f32 */
#define WIDE           (16.0f / 9.0f)

static u64 g_main;
static GuestFn g_dock, g_hand, g_setup, g_persp, g_fov, g_ortho;
static GuestFn g_ui_w, g_target_w, g_target_h, g_clip_cmd, g_capture, g_gauss, g_ui_pos, g_ui_right, g_ui_bottom, g_aim;
static float g_a; /* aspect_ratio, 0: off */
/* a point of the screen in 720 high units (the viewport's) -> the UI's: x * g_k + g_x0, y * g_k + g_y0 (ui_ortho) */
static float g_x0, g_y0, g_k = 1.0f;
static float g_clip_off, g_clip_y, g_clip_w, g_clip_h; /* target_w and target_h, for clip_cmd */

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
        g_x0 = (1280.0f - (float)even_width(720)) / 2.0f;
        c->v[0].f[0] += g_x0;
        c->v[1].f[0] -= g_x0;
    } else {
        g_k = 1280.0f / (720.0f * g_a);
        g_y0 = (720.0f - 720.0f * g_k) / 2.0f;
        c->v[2].f[0] += g_y0;
        c->v[3].f[0] -= g_y0;
    }
    g_ortho(c, pc);
}

/* x30 as an offset into main, to tell callers apart */
static u64 caller(const CPU* RESTRICT c) { return c->x[30] - g_main; }
static int listed(u64 lr, const u32* list, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i)
        if (list[i] == lr) return 1;
    return 0;
}

static const u32 k_view_w[] = {
    0x9207fc,                                                   /* the chapter select's aim */
    0x9c93b8, 0x9c9520, 0x9c9640,                               /* the off-screen arrow */
    0x269080, 0x5a1a00, 0x864cc4, 0x86fb10, 0x8af4f8,           /* is a projected point on screen? */
    0x787350, 0x787360, 0x7834ec, 0x783500, 0x783534,           /* target picks around the middle */
    0x52dfec,                                                   /* the ray through the middle */
    0x4f63d4, 0x4f65ac, 0x4f7390, 0x4f7438, 0x41af94, 0x4a9828, /* the stick-aim cursor: half this width, unprojected */
    0x4d8954, 0x4d8c7c, 0x536c84, 0x53819c, 0x538710, 0x398c74};
static void ui_w(CPU* RESTRICT c, u64 pc) {
    u64 g = 0;
    i32 h = 0;
    g_ui_w(c, pc);
    if (!listed(caller(c), k_view_w, sizeof k_view_w / sizeof *k_view_w)) return;
    memcpy(&g, GP(g_main + GFX_OBJECT), sizeof g);
    if (!g) return;
    memcpy(&h, GP(g + 0x134), sizeof h);
    c->x[0] = (u32)even_width(h);
}
static void target_w(CPU* RESTRICT c, u64 pc) {
    u64 lr = caller(c);
    g_target_w(c, pc);
    if (g_a > WIDE && (lr == 0xc1e044 || lr == 0xc1e110)) {
        double ui = (double)even_width(720), w = (double)(u32)c->x[0];
        g_clip_off = (float)((ui - 1280.0) / 2.0 * w / ui);
        g_clip_w = (float)w;
        c->x[0] = (u32)(w * 1280.0 / ui + 0.5);
    }
}
static void target_h(CPU* RESTRICT c, u64 pc) {
    u64 lr = caller(c);
    g_target_h(c, pc);
    if (g_a < WIDE && (lr == 0xc1e04c || lr == 0xc1e118)) {
        double ui = 1280.0 / (double)g_a, h = (double)(u32)c->x[0];
        g_clip_y = (float)((ui - 720.0) / 2.0 * h / ui);
        g_clip_h = (float)h;
        c->x[0] = (u32)(h * 720.0 / ui + 0.5);
    }
}
static void clip_cmd(CPU* RESTRICT c, u64 pc) {
    int ours = caller(c) == 0xc1e38c;
    float r[4], t; /* x0 y0 x1 y1 */
    g_clip_cmd(c, pc);
    if (!ours || (g_clip_off == 0.0f && g_clip_y == 0.0f)) return;
    memcpy(r, GP(c->x[31]), sizeof r);
    /* a rectangle over the whole 1280 reaches the screen's edges (a heading's wipe is that long at very wide ratios) */
    t = (g_clip_w - 2.0f * g_clip_off) * 0.99f;
    if (g_clip_off != 0.0f && r[2] - r[0] >= t && r[2] >= t) {
        r[0] = 0.0f;
        r[2] = g_clip_w;
    } else {
        r[0] += g_clip_off;
        r[2] += g_clip_off;
    }
    if (g_clip_y != 0.0f && r[3] - r[1] >= (g_clip_h - 2.0f * g_clip_y) * 0.99f) {
        r[1] = 0.0f;
        r[3] = g_clip_h;
    } else {
        r[1] += g_clip_y;
        r[3] += g_clip_y;
    }
    memcpy(GP(c->x[31]), r, sizeof r);
}
/* the quad at x2 (u0 v0 u1 v1) in the widened UI's space for the call, then the caller's own put back */
static void capture_call(CPU* RESTRICT c, u64 pc, GuestFn original) {
    float keep[4], t[4], w = 1280.0f - 2.0f * g_x0, h = 720.0f - 2.0f * g_y0;
    u64 at = c->x[2];
    memcpy(keep, GP(at), sizeof keep);
    t[0] = (keep[0] * 1280.0f - g_x0) / w;
    t[2] = (keep[2] * 1280.0f - g_x0) / w;
    t[1] = (keep[1] * 720.0f - g_y0) / h;
    t[3] = (keep[3] * 720.0f - g_y0) / h;
    memcpy(GP(at), t, sizeof t);
    original(c, pc);
    memcpy(GP(at), keep, sizeof keep);
}
static void capture(CPU* RESTRICT c, u64 pc) {
    u64 lr = caller(c);
    u32 cap = 1;
    /* the colour shift's call with the sprite's own UVs (a material, [x21 + 0x18] = 0) isn't a capture */
    if (lr == 0x972924) memcpy(&cap, GP(c->x[21] + 0x18), sizeof cap);
    if ((g_x0 != 0.0f || g_y0 != 0.0f) && (lr == 0x9732dc || lr == 0x972dc8 || lr == 0x972924) && cap)
        capture_call(c, pc, g_capture);
    else
        g_capture(c, pc);
}
static void gauss(CPU* RESTRICT c, u64 pc) { /* two callers, the blur's two passes */
    if (g_x0 != 0.0f || g_y0 != 0.0f)
        capture_call(c, pc, g_gauss);
    else
        g_gauss(c, pc);
}
static const u32 k_ui_pos[] = {0x9ae8c0, 0x9aedb0, 0x9b2380, 0x9b2440, 0x9b3ff4,
                               0x9b40c4, 0x9c961c, 0x9b6fe8, 0x9b709c /* the stick-aim reticle */};
static void ui_pos(CPU* RESTRICT c, u64 pc) {
    float keep[2], p[2];
    u64 at = c->x[1];
    if (!listed(caller(c), k_ui_pos, sizeof k_ui_pos / sizeof *k_ui_pos)) {
        g_ui_pos(c, pc);
        return;
    }
    memcpy(keep, GP(at), sizeof keep);
    p[0] = keep[0] * g_k + g_x0;
    p[1] = keep[1] * g_k + g_y0;
    memcpy(GP(at), p, sizeof p);
    g_ui_pos(c, pc);
    memcpy(GP(at), keep, sizeof keep);
}
static void ui_right(CPU* RESTRICT c, u64 pc) {
    u64 lr = caller(c);
    g_ui_right(c, pc);
    if (lr == 0x9a83a8 || lr == 0x9b05a0)
        c->v[0].f[0] = 1280.0f - 2.0f * g_x0;
    else if (lr == 0x9b274c || lr == 0x9b28a0 || lr == 0x9b48a4)
        c->v[0].f[0] = (float)even_width(720);
}
static void ui_bottom(CPU* RESTRICT c, u64 pc) {
    u64 lr = caller(c);
    g_ui_bottom(c, pc);
    if (lr == 0x9a83b0 || lr == 0x9b05a8) c->v[0].f[0] = 720.0f - 2.0f * g_y0;
}
static void aim_pos(CPU* RESTRICT c, u64 pc) {
    c->v[0].f[0] = c->v[0].f[0] * g_k + g_x0;
    c->v[1].f[0] = c->v[1].f[0] * g_k + g_y0;
    g_aim(c, pc);
}

/* Stage 2b, hook points (rt_hook_set; the game code compiled with launcher/code-insnhooks.txt), Witch Time's lifter
   eh.rs UI_SIZE_SITES and UI_MARK_SITES. A sprite in the UI's screen mode (+0x4c = 1) is as large as the screen: 1280.0
   and 720.0, mov w8-w10 immediates in main+0xc15ae4 and main+0xc180xx-0xc1a8f4. After each the register holds the
   UI's real size (the hook is on the next instruction, and checks the constant is still there). */
static const u32 k_size_sites[][2] = {
    /* main offset of the mov, its register */
    {0xc15b78, 9},  {0xc15bd8, 8}, {0xc1818c, 8},  {0xc181dc, 8},  {0xc19108, 8},  {0xc19110, 8},  {0xc19250, 8},
    {0xc19270, 8},  {0xc19330, 8}, {0xc19350, 8},  {0xc19688, 9},  {0xc19694, 9},  {0xc19d18, 8},  {0xc19d30, 8},
    {0xc19df4, 8},  {0xc19e0c, 8}, {0xc19ea4, 10}, {0xc19f00, 10}, {0xc19f64, 10}, {0xc19fd8, 8},  {0xc1a06c, 10},
    {0xc1a0e8, 10}, {0xc1a108, 8}, {0xc1a1a4, 8},  {0xc1a610, 10}, {0xc1a628, 10}, {0xc1a6c8, 10}, {0xc1a6ec, 10},
    {0xc1a7d4, 10}, {0xc1a7f4, 8}, {0xc1a808, 8},  {0xc1a8f4, 8}};
#define F1280 0x44a00000u
#define F720  0x44340000u
static void ui_size(CPU* RESTRICT c, u64 pc) {
    size_t i;
    float s;
    u32 bits;
    for (i = 0; i < sizeof k_size_sites / sizeof *k_size_sites; ++i) {
        u64* x;
        if (g_main + k_size_sites[i][0] + 4 != pc) continue;
        x = &c->x[k_size_sites[i][1]];
        if (*x == F1280)
            s = 1280.0f - 2.0f * g_x0;
        else if (*x == F720)
            s = 720.0f - 2.0f * g_y0;
        else
            return;
        memcpy(&bits, &s, sizeof bits);
        *x = bits;
        return;
    }
}
/* Markers kept on screen (main+0x9a81f4, main+0x9b05fc's function) place themselves at 0..1 of the screen times its size,
   clamped: a position from the UI's left edge. Before it is stored it gets the UI's left and top edge: stp s1, s0 at
   main+0x9a83e8, str s8 then str s0 at main+0x9b05fc. */
#define MARK_A 0x9a83e8
#define MARK_B 0x9b05fc
static void ui_mark(CPU* RESTRICT c, u64 pc) {
    c->v[pc == g_main + MARK_A ? 1 : 8].f[0] += g_x0;
    c->v[0].f[0] += g_y0;
}
static void hook_points(void) {
    size_t i, set = 0;
    for (i = 0; i < sizeof k_size_sites / sizeof *k_size_sites; ++i) {
        u32 w = word_at(k_size_sites[i][0]), r = k_size_sites[i][1];
        if (w != (0x52a89400u | r) && w != (0x52a88680u | r)) {
            LOG("aspect: main+0x%x isn't the mov it knows, the UI's sprites keep the 16:9 size\n", k_size_sites[i][0]);
            return;
        }
    }
    if (word_at(MARK_A) != 0x2d0403e1u || word_at(MARK_B) != 0xbd016268u) {
        LOG("aspect: the markers' stores aren't the ones it knows, they keep the 16:9 place\n");
        return;
    }
    for (i = 0; i < sizeof k_size_sites / sizeof *k_size_sites; ++i)
        set += (size_t)rt_hook_set(g_main + k_size_sites[i][0] + 4, ui_size);
    set += (size_t)rt_hook_set(g_main + MARK_A, ui_mark);
    set += (size_t)rt_hook_set(g_main + MARK_B, ui_mark);
    LOG("aspect: %u hook points set\n", (unsigned)set);
}

void aspect_init(void) {
    float a = g_opt_aspect;
    if (!(a >= 0.5f && a <= 4.0f) || fabsf(a - WIDE) <= 0.001f) return;
    g_main = g_code_lo + MAIN_FROM_LOW;
    /* words of the functions in v1.2.0: the getters' mov w8, #900 / #720 (after storing the width); the set-up's
       mov x2, x1; the perspective's scvtf s3, w1; the projection's mov w0, wzr */
    if (!g_disp || g_main + FN_UI_ORTHO >= g_code_hi || word_at(FN_SCREEN_DOCK + 8) != 0x52807088u ||
        word_at(FN_SCREEN_HAND + 8) != 0x52805a08u || word_at(FN_GFX_SETUP + 8) != 0xaa0103e2u ||
        word_at(FN_PERSPECTIVE + 0x10) != 0x1e220023u || word_at(FN_UI_ORTHO + 4) != 0x2a1f03e0u ||
        /* stage 2: the UI width's ldr w0, [x8, #0x130], the targets' ldr w0, [x8, #0x10cc] and [x8, #0x10d0], the clip
           command's ldr x8, [x0, #0xb0], the capture quad's mov w0, #0x10, the blur's sub sp, sp, #0x60, the UI
           position's fmov v0.4s, #1.0, the UI size's 1280.0 and 720.0, the aim's mov w9, #0x4160 */
        word_at(FN_UI_W + 8) != 0xb9413100u || word_at(FN_TARGET_W + 0xc) != 0xb950cd00u ||
        word_at(FN_TARGET_H + 0x10) != 0xb950d100u || word_at(FN_CLIP_CMD) != 0xf9405808u ||
        word_at(FN_CAPTURE + 0x14) != 0x52800200u || word_at(FN_GAUSS) != 0xd10183ffu ||
        word_at(FN_UI_POS + 8) != 0x4f03f600u || word_at(FN_UI_RIGHT) != 0x52a89408u ||
        word_at(FN_UI_BOTTOM) != 0x52a88688u || word_at(FN_AIM_POS + 8) != 0x52882c09u) {
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
    g_ui_w = take(FN_UI_W, ui_w);
    g_target_w = take(FN_TARGET_W, target_w);
    g_target_h = take(FN_TARGET_H, target_h);
    g_clip_cmd = take(FN_CLIP_CMD, clip_cmd);
    g_capture = take(FN_CAPTURE, capture);
    g_gauss = take(FN_GAUSS, gauss);
    g_ui_pos = take(FN_UI_POS, ui_pos);
    g_ui_right = take(FN_UI_RIGHT, ui_right);
    g_ui_bottom = take(FN_UI_BOTTOM, ui_bottom);
    g_aim = take(FN_AIM_POS, aim_pos);
    hook_points();
    LOG("aspect: the game's picture at %.3f (docked %dx900), the interface a 16:9 block in the middle\n", (double)a,
        even_width(900));
}
