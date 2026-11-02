/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Deterministic check of landing by target in gpu/vk/buffer.c: a landing waits for the submission that holds the
   result the caller asked for (B2recomp's slot_wait), and not for the whole queue. No Vulkan device is needed: a
   submission is a number its completion signals, and the submit/wait/land calls are recorded.
   Run plain for that path, with "fallback" as the first argument for SNX_VK_LAND_TARGET=0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"

/* ---- what buffer.c names, faked ---- */
VkState g_vk;
u8* g_mem;
_Atomic int g_cs_any, g_gfx_any;
u32 g_vk_epoch, g_vk_present;
u64 g_vk_dispatches, g_vk_drawn;
static unsigned lock_depth, submits, finishes, waits, lands;
static u64 last, waited, landed;
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what;
    (void)file;
    (void)line;
    return r == VK_SUCCESS;
}
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    abort();
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 plat_ns(void) { return 0; }
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    (void)p;
    (void)size;
    (void)cb;
    return 0;
}
void vk_lock(void) { lock_depth++; }
void vk_unlock(void) { lock_depth--; }
/* Every range is unmapped here, so only an exact GPU range overlaps another one (no backing aliases). */
u64 gmmu_translate(u64 gva) {
    (void)gva;
    return 0;
}
u8* gmem_ptr(u64 gva, u64 size) {
    (void)gva;
    (void)size;
    return NULL;
}
u8* gmem_ptr_untracked(u64 gva, u64 size) {
    (void)gva;
    (void)size;
    return NULL;
}
int gmem_read(u64 gva, void* dst, u64 size) {
    (void)gva;
    (void)dst;
    (void)size;
    return 0;
}
u64 gpu_mem_generation(void) { return 0; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)since;
    (void)a;
    (void)sz;
    return 0;
}
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    (void)addr;
    (void)res;
    (void)org;
    (void)size;
}
u64 vk_submit_number(void) { return last + 1; }
u64 vk_frame_id(void) { return 0; }
u64 vk_last_submit(void) { return last; }
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 stage, VkSemaphore signal) {
    (void)wait;
    (void)stage;
    (void)signal;
    submits++;
    last++; /* a submission is the timeline value it signals */
}
void vk_finish(void) {
    finishes++;
    last++;
}
void vk_wait_submit(u64 value) {
    waits++;
    waited = value;
}
void vk_land_through(u64 value) {
    lands++;
    landed = value;
}
void vk_render_end(void) {}
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
/* volk's entry points are pointers (VK_NO_PROTOTYPES): the test records nothing through them */
PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2;
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) {
    (void)size;
    (void)align;
    (void)buf;
    (void)off;
    return NULL;
}
u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    (void)size;
    (void)align;
    (void)buf;
    (void)off;
    (void)mem;
    return NULL;
}
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    (void)size;
    (void)buf;
    (void)off;
    (void)mem;
    return NULL;
}
u64 vk_stage_align(void) { return 16; }
u64 vk_stage_pad(void) { return 16; } /* the atom a ranged invalidation aligns its end up to */
/* the ranged invalidation a landing asks for, recorded so its bounds can be checked */
static VmaAllocation invalidated_mem;
static u64 invalidated_from, invalidated_to;
static int invalidate_calls;
VkResult vk_invalidate_range(VmaAllocation mem, u64 from, u64 to) {
    invalidated_mem = mem;
    invalidated_from = from;
    invalidated_to = to;
    invalidate_calls++;
    return VK_SUCCESS;
}
u64 vk_hash(const void* p, u64 size) {
    (void)p;
    return size;
}
u64 vk_sample_hash(u64 gva, u64 len) {
    (void)gva;
    return len;
}
void vk_defer_buffer(VkBuffer b, VmaAllocation a) {
    (void)b;
    (void)a;
}
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    (void)gva;
    (void)len;
    (void)buf;
    (void)off;
    return 0;
}
void vk_compute_mem_need(u64 a, u64 n) {
    (void)a;
    (void)n;
}
VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo* ci, const VmaAllocationCreateInfo* aci,
                         VkBuffer* buf, VmaAllocation* mem, VmaAllocationInfo* info) {
    (void)allocator;
    (void)ci;
    (void)aci;
    (void)buf;
    (void)mem;
    (void)info;
    return VK_SUCCESS;
}
VkResult vmaFlushAllocation(VmaAllocator allocator, VmaAllocation mem, VkDeviceSize offset, VkDeviceSize size) {
    (void)allocator;
    (void)mem;
    (void)offset;
    (void)size;
    return VK_SUCCESS;
}
VkResult vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation mem, VkDeviceSize offset, VkDeviceSize size) {
    (void)allocator;
    (void)mem;
    (void)offset;
    (void)size;
    return VK_SUCCESS;
}

