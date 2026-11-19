/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The cheats turned on (nx/loader/cheat.h), run once for every frame the game shows */
#ifndef SNX_NX_RUNTIME_CHEATS_H
#define SNX_NX_RUNTIME_CHEATS_H
#include "nx/loader/cheat.h"
#include "common/cpu/aarch64/cpu.h"

/* p kept to run, its main module at main (its code from text for text_size bytes): 1, p is the runtime's now; 0, an
   empty program or no room, p is still the caller's */
int cheats_add(SnxCheatProgram* p, u64 main, u64 text, u64 text_size);
/* each program kept, once */
void cheats_frame(void);
#endif
