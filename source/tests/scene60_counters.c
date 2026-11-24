/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The frame counters above 60 (scene60.c): which updates of a 90 Hz play count, and what each counter's hook does to
   the registers of the add or subtract it runs before */
#include "nx/hle/scene60.c"
#include <math.h>
#include <stdio.h>

u8* g_mem;
GuestFn* g_disp;
GuestFn snx_interp_entry;
u64 g_code_lo, g_code_hi;
unsigned g_vi_hz = VI_REFRESH_HZ;
int g_opt_frame_rate = 90, g_opt_cutscene_fps = 60;
u64 plat_ns(void) { return 0; }
int rt_hook_set(u64 pc, void (*fn)(CPU* RESTRICT c, u64 pc)) {
    (void)pc;
    (void)fn;
    return 1;
}
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
static unsigned checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "scene60_counters.c:%d: ", __LINE__);                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static int skips(int frames, f32 step) {
    int i, n = 0;
    for (i = 0; i < frames; i++) {
        count_update(step, 0);
        n += g_count_skip;
    }
    return n;
}

int main(void) {
    static CPU c;
    size_t i;
    /* 90 updates a second: 30 of them complete no 60 Hz frame. At 60 and below, none skip */
    g_play_owed = 0.0f;
    CHECK(skips(90, 60.0f / 90.0f) == 30, "skips at 90");
    CHECK(skips(120, 0.5f) == 60, "skips at 120");
    CHECK(skips(60, 1.0f) == 0 && skips(10, 1.7f) == 0, "skips at 60 and below");
    count_update(0.5f, 1);
    CHECK(!g_count_skip && g_count_part == 1.0f && g_play_owed == 0.0f, "fixed-step frames count as made");

    /* subs w8, w8, #1: skipped it leaves w8 as it was, at 0 it counts as made */
    c.x[8] = 5;
    CHECK(counter_adjust(&c, 0x71000508u, 1, 0.5f) && c.x[8] == 6, "countdown skipped: %llu",
          (unsigned long long)c.x[8]);
    c.x[8] = 5;
    CHECK(!counter_adjust(&c, 0x71000508u, 0, 0.5f) && c.x[8] == 5, "countdown not skipped");
    c.x[8] = 0;
    CHECK(!counter_adjust(&c, 0x71000508u, 1, 0.5f) && c.x[8] == 0, "countdown at 0");
    /* add w9, w9, #1: the up-counter of a held demon button */
    c.x[9] = 3;
    CHECK(counter_adjust(&c, 0x11000529u, 1, 0.5f) && c.x[9] == 2, "up-counter skipped");
    /* fadd s0, s0, s1 with s1 = -1.0: a float countdown skips; with -1/60 it takes the update's share */
    c.v[0].f[0] = 3.0f;
    c.v[1].f[0] = -1.0f;
    CHECK(counter_adjust(&c, 0x1e212800u, 1, 0.5f) && c.v[0].f[0] + c.v[1].f[0] == 3.0f, "float countdown skipped");
    c.v[0].f[0] = 3.0f;
    CHECK(!counter_adjust(&c, 0x1e212800u, 0, 0.5f) && c.v[0].f[0] == 3.0f, "float countdown not skipped");
    c.v[0].f[0] = 1.0f;
    c.v[1].f[0] = -1.0f / 60.0f;
    CHECK(counter_adjust(&c, 0x1e212800u, 0, 2.0f / 3.0f), "seconds take the share");
    CHECK(fabsf(c.v[0].f[0] + c.v[1].f[0] - (1.0f - 1.0f / 90.0f)) < 1e-6f, "seconds: %g", (double)c.v[0].f[0]);
    /* fsub s0, s0, s8 with s8 = 2.0 */
    c.v[0].f[0] = 10.0f;
    c.v[8].f[0] = 2.0f;
    CHECK(counter_adjust(&c, 0x1e283800u, 0, 0.5f) && c.v[0].f[0] - c.v[8].f[0] == 9.0f, "fsub's share");
    /* every word in the table is a form the hook knows */
    for (i = 0; i < COUNTERS; i++) {
        u32 w = k_counters[i][1], n = (w >> 5) & 31, m = (w >> 16) & 31;
        memset(&c, 0, sizeof c);
        c.x[n] = 5;
        c.v[n].f[0] = 5.0f;
        if (m != n) c.v[m].f[0] = -1.0f;
        CHECK(counter_adjust(&c, w, 1, 0.5f), "main+0x%x: %08x isn't a form the hook knows", k_counters[i][0], w);
    }
    /* a smoothing: three updates at 90 close in as far as two at 60; stock at 60 and for k outside (0, 1) */
    {
        f32 k = 0.3f, k90 = ease_rate(k, 60.0f / 90.0f), left60 = (1 - k) * (1 - k);
        f32 left90 = (1 - k90) * (1 - k90) * (1 - k90);
        CHECK(fabsf(left90 - left60) < 1e-5f, "ease at 90: %g left, %g at 60", (double)left90, (double)left60);
        CHECK(ease_rate(k, 1.0f) == k && ease_rate(1.0f, 0.5f) == 1.0f && ease_rate(0.0f, 0.5f) == 0.0f, "ease stock");
    }
    /* the chapter select's camera eases only in an update the map ran in, or the next */
    {
        g_main = 0;
        g_ease_step = 0.5f;
        g_updates = 10;
        memset(&c, 0, sizeof c);
        c.v[0].f[0] = 0.75f;
        ease_hook(&c, EASE_CAMERA);
        CHECK(c.v[0].f[0] == 0.75f, "the camera eased without the map");
        ease_hook(&c, EASE_MAP);
        g_updates = 11;
        ease_hook(&c, EASE_CAMERA);
        CHECK(c.v[0].f[0] == 0.5f, "the camera's rate %g, not 0.5", (double)c.v[0].f[0]);
        g_updates = 12;
        c.v[0].f[0] = 0.75f;
        ease_hook(&c, EASE_CAMERA);
        CHECK(c.v[0].f[0] == 0.75f, "the camera eased two updates after the map");
        c.v[1].f[0] = 0.75f;
        ease_hook(&c, EASE_RING);
        CHECK(c.v[1].f[0] == 0.5f, "the ring's rate %g, not 0.5", (double)c.v[1].f[0]);
    }
    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
