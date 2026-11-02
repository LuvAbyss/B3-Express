/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module the runtime runs (<Game>_code.so, whose descriptor is module_abi.h's SnxModule), and loading the
   game's own executables into guest memory where that module expects them (loader.c) */
#ifndef SNX_LOADER_H
#define SNX_LOADER_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/loader/module_abi.h"

/* The loaded game module and its dispatch table, indexed by (guest address - g_code_lo) / 4 (main.c) */
extern const SnxModule* g_mod;
extern GuestFn* g_disp;
/* Maps every module g_mod lists at its base, from the game's data folder, and sets its pages' permissions. 0, or -1
   after logging why */
int loader_load(void);
/* The guest code range g_disp covers */
extern u64 g_code_lo, g_code_hi;

#endif
