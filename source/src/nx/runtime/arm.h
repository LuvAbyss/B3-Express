/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit game: its module (nx/loader/module_abi.h's SnxArmModule), the thread loop running its AArch32 blocks, and
   the backend they leave coprocessors and what they can't run to (cpu/arm/arm.h's SnxArmRt) */
#ifndef SNX_NX_ARM_H
#define SNX_NX_ARM_H
#include "nx/loader/module_abi.h"

struct KThread;

/* the 32-bit game's module, NULL for a 64-bit one */
extern const SnxArmModule* g_arm_mod;
/* the backend the game's threads run with (SnxArmCPU.rt) */
extern const SnxArmRt g_arm_rt;
/* The SnxModule the rest of the runtime reads (its modules, title and main thread) for a 32-bit module, after checking
   its version: NULL when it is from another ScarletNX version. It has no A64 entries */
const SnxModule* arm_module(const SnxArmModule* module);
/* the block table, from the module's entries; before any thread runs */
void arm_dispatch_init(void);
/* runs the thread's AArch32 code until it exits; never returns */
void arm_thread_run(struct KThread* t);
/* the recompiled block whose host code is closest below host_pc, for the crash report: 0 when none is near it, else its
   guest address (bit 0 set for Thumb) and *offset into it */
u32 arm_block_at(const void* host_pc, u64* offset);

#endif
