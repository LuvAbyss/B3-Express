/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bayonetta 3 v1.2.0's models at their finest level of detail (model_lod=1).

   main+0xbd563c picks a mesh's level of detail for the camera's distance and writes it to *x1: -2 none, -1 hidden,
   0 the finest, higher coarser. Its one caller (main+0xbd5b3c, the call at main+0xbd5cfc) runs the cross-fade
   between levels. After the game's own choice, a coarser level becomes the finest, and a mesh hidden for distance is
   drawn at its finest when it has more than one level (x2 and the mesh's last level at +0x14 bound what the call may
   pick). A mesh with a single level keeps the game's distance cut, so far props still go.

   The function and the rule are Witch Time's (github.com/Zexyen/Witch-Time, runtime/game_shim_eh.c h_model_lod,
   GPL-3.0, issue 86), from the Switch "Increased LOD" mod (mov w8, #0 at main+0xbd5b2c). Their numbers on an
   RX 9070 XT: 3.7x the indices a frame, about 0.2 ms more GPU time, frame times unchanged at 60.

   Like scene60.c, the direct calls go through g_disp only in a game module compiled with the address in its
   .tablecalls file: with an older module the handler doesn't run and models stay as the game picks them */
#include "nx/hle/model_lod.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#include <string.h>

#define MAIN_FROM_LOW 0x5000   /* main loads right after rtld */
#define FN_SELECT     0xbd563c /* (mesh x0, level out x1, last level wanted w2, ...) */
#define CALL_SELECT   0xbd5cfc /* its one caller's call, checked before anything is replaced */
#define MESH_LAST     0x14     /* in the mesh, i32: its last level */

static u64 g_main;
static GuestFn g_select;
static u64 g_raised;

static void select_level(CPU* RESTRICT c, u64 pc) {
    u64 out = c->x[1];
    i32 last = 0, levels, level;
    memcpy(&last, GP(c->x[0] + MESH_LAST), sizeof last);
    levels = (i32)c->x[2] < last ? (i32)c->x[2] : last; /* the last level this call may pick */
    g_select(c, pc);
    memcpy(&level, GP(out), sizeof level);
    if (level > 0 || (level == -1 && levels > 0)) {
        level = 0;
        memcpy(GP(out), &level, sizeof level);
        if (!__atomic_fetch_add(&g_raised, 1, __ATOMIC_RELAXED))
            LOG("model_lod: a model drawn at its finest level instead of the game's\n");
    }
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

void model_lod_init(void) {
    GuestFn* slot;
    if (!g_opt_model_lod) return;
    g_main = g_code_lo + MAIN_FROM_LOW;
    if (!g_disp || g_main + CALL_SELECT >= g_code_hi || !calls(CALL_SELECT, FN_SELECT)) {
        LOG("model_lod: this isn't the game it knows, models stay as the game picks them\n");
        return;
    }
    slot = &g_disp[(g_main + FN_SELECT - g_code_lo) / 4];
    g_select = *slot ? *slot : snx_interp_entry;
    *slot = select_level;
    LOG("model_lod: models at their finest level, main at %llx\n", (unsigned long long)g_main);
}
