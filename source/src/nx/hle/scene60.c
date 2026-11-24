/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bayonetta 3 v1.2.0's title, menus and cutscenes at 60 FPS. The game's main loop picks a frame-rate mode each frame and
   such a scene asks for 30. Its frames then run two logic steps of a fixed 1.0, of two kinds: the first recomputes the
   timeline, the second latches what was asked for (a skip, a branch, the next act). Asking for 60 alone would run those
   scenes twice as fast, so at 60 a frame runs one step of the same size, the kinds take turns, and the step follows
   real time, because the scene's long sounds never wait for a late frame.

   The addresses and the mechanism were found by Witch Time (github.com/Zexyen/Witch-Time, GPL-3.0, docs/STATUS.md, "60
   fps menus and cutscenes"). This code is written from that description, and every call site is checked below.

   The direct calls to these functions go through g_disp only in a game module compiled with their addresses in the
   game's .tablecalls file. With an older module the handlers don't run, and the game stays at its own 30 */
#include "nx/hle/scene60.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/services/vi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/rtapi.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#include <math.h>
#include <string.h>

#define MAIN_FROM_LOW  0x5000   /* main loads right after rtld */
#define LOOP_MODE_SET  0x670c4c /* the main loop's calls of the functions below, checked before anything is replaced */
#define LOOP_CLOCK     0x670c24
#define LOOP_STEP_KIND 0x670d18
#define LOOP_STEP_END  0x670d88
#define FN_MODE_SET    0xd02de0 /* (mode w0): 0 = 60, 1 = 30, 2 = 15 */
#define FN_CLOCK       0xc1f600 /* (clock x0, step s0, measured w1) */
#define FN_STEP_KIND   0x67037c /* comes just after the first kind's flag 27 is set in the word at x19 */
#define FN_STEP_END    0xb20a20 /* ends a logic step */
#define FN_MOVIE_FLAG  0x7077d0 /* (w0): sets or clears flag 28 */
#define SCENE_START    0xbb1bf4 /* its two callers */
#define SCENE_END      0xbb2a6c
#define STATE_PTR      0x249c388    /* the pointer to the game's state flags */
#define SCENES_PTR     0x24aae68    /* the pointer to its eight scene slots */
#define CLOCK_STEP     0xe0         /* in a clock, f32: this frame's step */
#define CLOCK_CARRY    0xe4         /* f32: what a step raised to the floor took ahead, taken off the next */
#define CLOCK_SUM      0xe8         /* f32: measured frames' lengths summed */
#define CLOCK_FRAME_MS 0xec         /* f32: the length of a step of 1.0 */
#define CLOCK_MS       0xf0         /* f32: the last frame's length */
#define CLOCK_COUNT    0xf4         /* i32: the measured frames summed */
#define FLAG_RIDE      (1u << 16)   /* a ride set piece: its prompts need steps of exactly 1.0 */
#define FLAG_SEEK      (1u << 21)   /* the scene player repeats the second step until it reaches a frame */
#define FLAG_FIRST     (1u << 27)   /* this step is the first kind */
#define PASS_TWO       (1ull << 28) /* in the main loop's x21: run the second step of a fixed-step frame */
#define HELD           0x4001u      /* a load or a pause menu: the scene's clock stands */
#define MOVIE_FLAGS    0x50000000u
#define MOVIE_MISSING  0x40000000u
#define OWED_MAX       90.0f /* 30 Hz frames: a window held for minutes must not fast-forward the scene for minutes */

/* Frame counters above 60. Play counts input windows, press buffers, hit-stops, Witch Time's slow-downs and rumble in
   updates: a load, an add or subtract of a constant, a store, made for 60 updates a second. The sites are Witch Time's
   (github.com/Zexyen/Witch-Time, GPL-3.0: tools/wtbuild/fps_counters.txt, found by their census and read one by one,
   with the rules of runtime/game_shim_eh.c fps_ci, fps_cf and fps_fs). Each entry is the add or subtract (main
   offset, its word), and a hook point runs before it (launcher/code-insnhooks.txt). On an update that completes no
   60 Hz frame of play the hook raises the loaded value by the step, so the field stays and the code after it reads
   what a field one step larger would leave: integer counters and floats counting 1.0 skip that update, other floats
   take the update's fraction of their amount. A countdown at 0 takes the stock step */
static const u32 k_counters[][2] = {
    {0x81425c, 0x1e212800u}, {0x627d10, 0x1e212800u}, {0x6274ac, 0x1e212800u},
    {0x773974, 0x1e202820u}, {0x084320, 0x1e212800u}, {0xc8a7c4, 0x1e212800u},
    {0x64cff0, 0x1e242863u}, {0x55a838, 0x51000529u}, {0x55a8d0, 0x71000430u},
    {0x55a8e4, 0x71000450u}, {0x55a414, 0x71000508u}, {0x55a808, 0x71000509u},
    {0x55a81c, 0x71000549u}, {0x55a844, 0x7100058bu}, {0x55a858, 0x7100056du},
    {0x55a86c, 0x710005aeu}, {0x55a880, 0x710005cfu}, {0x55a894, 0x710005f0u},
    {0x55a8a8, 0x71000630u}, {0x55a8bc, 0x71000650u}, {0x55a8f8, 0x71000470u},
    {0x55a90c, 0x71000490u}, {0x55a920, 0x710004b0u}, {0x55a934, 0x710004d0u},
    {0x55ae90, 0x51000508u}, {0x55ac44, 0x71000508u}, {0x55ac54, 0x71000508u},
    {0x55af04, 0x51000508u}, {0x55ac64, 0x71000508u}, {0x55ac74, 0x71000508u},
    {0x55ac84, 0x71000508u}, {0x55acbc, 0x71000508u}, {0x55ac94, 0x71000508u},
    {0x55aca4, 0x71000508u}, {0x55afa4, 0x1e212800u}, {0x55b014, 0x71000508u},
    {0x55b03c, 0x71000508u}, {0x55b2c8, 0x71000509u}, {0x55b2dc, 0x71000529u},
    {0x55b938, 0x11000529u}, {0x55b180, 0x71000508u}, {0x43574c, 0x71000508u},
    {0x7a233c, 0x71000508u}, {0x7a238c, 0x71000508u}, {0x7a25a4, 0x71000508u},
    {0x7a25f4, 0x71000508u}, {0x7a25b4, 0x71000508u}, {0x7a2608, 0x71000508u},
    {0x7a2594, 0x71000508u}, {0x7a2584, 0x71000508u}, {0xc1f900, 0x710005efu},
    {0xcc3768, 0x7100058du}, {0xcc37e8, 0x7100058du}, {0x340788, 0x1e212800u},
    {0x422f74, 0x71000508u}, {0x43e5fc, 0x1e212800u}, {0x43e93c, 0x1e212800u},
    {0x79bd80, 0x1e212800u}, {0x7a01d0, 0x1e212800u}, {0x086e00, 0x1e283800u},
    {0x0dc688, 0x71000529u}, {0x0dd0c8, 0x51000508u}, {0x3b5828, 0x1e212800u},
    {0x6e1c24, 0x7100058cu}, {0x780a10, 0x71000508u}, {0x814208, 0x71000508u},
    {0x814224, 0x1e212800u}, {0x9202bc, 0x1e222800u}}; /* ours: the chapter select's snap, t += 0.2 */
#define COUNTERS (sizeof k_counters / sizeof *k_counters)

/* Smoothings x += k (T - x) an update close in 1.5x as fast at 90. Before each, k becomes 1 - (1 - k)^step, what updates
   of this length add up to in a 60 Hz frame's time. Witch Time's (fps_ease, lifter/eh.rs FPS_EASE_SITES, measured on
   the chapter select at 120: a hop's glide back to 0.25 s to 90 %). The camera manager's easing serves every camera,
   whose rates aren't read, so it is eased only while the chapter select runs (its update this update or the last) */
#define EASE_RING   0x91f1c8 /* fmla v0.4s, v3.4s, v1.s[0]: the chapter select's ring eases to the aim, k in s1 */
#define EASE_MAP    0x91dba8 /* stp x29, x30, [sp, #-0x50]!: the chapter select's update starts */
#define EASE_CAMERA 0x626404 /* add x8, x19, #0x1f0: right after the camera manager loads its rate into s0 */
static const u32 k_eases[][2] = {{EASE_RING, 0x4f811060u}, {EASE_MAP, 0xa9bb7bfdu}, {EASE_CAMERA, 0x9107c268u}};

static u64 g_main;
static GuestFn g_mode_set, g_clock, g_step_kind, g_step_end;
static f32 g_play_owed;            /* 60 Hz frames of play not yet counted, under 1 */
static int g_count_skip;           /* this update completes no 60 Hz frame: counters stay. Atomic, read on any thread */
static f32 g_count_part = 1;       /* this update's 60 Hz frames, at most 1: the share of a float's amount */
static f32 g_ease_step = 1;        /* this update's 60 Hz frames, the power of a smoothing's 1 - k */
static u64 g_updates;              /* the updates the main loop's clock started */
static u64 g_map_update = (u64)-2; /* the last update the chapter select ran in */
static int g_one_step;             /* this fixed-step frame runs one logic step */
static int g_half;                 /* the last frame ran the first kind alone, so this one is the second */
static u64 g_frames_forced;

static GuestFn take(u64 offset, GuestFn host) {
    GuestFn* slot = &g_disp[(g_main + offset - g_code_lo) / 4];
    GuestFn guest = *slot ? *slot : snx_interp_entry;
    *slot = host;
    return guest;
}

static u32 state_flags(void) {
    u64 p = 0;
    u32 f = 0;
    memcpy(&p, GP(g_main + STATE_PTR), sizeof p);
    if (p) memcpy(&f, GP(p), sizeof f);
    return f;
}

static void mode_set(CPU* RESTRICT c, u64 pc) {
    static int last = -1;
    int want = (int)(u32)c->x[0];
    if (want != last) {
        LOG("scene60: the game asks for mode %d at frames forced %llu\n", want, (unsigned long long)g_frames_forced);
        last = want;
    }
    if (want == 1) {
        c->x[0] = 0;
        g_frames_forced++;
    }
    g_mode_set(c, pc);
}

/* a plain scene's step, in 30 Hz frames. Even while on time, so jitter in a frame's length can't move the picture. Past
   one even step owed half of the rest goes each frame: a steady 30 FPS leaves about 50 ms behind the sound */
static f32 scene_step(f32* owed, f32 due, f32 even) {
    f32 o = *owed + due - even, k = even;
    if (o > OWED_MAX) o = OWED_MAX;
    if (o > even)
        k += (o - even) / 2.0f;
    else if (o < -even)
        k += (o + even) / 2.0f;
    if (k > 2.0f) k = 2.0f;
    *owed = o + even - k;
    return k;
}

/* a ride's step never changes. A whole even step owed runs both logic steps of a console frame, where the turns allow */
static f32 ride_step(f32* owed, f32 due, f32 even) {
    f32 o;
    if (g_one_step && !g_half && *owed + due - even >= even) {
        g_one_step = 0;
        even *= 2.0f;
    }
    o = *owed + due - even;
    *owed = o < -even ? -even : o > OWED_MAX ? OWED_MAX : o;
    return even;
}

/* a movie scene's frame is the movie's times 2 only with flags 28 and 30, so with 28 lost its subtitles run at half
   speed. Runs the game's own setter, keeping the registers of the call it interrupts */
static void keep_movie_flag(CPU* RESTRICT c) {
    u64 slots = 0, saved_x[31], saved_pc = c->pc;
    V128 saved_v[8];
    int i;
    memcpy(&slots, GP(g_main + SCENES_PTR), sizeof slots);
    for (i = 0; slots && i < 8; i++) {
        u64 slot = slots + 112 * (u64)i, scene = 0, header = 0;
        u32 on = 0, movie = 0;
        signed char fps = 0, base = 0;
        memcpy(&on, GP(slot), sizeof on);
        memcpy(&scene, GP(slot + 56), sizeof scene);
        if (!(on & 1) || !scene) continue;
        memcpy(&movie, GP(scene + 168), sizeof movie);
        memcpy(&header, GP(scene + 8), sizeof header);
        memcpy(&fps, GP(header + 144), 1);
        memcpy(&base, GP(header + 145), 1);
        if (!movie || !base || fps / base != 2 || (state_flags() & MOVIE_FLAGS) != MOVIE_MISSING) continue;
        memcpy(saved_x, c->x, sizeof saved_x);
        memcpy(saved_v, c->v, sizeof saved_v);
        c->x[0] = 1;
        c->x[30] = g_main + LOOP_CLOCK + 4;
        snx_lookup(c, g_main + FN_MOVIE_FLAG)(c, g_main + FN_MOVIE_FLAG);
        memcpy(c->x, saved_x, sizeof saved_x);
        memcpy(c->v, saved_v, sizeof saved_v);
        c->pc = saved_pc;
        LOG("scene60: movie scene %d had lost flag 28, set again\n", i);
        return;
    }
}

static f32 load_f32(u64 a) {
    f32 v;
    memcpy(&v, GP(a), sizeof v);
    return v;
}
static void store_f32(u64 a, f32 v) { memcpy(GP(a), &v, sizeof v); }

/* A measured frame's step: the game averages the frames measured since its last step, in steps of CLOCK_FRAME_MS
   (16.7 ms, a 60 Hz frame, if the 1.0 .. 2.0 clamp means what it seems), takes off what the step before took ahead, and keeps it in 1.0 .. 2.0. A step under 1.0 becomes 1.0,
   and the difference is carried to the next, so above 60 FPS the game would run faster than real time. Above 60 the
   floor is 60 / frame_rate. The game's own result is recomputed first, in its order and in f32, so a step it took from
   elsewhere (its call at main+0xba3300, that path leaves the carry alone) is kept */
static void measured_floor(u64 clock, f32 carry, f32 sum, i32 count) {
    f32 raw = (sum + load_f32(clock + CLOCK_MS)) / (f32)(count + 1) / load_f32(clock + CLOCK_FRAME_MS) - carry;
    f32 r = raw < 0.0f ? 0.0f : raw, floor = 60.0f / (f32)g_vi_hz;
    f32 game_step = r < 1.0f ? 1.0f : r > 2.0f ? 2.0f : r, game_carry = r < 1.0f ? 1.0f - r : 0.0f;
    if (load_f32(clock + CLOCK_STEP) != game_step || load_f32(clock + CLOCK_CARRY) != game_carry) {
        WARN_LIMIT(1, "scene60: a measured step of %g isn't the average's %g: kept\n",
                   (double)load_f32(clock + CLOCK_STEP), (double)game_step);
        return;
    }
    if (r < 1.0f) LOG_LIMIT(1, "scene60: play's step is under 1.0 (%g) at %u Hz\n", (double)r, g_vi_hz);
    store_f32(clock + CLOCK_STEP, r < floor ? floor : r > 2.0f ? 2.0f : r);
    store_f32(clock + CLOCK_CARRY, r < floor ? floor - r : 0.0f);
}

/* every 600 fixed-step frames, the scene's time against real time: the check that a scene plays in real time at
   any frame rate. A held or new scene starts the count again */
static void scene_clock_report(f32 k, int restart) {
    static u64 t0, n;
    static double scene_s;
    u64 now = plat_ns();
    if (restart || !t0) {
        t0 = now;
        n = 0;
        scene_s = 0;
        return;
    }
    scene_s += (double)k / 30.0;
    if (++n < 600) return;
    LOG("scene60: a scene ran %.3f s of its time in %.3f s, %llu frames at %u Hz\n", scene_s, (double)(now - t0) / 1e9,
        (unsigned long long)n, g_vi_hz);
    t0 = now;
    n = 0;
    scene_s = 0;
}

/* the update the main loop's clock call starts: what the counters do in it. Fixed-step scenes keep the stock counts */
static void count_update(f32 step, int fixed) {
    int whole = 1;
    f32 ease;
    if (fixed || !(step > 0.0f)) {
        g_play_owed = 0.0f;
        step = 1.0f;
    } else {
        g_play_owed += step;
        if (g_play_owed > 4.0f) g_play_owed = 4.0f;
        whole = (int)g_play_owed;
        g_play_owed -= (f32)whole;
    }
    ease = step;
    if (step > 1.0f) step = 1.0f;
    __atomic_store_n(&g_count_skip, whole == 0, __ATOMIC_RELAXED);
    __atomic_store(&g_count_part, &step, __ATOMIC_RELAXED);
    __atomic_store(&g_ease_step, &ease, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_updates, 1, __ATOMIC_RELAXED);
}

/* a smoothing's rate for this update's step (1 - k)^step: unchanged at 1.0 */
static f32 ease_rate(f32 k, f32 step) { return step != 1.0f && k > 0.0f && k < 1.0f ? 1.0f - powf(1.0f - k, step) : k; }

static void ease(CPU* RESTRICT c, int r) {
    f32 step;
    __atomic_load(&g_ease_step, &step, __ATOMIC_RELAXED);
    c->v[r].f[0] = ease_rate(c->v[r].f[0], step);
}

static void ease_hook(CPU* RESTRICT c, u64 pc) {
    u64 n = __atomic_load_n(&g_updates, __ATOMIC_RELAXED);
    if (pc == g_main + EASE_RING)
        ease(c, 1);
    else if (pc == g_main + EASE_MAP)
        __atomic_store_n(&g_map_update, n, __ATOMIC_RELAXED);
    else if (n - __atomic_load_n(&g_map_update, __ATOMIC_RELAXED) <=
             1) /* the camera's and the map's order isn't known */
        ease(c, 0);
}

/* the hook's change to the registers before the add or subtract w (1 when it made one) */
static int counter_adjust(CPU* RESTRICT c, u32 w, int skip, f32 part) {
    u32 n = (w >> 5) & 31;
    if ((w & 0x9fc00000u) == 0x11000000u) { /* add or subtract (immediate), 32-bit, unshifted */
        u32 v = (u32)c->x[n], k = (w >> 10) & 0xfffu, sub = (w >> 30) & 1u;
        if (!skip || (sub && v == 0)) return 0;
        c->x[n] = (u64)(sub ? v + k : v - k);
        return 1;
    }
    if ((w & 0xffe0ec00u) == 0x1e202800u) { /* fadd or fsub, single */
        f32 a = c->v[(w >> 16) & 31].f[0], share = a == 1.0f || a == -1.0f ? (skip ? 0.0f : 1.0f) : part;
        if (share == 1.0f) return 0;
        c->v[n].f[0] += ((w >> 12) & 1u ? -a : a) * (share - 1.0f);
        return 1;
    }
    return 0;
}

static void counter(CPU* RESTRICT c, u64 pc) {
    u32 w;
    f32 part;
    memcpy(&w, GP(pc), sizeof w);
    __atomic_load(&g_count_part, &part, __ATOMIC_RELAXED);
    (void)counter_adjust(c, w, __atomic_load_n(&g_count_skip, __ATOMIC_RELAXED), part);
}

static void clock_update(CPU* RESTRICT c, u64 pc) {
    static f32 owed;
    static int was_fixed;
    u64 clock = c->x[0];
    int fixed = (u32)c->x[1] == 0, loop = c->x[30] == g_main + LOOP_CLOCK + 4;
    f32 carry = load_f32(clock + CLOCK_CARRY), sum = load_f32(clock + CLOCK_SUM);
    i32 count;
    memcpy(&count, GP(clock + CLOCK_COUNT), sizeof count);
    keep_movie_flag(c);
    g_one_step = fixed && c->v[0].f[0] == 1.0f; /* s0 is 4.0 in the game's 15 FPS mode */
    g_clock(c, pc);
    if (!fixed && g_vi_hz > VI_REFRESH_HZ) measured_floor(clock, carry, sum, count);
    if (loop && g_vi_hz > VI_REFRESH_HZ) count_update(load_f32(clock + CLOCK_STEP), fixed);
    if (fixed) {
        u32 flags = state_flags();
        /* in 30 Hz frames: a display frame at g_vi_hz is 30 / g_vi_hz of one */
        f32 step, ms, due, even = (g_one_step ? 0.5f : 1.0f) * (f32)VI_REFRESH_HZ / (f32)g_vi_hz, k;
        memcpy(&step, GP(clock + CLOCK_STEP), sizeof step);
        memcpy(&ms, GP(clock + CLOCK_MS), sizeof ms);
        due = ms > 0.0f && ms == ms ? ms / (1000.0f / 30.0f) : even;
        if (!was_fixed || (flags & HELD)) {
            owed = 0.0f;
            due = even;
        }
        k = flags & FLAG_RIDE ? ride_step(&owed, due, even) : scene_step(&owed, due, even);
        scene_clock_report(k, !was_fixed || (flags & HELD));
        step *= g_one_step ? 2.0f * k : k; /* one logic step: it is 2 of the 1/60 s ones of a 30 Hz frame */
        memcpy(GP(clock + CLOCK_STEP), &step, sizeof step);
    }
    was_fixed = fixed;
}

static void step_kind(CPU* RESTRICT c, u64 pc) {
    int second = g_one_step && g_half;
    g_half = g_one_step && !g_half;
    if (second) {
        u32 flags = state_flags() & ~FLAG_FIRST;
        memcpy(GP(c->x[19]), &flags, sizeof flags);
    }
    g_step_kind(c, pc);
}

static void step_end(CPU* RESTRICT c, u64 pc) {
    int first = c->x[30] == g_main + LOOP_STEP_END + 4;
    g_step_end(c, pc);
    if (!first || !g_one_step) return;
    if (state_flags() & FLAG_SEEK)
        g_half = 0; /* the second step repeats, and the console's next frame starts with the first kind */
    else
        c->x[21] &= ~PASS_TWO;
}

/* every counter's word where it's known, then its hook point */
static void counters_init(void) {
    size_t i, set = 0;
    for (i = 0; i < COUNTERS; i++) {
        u32 w = 0;
        memcpy(&w, GP(g_main + k_counters[i][0]), sizeof w);
        if (w != k_counters[i][1]) {
            LOG("scene60: main+0x%x isn't the counter it knows: play's windows and timers stay counted in frames\n",
                k_counters[i][0]);
            return;
        }
    }
    for (i = 0; i < COUNTERS; i++) set += (size_t)rt_hook_set(g_main + k_counters[i][0], counter);
    LOG("scene60: %u of %u frame counters count play's time\n", (unsigned)set, (unsigned)COUNTERS);
    for (i = 0; i < sizeof k_eases / sizeof *k_eases; i++) {
        u32 w = 0;
        memcpy(&w, GP(g_main + k_eases[i][0]), sizeof w);
        if (w != k_eases[i][1]) {
            LOG("scene60: main+0x%x isn't the smoothing it knows: the chapter select eases by frames\n", k_eases[i][0]);
            return;
        }
    }
    for (set = 0, i = 0; i < sizeof k_eases / sizeof *k_eases; i++)
        set += (size_t)rt_hook_set(g_main + k_eases[i][0], ease_hook);
    LOG("scene60: %u of 3 smoothings of the chapter select follow play's time\n", (unsigned)set);
}

/* does the word at offset call target? */
static int calls(u64 offset, u64 target) {
    u32 word = 0;
    u64 to;
    memcpy(&word, GP(g_main + offset), sizeof word);
    if ((word & 0xfc000000u) != 0x94000000u) return 0;
    to = offset + ((u64)(i64)((i32)(word << 6) >> 6) << 2);
    return to == target;
}

void scene60_init(void) {
    /* above 60 the scenes need their step at the display's rate too: as 30, two fixed steps a frame, they'd run fast */
    if (g_opt_cutscene_fps != 60 && g_opt_frame_rate <= VI_REFRESH_HZ) return;
    g_main = g_code_lo + MAIN_FROM_LOW;
    if (!g_disp || g_main + LOOP_MODE_SET >= g_code_hi || !calls(LOOP_CLOCK, FN_CLOCK) ||
        !calls(LOOP_MODE_SET, FN_MODE_SET) || !calls(LOOP_STEP_KIND, FN_STEP_KIND) ||
        !calls(LOOP_STEP_END, FN_STEP_END) || !calls(SCENE_START, FN_MOVIE_FLAG) || !calls(SCENE_END, FN_MOVIE_FLAG)) {
        LOG("scene60: this isn't the game it knows, cutscenes stay as they are%s\n",
            g_opt_frame_rate > VI_REFRESH_HZ ? ", and frame_rate is 60" : "");
        return;
    }
    g_mode_set = take(FN_MODE_SET, mode_set);
    g_clock = take(FN_CLOCK, clock_update);
    g_step_kind = take(FN_STEP_KIND, step_kind);
    g_step_end = take(FN_STEP_END, step_end);
    if (g_opt_frame_rate > VI_REFRESH_HZ) {
        g_vi_hz = (unsigned)g_opt_frame_rate;
        counters_init();
    }
    LOG("scene60: title, menus, cutscenes%s at %u, main at %llx\n", g_vi_hz > VI_REFRESH_HZ ? " and play" : "", g_vi_hz,
        (unsigned long long)g_main);
}
