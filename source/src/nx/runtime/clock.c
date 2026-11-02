/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock: real time since the run started, in nanoseconds and in the console's system ticks */
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/plat.h"

static u64 g_start; /* written once at startup, before any other thread */

void clock_start(void) { g_start = plat_ns(); }
u64 g_t0_ns(void) { return g_start; }
u64 guest_ns(void) { return plat_ns() - g_start; }

/* 19.2 MHz is 192 ticks every 10 us: whole 10 us steps, then the rest, so neither part overflows or rounds the way
   a double would after a few hours */
u64 guest_ticks(void) {
    u64 ns = guest_ns();
    return ns / 10000u * 192u + ns % 10000u * 192u / 10000u;
}
