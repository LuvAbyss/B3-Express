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
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

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
/* The demon form's double tap: after a dodge, a window of updates at [player + 0x3bc] in which a second ZR transforms.
   The update calls the input decoder, then counts the window down by one (main+0x55a410 .. 0x55a41c): updates, not
   time, so above 60 it closes early (Witch Time, docs/STATUS.md, found it at 120: main+0x55a41c) */
#define LOOP_DECODER   0x55a40c /* the update's call of the decoder; it is its only caller */
#define FN_DECODER     0x55b058 /* the input decoder (player x0) */
#define TAP_COUNT      0x55a410 /* ldr w8, [x19, #0x3bc]; subs w8, w8, #1; b.lt; str w8, [x19, #0x3bc] */
#define TAP_WINDOW     0x3bc    /* i32 in the player */
#define TAP_PLAYERS    4

static u64 g_main;
static GuestFn g_mode_set, g_clock, g_step_kind, g_step_end, g_decoder;
static f32 g_play_step = 1.0f; /* the last measured frame's step, in 60 Hz frames */
static int g_one_step;         /* this fixed-step frame runs one logic step */
static int g_half;             /* the last frame ran the first kind alone, so this one is the second */
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

static void clock_update(CPU* RESTRICT c, u64 pc) {
    static f32 owed;
    static int was_fixed;
    u64 clock = c->x[0];
    int fixed = (u32)c->x[1] == 0;
    f32 carry = load_f32(clock + CLOCK_CARRY), sum = load_f32(clock + CLOCK_SUM);
    i32 count;
    memcpy(&count, GP(clock + CLOCK_COUNT), sizeof count);
    keep_movie_flag(c);
    g_one_step = fixed && c->v[0].f[0] == 1.0f; /* s0 is 4.0 in the game's 15 FPS mode */
    g_clock(c, pc);
    if (!fixed && g_vi_hz > VI_REFRESH_HZ) {
        measured_floor(clock, carry, sum, count);
        g_play_step = load_f32(clock + CLOCK_STEP);
    }
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

/* The game counts the window down by 1 right after this returns, when it is 1 or more. Above 60 the count takes the
   whole 60 Hz frames of game time due instead: the step adds up per player, and before the game's 1 the window gets
   back what it shouldn't lose. As Witch Time's integer counters (B2's rules): never past 0 in one update */
static void decoder(CPU* RESTRICT c, u64 pc) {
    static struct {
        u64 player;
        f32 owed;
    } due[TAP_PLAYERS];
    u64 player = c->x[0];
    i32 n, whole;
    int i;
    g_decoder(c, pc);
    if (g_vi_hz <= VI_REFRESH_HZ || !player) return;
    for (i = 0; i < TAP_PLAYERS - 1 && due[i].player && due[i].player != player; i++) {}
    if (due[i].player != player) {
        due[i].player = player;
        due[i].owed = 0.0f;
    }
    due[i].owed += g_play_step;
    if (due[i].owed > 4.0f) due[i].owed = 4.0f;
    whole = (i32)due[i].owed;
    due[i].owed -= (f32)whole;
    memcpy(&n, GP(player + TAP_WINDOW), sizeof n);
    if (n < 1) return;
    LOG_LIMIT(1, "scene60: the double tap's window (%d) counts game time at %u Hz\n", n, g_vi_hz);
    n = n - (whole < n ? whole : n) + 1; /* the game's 1 comes next */
    memcpy(GP(player + TAP_WINDOW), &n, sizeof n);
}

/* do the words at offset read as these? */
static int words(u64 offset, const u32* want, int n) {
    int i;
    for (i = 0; i < n; i++) {
        u32 w = 0;
        memcpy(&w, GP(g_main + offset + 4 * (u64)i), sizeof w);
        if (w != want[i]) return 0;
    }
    return 1;
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
        static const u32 count[4] = {0xb943be68u, 0x71000508u, 0x5400004bu, 0xb903be68u};
        g_vi_hz = (unsigned)g_opt_frame_rate;
        if (calls(LOOP_DECODER, FN_DECODER) && words(TAP_COUNT, count, 4))
            g_decoder = take(FN_DECODER, decoder);
        else
            LOG("scene60: the double tap's window isn't where it's known: it stays counted in frames\n");
    }
    LOG("scene60: title, menus, cutscenes%s at %u, main at %llx\n", g_vi_hz > VI_REFRESH_HZ ? " and play" : "", g_vi_hz,
        (unsigned long long)g_main);
}
