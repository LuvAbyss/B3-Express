/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's side of RtAPI (rtapi.c), and the hints file: the code addresses a run met that the recompiler had not
   found, so the next recompile compiles them too */
#ifndef SNX_RTAPI_H
#define SNX_RTAPI_H
#include "common/cpu/aarch64/cpu.h"

extern const RtAPI g_rtapi; /* every game thread's CPU.rt */
void hints_init(void);      /* <exedir><name>.hints, once the game is named */
void load_hints(void);      /* the file's addresses, so a save keeps them */
void save_hints(void);      /* the file again, when this run added any. Any thread */
void snx_fpcr_apply(CPU* c); /* the thread's FPCR as it starts and on each write */
/* What runs at a hook point (the game's .insnhooks, SNX_RT_HOOK): fn before the instruction at pc, in the game's
   thread. Set before the game starts, 0 when the table is full. A hook point with nothing set does nothing */
int rt_hook_set(u64 pc, void (*fn)(CPU* RESTRICT c, u64 pc));

#endif
