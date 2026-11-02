/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Reconcile a shader's mapped output with guest memory. The host may have written the guest range while the GPU was
   running: shader-changed bytes land only where the guest still holds the value the shader originally read. */
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "nx/gpu/gpu.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

/* Nested because a result can flush an overlapping render target while landing. */
static _Thread_local unsigned t_result_depth;
void vk_result_begin(void) { t_result_depth++; }
void vk_result_end(void) { t_result_depth--; }
int vk_result_landing(void) { return t_result_depth != 0; }

void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    vk_result_begin();
    vk_rt_before_write(addr, size);
    u8* dst = gmem_ptr(addr, size);
    u8* tmp = NULL;
    const u8* cur = dst;
    int changed = 0;
    if (!cur) {
        tmp = malloc(size);
        if (!tmp) fatal("Out of memory landing a shader storage buffer");
        gmem_read(addr, tmp, size);
        cur = tmp;
    }
    u32 i = 0;
#if defined(__SSE2__)
    if (dst) {
        for (; size - i >= 16; i += 16) {
            __m128i r = _mm_loadu_si128((const __m128i*)(res + i));
            __m128i o = _mm_loadu_si128((const __m128i*)(org + i));
            unsigned mask = (unsigned)~_mm_movemask_epi8(_mm_cmpeq_epi8(r, o)) & 0xffff;
            if (!mask) continue;
            /* Every byte in this block changed in the shader. A full store is
               valid only if every guest byte still matches the input snapshot. */
            if (mask == 0xffff && !memcmp(dst + i, org + i, 16)) {
                memcpy(dst + i, res + i, 16);
                changed = 1;
                continue;
            }
            /* Inspect only shader-changed bytes. Never store an unchanged byte
               or replace a conflicting guest write with an old snapshot. */
            do {
                unsigned bit = (unsigned)__builtin_ctz(mask);
                if (dst[i + bit] == org[i + bit]) {
                    dst[i + bit] = res[i + bit];
                    changed = 1;
                }
                mask &= mask - 1;
            } while (mask);
        }
    }
#endif
    for (; i < size;) {
        if (i + 8 <= size && !memcmp(res + i, org + i, 8)) {
            i += 8;
            continue;
        }
        if (res[i] == org[i] || cur[i] != org[i]) {
            i++;
            continue;
        }
        u32 s = i;
        while (i < size && res[i] != org[i] && cur[i] == org[i]) i++;
        if (dst)
            memcpy(dst + s, res + s, i - s);
        else
            gmem_write(addr + s, res + s, i - s);
        changed = 1;
    }
    free(tmp);
    if (changed) gpu_mem_written_shader(addr, size); /* other GPU virtual addresses can alias these guest pages */
    vk_result_end();
}
