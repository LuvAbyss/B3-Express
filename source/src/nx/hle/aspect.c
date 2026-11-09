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
/* Stage 3, the layouts (game_shim_eh.c h_ui_element, ui_name, ui_rule). main+0xc0ef04 (layout x0, state out x1, element
   w2, time s0) works out one element of a UI layout (a .uid of a ui .dat, elements of 0x1b4 bytes from layout +
   [layout + 0xc]) for this frame: its position into out + 0x10, its scale into out + 0x50, and so on. The layouts are
   in a 1280x720 space around the middle, which the projection keeps in the middle (ui_ortho). After it:
   - what is 1280 across (not turned, in the middle: dims, strips, the cutscene bars) gets the screen's width, and the
     vignettes turned a quarter their length; a few layouts by name (Game Over's paper, Play History's vignette, the
     bands' end lines, the event shutter, the chapter select's banner, the credits' ring);
   - the rule under a heading (a nine-grid picture past the 1280's left edge) reaches the screen's left edge;
   - hud_area 0: a root (element 0) of a HUD layout (a .uid named hud*) placed 200 or more from the middle (health,
     gauges, score) moves out by half the extra width, to the screen's edge as at 16:9. Centred ones stay. */
#define FN_UI_ELEMENT 0xc0ef04
#define UI_EL_SIZE    0x1b4u
static GuestFn g_ui_element;
static u32 rd32(u64 a) {
    u32 v = 0;
    memcpy(&v, GP(a), sizeof v);
    return v;
}
static float rdf(u64 a) {
    float v = 0;
    memcpy(&v, GP(a), sizeof v);
    return v;
}
static void wrf(u64 a, float v) { memcpy(GP(a), &v, sizeof v); }
/* The name of the layout at lay, an entry of a DAT archive in memory, or "". The archive ("DAT\0", file count, offset
   table at +8, name table at +0x10: u32 name size, then the names) starts before it, 4-aligned, within 16 MB; its
   offset table holds lay - archive. Looked up once per layout (a table of 1024, checked by counts and sizes, as another
   layout can be loaded at the same address later). The name is copied: up to 15 characters are all that's compared */
static const char* ui_name(u64 lay) {
    static struct {
        u64 lay, sig;
        char name[16];
    } t[1024];
    u32 h = (u32)((lay >> 2) * 0x9E3779B1u) >> 22, i, e = h;
    u64 sig = rd32(lay) | (u64)rd32(lay + 4) << 16 ^ (u64)rd32(lay + 0x10) << 32, p;
    const char* name = "";
    MemBlock b;
    for (i = 0; i < 8; i++)
        if (t[(h + i) & 1023].lay == lay && t[(h + i) & 1023].sig == sig) return t[(h + i) & 1023].name;
    mem_query(lay, &b); /* within lay's run of pages: guest memory below it may not be mapped on the host */
    for (p = lay & ~3ull; p + (16u << 20) > lay && p >= b.addr && p >= 0x1000; p -= 4) {
        u32 n, po, no, nl, k;
        if (rd32(p) != 0x00544144u) continue; /* "DAT\0" */
        n = rd32(p + 4);
        po = rd32(p + 8);
        no = rd32(p + 0x10);
        if (n == 0 || n > 4096 || po > (16u << 20) || no > (16u << 20)) continue;
        nl = rd32(p + no);
        if (nl == 0 || nl > 256) continue;
        for (k = 0; k < n; k++)
            if (p + rd32(p + po + 4 * k) == lay) {
                name = (const char*)GP(p + no + 4 + (u64)k * nl);
                goto found;
            }
    }
found:
    for (i = 0; i < 8; i++) /* the first free or stale slot of the 8, else the first */
        if (!t[(h + i) & 1023].lay || t[(h + i) & 1023].lay == lay) {
            e = (h + i) & 1023;
            break;
        }
    t[e].lay = lay;
    t[e].sig = sig;
    strncpy(t[e].name, name, sizeof t[e].name - 1);
    t[e].name[sizeof t[e].name - 1] = 0;
    return t[e].name;
}
/* The rule under a heading (Main Menu, Options, the Shop ...): one picture in a nine-grid (kind 2), anchored left,
   starting past the 1280's left edge; scale lengthens only its straight middle. Half are wiped in by a clip (kind 6, the
   element before them) whose rectangle cut them at the 1280's edge. 0 if el isn't one, else the scale that takes the
   clip to the screen's left edge (1: no clip) */
