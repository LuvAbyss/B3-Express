/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the kernel's files (kernel.c, thread.c, sync.c, debug.c, ipc.c, the SVCs) share among themselves */
#ifndef SNX_KERNEL_INT_H
#define SNX_KERNEL_INT_H
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/loader/loader.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/rtapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's results (module 1), as RC(1, description) gives them */
#define R_OUTOFMEM      0xD001  /* 104 OutOfMemory */
#define R_BADSIZE       0xCA01  /* 101 InvalidSize */
#define R_BADADDR       0xCC01  /* 102 InvalidAddress */
#define R_BADMEMSTATE   0xD401  /* 106 InvalidCurrentMemory */
#define R_BADREGION     0xDC01  /* 110 InvalidMemoryRegion */
#define R_BADPRIO       0xE001  /* 112 InvalidPriority */
#define R_BADCORE       0xE201  /* 113 InvalidCoreId */
#define R_BADHANDLE     0xE401  /* 114 InvalidHandle */
#define R_BADCOMBO      0xE801  /* 116 InvalidCombination */
#define R_TIMEOUT       0xEA01  /* 117 TimedOut */
#define R_CANCELLED     0xEC01  /* 118 Cancelled */
#define R_OUTOFRANGE    0xEE01  /* 119 OutOfRange */
#define R_BADENUM       0xF001  /* 120 InvalidEnumValue */
#define R_NOTFOUND      0xF201  /* 121 NotFound */
#define R_BUSY          0xF401  /* 122 Busy */
#define R_SESSIONCLOSED 0xF601  /* 123 SessionClosed */
#define R_BADSTATE      0xFA01  /* 125 InvalidState */
#define R_LIMIT         0x10801 /* 132 LimitReached */
#define R_MSGTOOLARGE   0x20801 /* 260 MessageTooLarge */
#define R_NOTIMPL       0x4201  /* 33 NotImplemented (not SessionClosed's 0xF601) */

#define HANDLE_WAIT  0x40000000u /* the waiters bit of a mutex word, beside the owner's handle */
#define H_CUR_PROC   0xFFFF8001u /* pseudo-handles */
#define H_CUR_THREAD 0xFFFF8000u

/* one SVC as the trace ring records it: number, result, caller, the first arguments and an output */
typedef struct {
    u32 imm, res;
    u64 lr, a[4], o1;
} SvcRec;

/* ---- kernel.c: handles, waiting, events. _nl: the caller holds g_klock */
extern u64 g_next_tid;
extern SvcRec g_svc_ring[64];
extern u32 g_svc_ring_n;
KObj* h_get_nl(u32 h);
/* no reference taken: only for threads, which are never freed */
KObj* h_get(u32 h);
KObj* h_get_type(u32 h, int t);
int h_close(u32 h);
void waiter_add(KObj* o, KThread* t);
void waiter_del(KObj* o, KThread* t);
void wake(KThread* t, int result);
void kobj_signal_nl(KObj* o);
int sleep_nl(KThread* t, i64 timeout);

/* ---- thread.c: threads and the guest cores */
#define THREAD_MAX 96 /* the application's ThreadCountMax: creating one more answers R_LIMIT */
extern KThread* g_threads[256];
extern int g_nthreads;
int kthread_live_count(void);
KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core);
void kthread_start(KThread* t);
void svc_exit_thread(KThread* t);
extern KThread* g_main;
void process_exit(int code);
extern int g_default_core; /* the main thread's, which core -2 names */
/* gives the core to a waiting thread of higher priority, then waits for it again */
void core_preempt(KThread* t);
/* gives the core to a waiting thread of any priority. migrate 2 may run it on an idle core of its mask instead */
void core_yield(KThread* t, int migrate);
void core_set_prio(KThread* t, int prio); /* the effective priority, reordering the core's wait list */

/* ---- sync.c: mutexes, condition variables, address arbitration, WaitSynchronization */
u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag);
u32 svc_arbitrate_unlock(u64 addr);
void prio_set_base_nl(KThread* t, int prio);
u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout);
u32 svc_signal_cv(u64 key, i32 count);
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i32 value, i64 timeout);
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count);
u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx);

/* ---- svc_mem.c: the memory SVCs */
u32 svc_query(u64 info, u64 addr);
u32 svc_map_memory(u64 dst, u64 src, u64 size);
u32 svc_unmap_memory(u64 dst, u64 src, u64 size);
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm);
u32 svc_unmap_shm(u32 h, u64 addr, u64 size);
void tmem_destroy(KObj* o);
u64 mem_total(void);
u64 used_memory(void);
u32 svc_set_heap_size(u64 size);
u32 svc_map_physical(u64 addr, u64 size);
u32 svc_unmap_physical(u64 addr, u64 size);
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out);
u32 svc_set_mem_perm(u64 addr, u64 size, u32 perm);
u32 svc_set_mem_attr(u64 addr, u64 size, u32 mask, u32 attr);
/* the new object comes back in *out, for svc.c to make its handle */
u32 svc_tmem_new(u64 addr, u64 size, u32 perm, KTmem** out);
u32 svc_codemem_new(u64 addr, u64 size, KObj** out);
u32 svc_codemem_control(KObj* o, u32 op, u64 addr, u64 size, u32 perm);
#endif
