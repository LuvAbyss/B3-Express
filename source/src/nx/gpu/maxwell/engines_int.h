/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the GPU engines' files (engines.c, mme.c, compute.c, fermi2d.c, dma.c, maxwell3d.c) share */
#ifndef SNX_ENGINES_INT_H
#define SNX_ENGINES_INT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* engines.c: a report semaphore operation (the word v written to base + 3) of the 3D or compute engine, whose
   registers r hold the address at base and base + 1 and the payload at base + 2 */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);

/* dma.c: a block linear surface's bytes up to slice z as one host pointer, with their count in span, when they are
   contiguous in guest memory and 256 MiB at most; NULL otherwise */
u8* bl_span_ptr(u64 base, u32 wbytes, u32 h, u32 bh, u32 z, u32 bd, u64* span);
/* dma.c: the bytes of a block linear surface up to the end of slice z (the span bl_span_ptr hands out) */
u64 bl_span_size(u32 wbytes, u32 h, u32 bh, u32 z, u32 bd);

/* mme.c: runs macro idx over the np parameters in ch->mme_params (it may stop at a wait, gpu.h's mme_resume) */
void mme_run(GpuChannel* ch, u32 idx, u32 np);

#endif