static float ui_rule(u64 lay, u32 el) {
    u32 n = rd32(lay), f = rd32(lay + 0xc), kind, d, part, anchor;
    i32 par;
    float px, w;
    u64 e, p;
    if (el >= n) return 0.0f;
    e = lay + f + el * (u64)UI_EL_SIZE;
    par = (i32)rd32(e + 0x44);
    kind = rd32(e + 0x198);
    d = rd32(e + 0x1a4);
    if (kind != 2 || !d || par < 0 || rdf(e + 0x14) != 0.0f) return 0.0f;
    part = rd32(lay + d + 0x24);
    anchor = rd32(lay + d + 0x10);
    p = lay + f + (u32)par * (u64)UI_EL_SIZE;
    px = rdf(p);
    kind = rd32(p + 0x198);
    d = rd32(p + 0x1a4);
    if (part != 0x364a6e68u || anchor || px > -620.0f || rdf(p + 0x14) != 0.0f) return 0.0f;
    if (kind != 6) return 1.0f;
    w = rdf(lay + d + 0xc);
    return (w - g_x0) / w;
}
static float ui_k(void) { return g_a > WIDE ? g_a * 9.0f / 16.0f : 16.0f / (g_a * 9.0f); }
static void ui_element(CPU* RESTRICT c, u64 pc) {
    u64 lay = c->x[0], out = c->x[1], e;
    u32 el = (u32)c->x[2], f, kind, ty, d, mode = 0;
    float ex0, sc[2], rot, sz[2] = {0, 0}, v[2], wd, ht, rule;
    const char* nm;
    g_ui_element(c, pc);
    if (!(u32)lay) return; /* no such element */
    f = rd32(lay + 0xc);
    e = lay + f + el * (u64)UI_EL_SIZE;
    ex0 = rdf(e);
    rot = rdf(e + 0x14);
    sc[0] = rdf(e + 0x18);
    sc[1] = rdf(e + 0x1c);
    kind = rd32(e + 0x198);
    ty = rd32(e + 0x19c);
    d = rd32(e + 0x1a4);
    if ((kind == 1 || kind == 2) && d) { /* a picture's or a framed panel's size; mode 1: the screen's (ui_size) */
        sz[0] = rdf(lay + d);
        sz[1] = rdf(lay + d + 4);
        mode = rd32(lay + d + 0x18);
    }
    v[0] = rdf(out + 0x50);
    v[1] = rdf(out + 0x54);
    wd = sz[0] * sc[0];
    ht = sz[1] * sc[1];
    if ((ty == 0 || ty == 2001 || ty == 1021 || ty == 1000) && mode != 1 &&
        (wd >= 1270.0f || (wd >= 1260.0f && ht < 100.0f)) && sz[1] >= 1.0f && sz[0] != sz[1] && rot == 0.0f &&
        ex0 >= -30.0f && ex0 <= 30.0f) {                        /* 1280 across; none has children */
        if (el != 4 || strncmp(ui_name(lay), "event0015", 9)) { /* the credits' neighbours are 1280 away: below */
            /* a whole screen keeps its shape and loses a little at the edges; the capture is the screen's already */
            int full = ht >= 700.0f, shot = ty == 1021 || ty == 1000;
            if (g_a > WIDE || (full && !shot)) v[0] *= ui_k();
            if (g_a > WIDE ? full && !shot : full) v[1] *= ui_k();
            wrf(out + 0x50, v[0]);
            wrf(out + 0x54, v[1]);
        }
    } else if (g_a > WIDE && kind == 2 && ht >= 1240.0f && ex0 >= -30.0f && ex0 <= 30.0f &&
               (fabsf(rot - 1.5708f) < 0.02f || fabsf(rot + 1.5708f) < 0.02f)) { /* the vignettes: length is y */
        wrf(out + 0x54, v[1] * g_a * 9.0f / 16.0f);
    } else if (el >= 11 && el <= 22 && !strncmp(ui_name(lay), "hud0100", 7)) { /* Game Over's paper */
        if (el <= 12) { /* its speckles drift 200 from their place: as the rule, or their edge comes to the 1280's */
            wrf(out + 0x50, v[0] * ui_k());
            wrf(out + 0x54, v[1] * ui_k());
        } else if (el >= 17) { /* the corner triangles and the bands along the top and bottom, to the screen's edges */
            float x = rdf(out + 0x10), y = rdf(out + 0x14);
            wrf(out + 0x10, x + (x < 0.0f ? g_x0 : -g_x0));
            wrf(out + 0x14, y + (y < 0.0f ? g_y0 : -g_y0));
            if (el >= 21) wrf(out + 0x54, v[1] - 2.0f * g_x0 / sz[1]);
        }
    } else if (el >= 20 && el <= 23) {
        if (!strncmp(ui_name(lay), "menu006a", 8)) { /* Play History's vignette: strips at the 1280's or 720's edges */
            float x = rdf(out + 0x10), y = rdf(out + 0x14);
            if (g_a > WIDE) {
                if (el >= 22) x += el == 22 ? g_x0 : -g_x0;
            } else if (el < 22)
                y += el == 20 ? g_y0 : -g_y0;
            else
                wrf(out + 0x54, v[1] * 16.0f / (g_a * 9.0f));
            wrf(out + 0x10, x);
            wrf(out + 0x14, y);
        }
    } else if ((el == 2 || el == 3) && g_a > WIDE && !strncmp(ui_name(lay), "hud04", 5) &&
               (!strncmp(ui_name(lay) + 5, "30", 2) || !strncmp(ui_name(lay) + 5, "50", 2))) {
        float k = g_a * 9.0f / 16.0f; /* the lines at the ends of the bands of hud0430 and hud0450 */
        wrf(out + 0x50, v[0] * k);
        wrf(out + 0x10, rdf(out + 0x10) * k);
    } else if ((el == 7 || el == 8) && g_y0 != 0.0f && !strncmp(ui_name(lay), "event0020", 9)) { /* its shutter */
        wrf(out + 0x14, rdf(out + 0x14) + (el == 7 ? g_y0 : -g_y0));
    } else if (el == 1 || el == 2) {
        nm = ui_name(lay);
        if (!strncmp(nm, "menu000b", 8)) {
            /* the chapter select's banner, the Shop's panels: a picture with a diagonal edge, scaled around a point of
               that edge (149 right of its centre) so the diagonal stays */
            float k = (380.0f - g_x0) / 410.0f, ky = (360.0f - g_y0) / 360.0f;
            if (ky > k) k = ky;
            if (k > 1.0f) {
                wrf(out + 0x50, v[0] * k);
                wrf(out + 0x54, v[1] * k);
                wrf(out + 0x10, rdf(out + 0x10) + 149.0f * (1.0f - k));
            }
        } else if (el == 1 && !strncmp(nm, "event0015", 9)) { /* the credits' ring of 5 pictures, 1280 apart */
            wrf(out + 0x50, v[0] * ui_k());
            wrf(out + 0x54, v[1] * ui_k());
        }
    }
    if (!(g_a > WIDE)) return;
    v[0] = rdf(out + 0x50);
    rule = kind == 2 ? ui_rule(lay, el) : kind == 6 ? ui_rule(lay, el + 1) : 0.0f;
    if (kind == 2 ? rule > 0.0f : rule > 1.0f) { /* the left end moves to the screen's, the right end stays */
        float rx = rdf(out + 0x10);
        if (kind == 6) {
            rx += g_x0;
            v[0] *= rule;
        } else {
            if (rule == 1.0f) rx += g_x0;
            v[0] -= g_x0 / sz[0];
        }
        wrf(out + 0x10, rx);
        wrf(out + 0x50, v[0]);
    }
    if (el != 0 || __atomic_load_n(&g_opt_hud_area, __ATOMIC_RELAXED)) return;
    if (!(ex0 >= 200.0f || ex0 <= -200.0f) || strncmp(ui_name(lay), "hud", 3)) return;
    wrf(out + 0x10, rdf(out + 0x10) + (ex0 > 0.0f ? -g_x0 : g_x0)); /* -g_x0 is half the extra width */
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
    /* the element's str d8, [sp, #-0x70]! and stp x29, x30, [sp, #0x10] */
    if (word_at(FN_UI_ELEMENT) == 0xfc190fe8u && word_at(FN_UI_ELEMENT + 4) == 0xa9017bfdu) {
        g_ui_element = take(FN_UI_ELEMENT, ui_element);
        LOG("aspect: the layouts follow the screen, the HUD %s (hud_area=%d)\n",
            g_opt_hud_area ? "in the middle" : "at its edges", g_opt_hud_area);
    } else
        LOG("aspect: the layouts' element isn't the one it knows, the HUD stays in the middle\n");
    LOG("aspect: the game's picture at %.3f (docked %dx900), the interface a 16:9 block in the middle\n", (double)a,
        even_width(900));
}