#include "nx/gpu/vk/buffer.c"

static int failures;
static void want(int cond, const char* what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}
static void out_add(u64 addr, u32 size, u64 frame) {
    GfxOut* o = gfx_out_new(1, size);
    if (!o) abort();
    o->addr = addr;
    o->size = size;
    o->frame = frame;
    o->pages[0] = ~0ull; /* unmapped: the exact range decides */
    o->npages = 1;
    if (g_prep_tail)
        g_prep_tail->next = o;
    else
        g_prep = o;
    g_prep_tail = o;
    vk_buf_writable_commit();
}
static void outs_reset(void) {
    GfxOut* o = g_out;
    while (o) {
        GfxOut* next = o->next;
        free(o);
        o = next;
    }
    g_out = NULL;
    g_out_tail = NULL;
    g_gfx_any = 0;
}
static void counts_reset(void) {
    submits = finishes = waits = lands = 0;
    waited = landed = 0;
}

int main(int argc, char** argv) {
    int fallback = argc > 1 && strcmp(argv[1], "fallback") == 0;
    _putenv_s("SNX_VK_LAND_TARGET", fallback ? "0" : "1");
    last = 9;
    out_add(0x1000, 0x100, 3);
    out_add(0x8000, 0x40, 7);
    want(out_land_target(0x1000, 0x100, NULL, 0) == 3, "a range's own result decides its wait");
    want(out_land_target(0x8000, 0x40, NULL, 0) == 7, "the newer result over another range decides its own");
    want(out_land_target(0x4000, 0x10, NULL, 0) == 0, "a range no result covers has no target");
    want(out_land_target(0, 0, NULL, 1) == 7, "landing everything targets the newest pending result");
    if (fallback) {
        /* SNX_VK_LAND_TARGET=0: the whole queue, as before */
        counts_reset();
        vk_buf_land();
        want(finishes == 1, "the fallback still waits for the whole queue");
        want(waits == 0 && lands == 0, "the fallback does not land by target");
    } else {
        /* a read of one range lands what covers it, waiting for that submission alone */
        counts_reset();
        vk_buf_mem_need(0x1000, 0x100);
        want(waits == 1 && waited == 3, "the read waits for its own result's submission");
        want(lands == 1 && landed == 3, "and lands through it");
        want(submits == 0 && finishes == 0, "an already submitted result is neither submitted nor waited for again");
        /* a read no pending result covers waits for nothing */
        counts_reset();
        vk_buf_mem_need(0x4000, 0x10);
        want(waits == 0 && lands == 0 && submits == 0, "a range no result covers is waited for by nothing");
        /* a result still in the recording has to be submitted before it can land */
        out_add(0x2000, 0x80, last + 1);
        counts_reset();
        vk_buf_mem_need(0x2000, 0x80);
        want(submits == 1, "the result in the recording is submitted");
        want(waits == 1 && waited == 10 && landed == 10, "the wait follows that submission");
        want(finishes == 0, "without a whole-queue wait");
        /* the game about to observe memory: every pending result lands, through the newest one */
        outs_reset();
        last = 20;
        out_add(0x3000, 0x40, 12);
        out_add(0x4000, 0x40, 15);
        counts_reset();
        vk_buf_land();
        want(waits == 1 && waited == 15, "a signal point waits for the newest pending result");
        want(lands == 1 && landed == 15, "and lands through it");
        want(submits == 0 && finishes == 0, "and neither submits nor waits for the whole queue");
        outs_reset();
    }
    /* A landed result is invalidated over the bytes its copy wrote, not over the whole back ring: the allocations are
       tens of megabytes and this runs once per result of every frame. */
    outs_reset();
    invalidate_calls = 0;
    invalidated_mem = VK_NULL_HANDLE;
    last = 5;
    out_add(0x9000, 0x40, 5);
    {
        static u8 copied[0x40];
        GfxOut* o = g_out;
        while (o && o->frame != 5) o = o->next;
        want(o != NULL, "the result of the frame to land is pending");
        o->ptr = copied;
        o->mem = (VmaAllocation)(uintptr_t)0x1234;
        o->back_off = 0x200;
    }
    vk_buf_frame_done(5);
    want(invalidate_calls == 1 && invalidated_mem == (VmaAllocation)(uintptr_t)0x1234,
         "landing invalidates the back buffer once");
    want(invalidated_from == 0x200 && invalidated_to == 0x240, "over the copy's own bytes alone");
    want(g_out == NULL, "and the landed result leaves the frame's list");
    outs_reset();
    want(lock_depth == 0, "the landing paths leave the lock");
    if (failures) {
        fprintf(stderr, "land_target: %d failure(s)\n", failures);
        return 1;
    }
    printf("land_target: ok (%s)\n", fallback ? "fallback" : "target");
    return 0;
}
