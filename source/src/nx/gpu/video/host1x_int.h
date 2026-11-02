/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shared by host1x.c, nvdec.c and vic.c: the engines' view of memory through IOVAs, and their entry points */
#ifndef SNX_HOST1X_INT_H
#define SNX_HOST1X_INT_H
#include "nx/gpu/video/host1x.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* The little-endian u32 or u64 at byte off of p, at any alignment */
static inline u32 rd32(const u8* p, u32 off) {
    u32 v;
    memcpy(&v, p + off, 4);
    return v;
}
static inline u64 rd64(const u8* p, u32 off) {
    u64 v;
    memcpy(&v, p + off, 8);
    return v;
}

/* host1x.c: the guest memory behind [iova, iova + len), or NULL unless one mapping holds all of it (len 0 is NULL
   too). Callable from any thread */
u8* iova_ptr(u64 iova, u64 len);

/* nvdec.c: one decoder per NVDEC channel. The channel's worker calls nvdec_execute with the engines' lock held, and
   the channel's close calls nvdec_free the same way */
typedef struct NvdecCtx NvdecCtx;
NvdecCtx* nvdec_new(void); /* NULL when out of memory */
void nvdec_free(NvdecCtx* d);
void nvdec_execute(NvdecCtx* d, const u32* r); /* r: the channel's 0x400 method registers */

/* vic.c: one composition from the channel's method registers */
void vic_execute(const u32* r);

#endif
