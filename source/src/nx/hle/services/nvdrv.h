/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What nvdrv shares with the GPU, host1x and vi: nvmap handles, the GPU's address space, and the syncpoints */
#ifndef SNX_NVDRV_H
#define SNX_NVDRV_H
#include "common/cpu/aarch64/cpu.h"

/* The syncpoint ids the table holds. An id at or past it would alias a smaller one: it reads 0, never counts as
   reached, and the ioctls refuse it. syncpt_alloc hands out fewer (NVIDIA's stream library has room for 128) */
#define SYNCPT_IDS 256

void nv_init(void);

/* A live, allocated handle's address and size, read together under nvdrv's lock from any thread. 1, or 0 (both
   outputs 0) for a dead handle */
int nvmap_get(u32 h, u64* addr, u32* size);
u64 nvmap_addr_by_id(u32 id); /* 0 when no live handle has that id */

/* The guest address a GPU address maps to, 0 when its page isn't mapped */
u64 gmmu_translate(u64 gva);
/* The bytes from gva on, at most max, that stay contiguous in guest memory, with gva's guest address in *cpu. 0 when
   gva's page isn't mapped */
u64 gmmu_run(u64 gva, u64 max, u64* cpu);

/* Every syncpoint user allocates its ids here, so they stay below the 128 the stream library sizes its tables at.
   Running out is fatal. A freed id can be handed out again */
u32 syncpt_alloc(void);
void syncpt_free(u32 id);
u32 syncpt_read(u32 id);
void syncpt_incr(u32 id);
void syncpt_add(u32 id, u32 n); /* n increments, one wakeup */
/* A channel promises the next n increments it will make, in order. Returns the value they reach: the fence the ioctl
   hands back, and what nvhost-ctrl reads as the maximum */
u32 syncpt_promise(u32 id, u32 n);
/* 1 once id reaches thresh (across the 32-bit wrap), 0 when the time runs out. 0 looks once, negative waits for good */
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns);
/* Runs fn(arg) on the GPU thread after the submissions that promised the fences (id[i], val[i]), i < n <= 4. A fence
   nothing queued has promised is waited for here instead, for at most 1 s, and its id[i] is set to ~0 */
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg);
/* each SUBMIT_GPFIFO, on the submitting thread (hle/nvn/kick.c's trace), when set */
extern void (*g_nvdrv_submit_note)(u32 sp, u32 num, u32 flags, u32 fence, u64 job);

#endif
