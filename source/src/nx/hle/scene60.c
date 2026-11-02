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
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#include <string.h>

#define MAIN_FROM_LOW 0x5000   /* main loads right after rtld */
#define LOOP_MODE_SET 0x670c4c /* the main loop's calls of the functions below, checked before anything is replaced */
#define LOOP_CLOCK 0x670c24
#define LOOP_STEP_KIND 0x670d18
#define LOOP_STEP_END 0x670d88
#define FN_MODE_SET 0xd02de0   /* (mode w0): 0 = 60, 1 = 30, 2 = 15 */
#define FN_CLOCK 0xc1f600      /* (clock x0, step s0, measured w1) */
#define FN_STEP_KIND 0x67037c  /* comes just after the first kind's flag 27 is set in the word at x19 */
#define FN_STEP_END 0xb20a20   /* ends a logic step */
#define FN_MOVIE_FLAG 0x7077d0 /* (w0): sets or clears flag 28 */
#define SCENE_START 0xbb1bf4   /* its two callers */
#define SCENE_END 0xbb2a6c
#define STATE_PTR 0x249c388    /* the pointer to the game's state flags */
#define SCENES_PTR 0x24aae68   /* the pointer to its eight scene slots */
#define CLOCK_STEP 0xe0        /* in a clock, f32: this frame's step */
#define CLOCK_MS 0xf0          /* f32: the last frame's length */
#define FLAG_RIDE (1u << 16)   /* a ride set piece: its prompts need steps of exactly 1.0 */
#define FLAG_SEEK (1u << 21)   /* the scene player repeats the second step until it reaches a frame */
#define FLAG_FIRST (1u << 27)  /* this step is the first kind */
#define PASS_TWO (1ull << 28)  /* in the main loop's x21: run the second step of a fixed-step frame */
#define HELD 0x4001u           /* a load or a pause menu: the scene's clock stands */
#define MOVIE_FLAGS 0x50000000u
#define MOVIE_MISSING 0x40000000u
#define OWED_MAX 90.0f /* 30 Hz frames: a window held for minutes must not fast-forward the scene for minutes */

static u64 g_main;
static GuestFn g_mode_set, g_clock, g_step_kind, g_step_end;
static int g_one_step; /* this fixed-step frame runs one logic step */
static int g_half;     /* the last frame ran the first kind alone, so this one is the second */
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

static void clock_update(CPU* RESTRICT c, u64 pc) {
    static f32 owed;
    static int was_fixed;
    u64 clock = c->x[0];
    int fixed = (u32)c->x[1] == 0;
    keep_movie_flag(c);
    g_one_step = fixed && c->v[0].f[0] == 1.0f; /* s0 is 4.0 in the game's 15 FPS mode */
    g_clock(c, pc);
    if (fixed) {
        u32 flags = state_flags();
        f32 step, ms, due, even = g_one_step ? 0.5f : 1.0f, k;
        memcpy(&step, GP(clock + CLOCK_STEP), sizeof step);
        memcpy(&ms, GP(clock + CLOCK_MS), sizeof ms);
        due = ms > 0.0f && ms == ms ? ms / (1000.0f / 30.0f) : even;
        if (!was_fixed || (flags & HELD)) {
            owed = 0.0f;
            due = even;
        }
        k = flags & FLAG_RIDE ? ride_step(&owed, due, even) : scene_step(&owed, due, even);
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
    if (g_opt_cutscene_fps != 60) return;
    g_main = g_code_lo + MAIN_FROM_LOW;
    if (!g_disp || g_main + LOOP_MODE_SET >= g_code_hi || !calls(LOOP_CLOCK, FN_CLOCK) ||
        !calls(LOOP_MODE_SET, FN_MODE_SET) || !calls(LOOP_STEP_KIND, FN_STEP_KIND) ||
        !calls(LOOP_STEP_END, FN_STEP_END) || !calls(SCENE_START, FN_MOVIE_FLAG) || !calls(SCENE_END, FN_MOVIE_FLAG)) {
        LOG("scene60: this isn't the game it knows, cutscenes stay as they are\n");
        return;
    }
    g_mode_set = take(FN_MODE_SET, mode_set);
    g_clock = take(FN_CLOCK, clock_update);
    g_step_kind = take(FN_STEP_KIND, step_kind);
    g_step_end = take(FN_STEP_END, step_end);
    LOG("scene60: title, menus and cutscenes at 60, main at %llx\n", (unsigned long long)g_main);
}
