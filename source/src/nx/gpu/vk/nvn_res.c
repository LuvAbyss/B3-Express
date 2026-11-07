/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Phase 3 step 3b of diagnostics/nvn-route-plan-20261029/PLAN.md: an NVN memory pool as a Vulkan buffer over the
   guest's own storage (VK_EXT_external_memory_host), so the GPU reads the bytes the game writes, with no copy and no
   tracking of its writes. The check copies part of such a buffer on the GPU and compares it with guest memory */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/nvn_res.h"
#include "nx/runtime/main.h"
#include "nx/gpu/vk/fsr1.h"
#include "nx/gpu/vk/dither.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/options.h"
#include "nx/hle/aspect.h"
#include "nx/gpu/vk/vk.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
/* The two scales. resolution the window's textures, resolution / fsr the scene when FSR 1 upscales it */
static u32 g_rp_win_w, g_rp_win_h;
void vk_nvn_window_size(u32 w, u32 h) {
    g_rp_win_w = w;
    g_rp_win_h = h;
}
static int rp_fsr_on(void) { return g_opt_fsr > 1.0f && vk_fsr1_ready(); }
static float rp_scene_scale(void) { return rp_fsr_on() ? g_vk_scale / g_opt_fsr : g_vk_scale; }
/* aspect_ratio wider than 16:9 (hle/aspect.c): the game's window is wider but its scene targets keep 16:9 sizes and
   its last pass stretches them into it. Such a target is made this much wider again, so the scene has the window's
   pixels across (Witch Time's rs_xstretch, runtime/nvn_vk_draw.c, GPL-3.0-or-later) */
static float rp_xstretch(void) {
    float a = aspect_ratio();
    return a > 16.0f / 9.0f ? a * 9.0f / 16.0f : 1.0f;
}
static int rp_scaling(void) { return g_vk_scale != 1.0f || rp_fsr_on() || rp_xstretch() != 1.0f; }
/* what prepared shaders are for, beside the driver: the scaling (FragCoord in the game's pixels) and dither=N, which
   change pixel stages. 0 and 1 with dither=0, as before it existed. The launcher reads it back (shaders_ready) */
static int prep_mode(void) { return rp_scaling() + 2 * g_opt_dither; }

/* c7 (the video memory copies, below): the GPU's blocks of a range land in guest memory before the CPU reads it */
static void own_land_range(int p, u64 off, u64 size);
static void own_land_gpu(u64 gpu, u64 size);
/* why the next landing happens (own_land_now counts it): 0 the game wrote a GPU block, 1 a texture over it, 2 a CPU
   copy of a constant buffer, 3 no memory to own, 4 no write tracking, 5 a read partly in the copy, 6 a storage buffer
   written in the pool, 7 a buffer copy or clear, 8 a constant update */
static int g_land_why;
static u64 g_land_by[9];
#define IMPORTS 64
/* A pool's storage imported as a Vulkan buffer. Importing pins every page of it in the driver, about 0.2 ms a MiB (300
   ms for the 1.3 GB pool of an area load, on the game's thread in nvnMemoryPoolInitialize). So the import runs on a
   thread of its own (import_thread): the slot is reserved at once (IMP_PENDING) and published by the renderer, under
   vk_lock, once ready (import_publish): gpu stays 0 until then, so every walk of the table skips it. A lookup of an
   address in a pool still importing waits for that one import (import_of), as nothing of it can be drawn or copied
   without the buffer. The thread never takes vk_lock */
enum { IMP_READY, IMP_PENDING, IMP_DONE, IMP_FAILED }; /* DONE: imported, not yet published */
static struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    u64 cpu, size, gpu; /* gpu: the pool's GPU address, 0 until known and published */
    u64 want_gpu;       /* the GPU address and the mirror wish given while it imported */
    int want_mirror, state;
} g_import[IMPORTS];
static int g_nimport;
static PMutex g_imp_lk = PMUTEX_INIT; /* the states and the queue */
static PCond g_imp_cv;
static int g_imp_queue[IMPORTS], g_imp_qn, g_imp_thread, g_imp_unpublished;
static u64 g_imp_waits;                    /* lookups that waited for an import */
static void mir_want(int which, int want); /* the video memory copies, below */
u64 vk_nvn_import_align(void) { return g_vk.has_host_import ? g_vk.host_import_align : 0; }
/* vk_lock held: imports finished since the last call become visible to the renderer */
static void import_publish(void) {
    int i;
    if (!__atomic_load_n(&g_imp_unpublished, __ATOMIC_ACQUIRE)) return;
    pm_lock(&g_imp_lk);
    __atomic_store_n(&g_imp_unpublished, 0, __ATOMIC_RELAXED);
    for (i = 0; i < g_nimport; i++)
        if (g_import[i].state == IMP_DONE) {
            g_import[i].state = IMP_READY;
            g_import[i].gpu = g_import[i].want_gpu;
            mir_want(i, g_import[i].want_mirror);
        }
    pm_unlock(&g_imp_lk);
}
/* waits for import i to end: 1 when it was made, 0 when it failed. The import thread takes no other lock */
static int import_wait(int i) {
    int state;
    pm_lock(&g_imp_lk);
    while (g_import[i].state == IMP_PENDING) pc_wait(&g_imp_cv, &g_imp_lk, -1);
    state = g_import[i].state;
    pm_unlock(&g_imp_lk);
    return state != IMP_FAILED;
}
void vk_nvn_pool_gpu(int which, u64 gpu) {
    vk_lock();
    import_publish();
    if (which >= 0 && which < g_nimport) {
        pm_lock(&g_imp_lk);
        g_import[which].want_gpu = gpu;
        if (g_import[which].state == IMP_READY) g_import[which].gpu = gpu;
        pm_unlock(&g_imp_lk);
    }
    vk_unlock();
}
/* the imported buffer holding GPU range [a, a + n): its number and the offset in it, -1 when none does. A pool still
   importing that holds it is waited for (vk_lock held) */
static int import_of(u64 a, u64 n, u64* off) {
    int i;
    import_publish();
    for (i = 0; i < g_nimport; i++)
        if (g_import[i].gpu && a >= g_import[i].gpu && a - g_import[i].gpu + n <= g_import[i].size) {
            *off = a - g_import[i].gpu;
            return i;
        }
    for (i = 0; i < g_nimport; i++)
        if (!g_import[i].gpu && g_import[i].want_gpu && a >= g_import[i].want_gpu &&
            a - g_import[i].want_gpu + n <= g_import[i].size && g_import[i].state != IMP_FAILED) {
            g_imp_waits++;
            if (!import_wait(i)) return -1;
            __atomic_store_n(&g_imp_unpublished, 1, __ATOMIC_RELEASE);
            import_publish();
            *off = a - g_import[i].gpu;
            return i;
        }
    return -1;
}
/* the expensive part of an import, on import_thread: the buffer over the guest's pages */
static int import_make(int i) {
    void* host = GP(g_import[i].cpu);
    u64 size = g_import[i].size;
    u32 type = ~0u, k;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkMemoryRequirements req;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkGetMemoryHostPointerPropertiesEXT(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                            &hp) != VK_SUCCESS)
        return 0;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext = &eb,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, &buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(g_vk.dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (k = 0; k < mp.memoryTypeCount && type == ~0u; k++) /* host visible and coherent first */
        if ((hp.memoryTypeBits & req.memoryTypeBits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & want) == want)
            type = k;
    for (k = 0; k < mp.memoryTypeCount && type == ~0u; k++)
        if (hp.memoryTypeBits & req.memoryTypeBits & (1u << k)) type = k;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = &imp,
                                .allocationSize = size,
                                .memoryTypeIndex = type};
    if (type == ~0u || vkAllocateMemory(g_vk.dev, &mai, NULL, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(g_vk.dev, buf, mem, 0) != VK_SUCCESS) {
        if (mem) vkFreeMemory(g_vk.dev, mem, NULL);
        vkDestroyBuffer(g_vk.dev, buf, NULL);
        return 0;
    }
    g_import[i].buf = buf; /* read by the renderer only once published */
    g_import[i].mem = mem;
    return 1;
}
static void import_thread(void* arg) {
    (void)arg;
    pm_lock(&g_imp_lk);
    for (;;) {
        int i, ok;
        u64 t0;
        while (!g_imp_qn) pc_wait(&g_imp_cv, &g_imp_lk, -1);
        i = g_imp_queue[0];
        memmove(g_imp_queue, g_imp_queue + 1, (size_t)--g_imp_qn * sizeof *g_imp_queue);
        pm_unlock(&g_imp_lk);
        t0 = plat_ns();
        ok = import_make(i);
        LOG("nvn-live: pool import %d (%llu MiB) %s in %.1f ms, off the game's thread\n", i,
            (unsigned long long)(g_import[i].size >> 20), ok ? "made" : "FAILED", (double)(plat_ns() - t0) / 1e6);
        pm_lock(&g_imp_lk);
        g_import[i].state = ok ? IMP_DONE : IMP_FAILED;
        __atomic_store_n(&g_imp_unpublished, 1, __ATOMIC_RELEASE);
        pc_broadcast(&g_imp_cv);
    }
}
/* nvnMemoryPoolInitialize (the game's thread): reserves the import and queues it. Its number, or -1 */
int vk_nvn_pool_import(u64 cpu, u64 size) {
    u64 align = g_vk.host_import_align;
    void* host;
    int i;
    if (!g_vk.has_host_import || !cpu || !size || !g_mem) return -1;
    host = GP(cpu);
    if (((uintptr_t)host & (align - 1)) || (size & (align - 1))) return -1;
    vk_lock(); /* the table's length is the renderer's */
    pm_lock(&g_imp_lk);
    if (g_nimport == IMPORTS) {
        pm_unlock(&g_imp_lk);
        vk_unlock();
        return -1;
    }
    i = g_nimport++;
    memset(&g_import[i], 0, sizeof g_import[i]);
    g_import[i].cpu = cpu;
    g_import[i].size = size;
    g_import[i].state = IMP_PENDING;
    g_imp_queue[g_imp_qn++] = i;
    if (!g_imp_thread) {
        pc_init(&g_imp_cv);
        g_imp_thread = 1;
        if (plat_thread(import_thread, NULL, 0)) fatal("could not start the pool import thread");
    }
    pc_broadcast(&g_imp_cv);
    pm_unlock(&g_imp_lk);
    vk_unlock();
    return i;
}

/* ------------------------------------------------------------ shader stages (phase 3 step 3c part 2) */
/* A program stage translated live with the renderer's own vk_translate_spirv, its constant buffer 1 (the program's
   constant data, BRX jump tables) read from guest memory, and loaded as a VkShaderModule, kept with its ShaderInfo
   for the pipelines. Under vk_lock, as the renderer translates: the translator's state is shared */
#define MODULES 8192
static struct {
    VkShaderModule mod;
    ShaderInfo si;
    u32 spv_hash; /* the same across runs, for the logs */
} g_mod[MODULES];
static int g_nmod;
static const u8* g_cb1;
static u32 g_cb1_size;
static int cb1_read(u32 idx, u32 off, u32* out) {
    if (idx != 1 || !g_cb1 || off + 4 > g_cb1_size) return 0;
    memcpy(out, g_cb1 + off, 4);
    return 1;
}
/* dither=N: the game's screen-door fade in a pixel stage, changed as gpu/vk/dither.c says. SNX_DITHER_FADE=a (test
   only) gives every fading object alpha a, so the pattern shows on everything that has it. Blending needs dual-source
   blending: without it the fade is only switched off */
static void dither_stage(u32** spv, size_t* words, ShaderInfo* si) {
    static int mode = -1, stages, compares;
    static float fade = -1.0f;
    int n, blend = 0;
    if (mode < 0) {
        const char* e = getenv("SNX_DITHER_FADE");
        mode = g_opt_dither;
        if (mode == DITHER_BLEND && !g_vk.feat.dualSrcBlend) mode = DITHER_OFF;
        if (e && *e) fade = (float)atof(e);
        LOG("nvn-live: object fade %d (%s)%s\n", mode, (const char*[]){"game", "off", "even", "blend"}[mode],
            fade >= 0.0f ? ", every fade at a fixed alpha (SNX_DITHER_FADE)" : "");
    }
    if (mode == DITHER_GAME && fade < 0.0f) return;
    n = dither_patch(spv, words, mode, fade, &blend);
    if (!n) return;
    si->dither_blend = blend;
    stages++, compares += n;
    if (stages <= 3 || stages % 100 == 0) LOG("nvn-live: object fade: %d stages changed, %d fades\n", stages, compares);
}
/* film_grain=0: the constant buffer word to read as 0 in a pixel stage, or 0. Bayonetta 3 draws its grain in four
   tonemap stages as colour * (1 + (0.5 - noise) * c4[13]) (m_GrainIntensity), so 0 there removes it and keeps the
   vignette. The stages are known by the FNV-1a hash of their code (from the program header at data + 0x30,
   control + 0x6f8 bytes, the offset basis xor 1) that Witch Time found by translating every pixel stage of the game:
   4 of 5,485 have it (github.com/Zexyen/Witch-Time, runtime/nvn_vk_m4.c stage_fx, GPL-3.0, issue 86) */
static u32 grain_word(int stage, const u8* code, u32 window) {
    static const u64 tonemap[] = {0x44931dbda608639dull, 0x48c1d6be354413c1ull, 0xe84083dddb26435cull,
                                  0xe3b5d8050915b977ull};
    static int logged;
    u64 h = 0xcbf29ce484222325ull ^ 1u;
    u32 i;
    if (g_opt_film_grain || stage != ST_FS || window < 0x50) return 0;
    for (i = 0; i < window - 0x50; i++) h = (h ^ code[i]) * 0x100000001b3ull;
    for (i = 0; i < 4; i++)
        if (h == tonemap[i]) {
            if (logged++ < 4) LOG("nvn-live: film grain off in tonemap stage %016llx\n", (unsigned long long)h);
            return (4u << 16 | 13u) + 1;
        }
    return 0;
}
/* the stage's SPIR-V for key, the caller to free it. Under vk_lock: the translator's state is shared */
static u32* stage_spirv(int stage, const u8* code, u32 window, int compute_stub, u64 cb1_gva, u32 cb1_size,
                        const u32* cs_local, u32 shared, const NvnStageKey* key, ShaderInfo* out, size_t* words) {
    static u8 framed[0x10000 + 80];
    ShaderInfo si;
    VkCbReads reads = {NULL, 0};
    u32* spv;
    if (compute_stub) { /* no program header: 80 zero bytes stand in for one, as the renderer frames compute code */
        memset(framed, 0, 80);
        memcpy(framed + 80, code, window);
        code = framed;
        window += 80;
    }
    memset(&si, 0, sizeof si);
    si.stage = stage;
    si.host_scale = rp_scaling();
    si.zero_cbuf = grain_word(stage, code, window);
    if (stage == ST_CS && cs_local) {
        memcpy(si.cs_local, cs_local, sizeof si.cs_local);
        si.shared_size = (int)shared;
    }
    if (key && stage == ST_FS) { /* as vk_shader_get keys a stage */
        si.rt_int_mask = key->rt_int;
        si.rt_sint_mask = key->rt_sint;
        si.in_unfed_mask = key->unfed;
        si.tex_int_mask = key->tex_int;
        si.tex_sint_mask = key->tex_sint;
        si.tex_dc_color_mask = key->dc_mask;
        si.tex_dc_color_funcs = key->dc_funcs;
    } else if (key) {
        si.attr_int_mask = key->int_mask;
        si.attr_sint_mask = key->sint_mask;
        si.rt_int_mask = key->rt_int;
        si.clip_enable = key->clip;
        if (stage == ST_CS) { /* as compute.c keys a program */
            si.tex_int_mask = key->tex_int;
            si.tex_sint_mask = key->tex_sint;
        }
    }
    g_cb1 = cb1_size && cb1_size <= 0x10000 ? gmem_ptr_untracked(cb1_gva, cb1_size) : NULL;
    g_cb1_size = g_cb1 ? cb1_size : 0;
    /* the instance id from BaseInstance: NVN's indirect draws carry their base instance on the GPU */
    g_shader_instance_base_builtin = g_vk.has_shader_draw_parameters;
    spv = vk_translate_spirv(code, window, stage, &si, words, cb1_read, &reads);
    g_shader_instance_base_builtin = 0;
    if (spv && stage == ST_FS) dither_stage(&spv, words, &si);
    g_cb1 = NULL;
    free(reads.r);
    *out = si;
    return spv;
}
/* SNX_NVN_SPV_DUMP=dir: each module as the renderer made it, named by the hash nvn-top logs: <hash>_<stage>.spv, the
   game's code it came from (.bin) and its constant buffer 1 (.cb1). The renderer handles programs itself, so
   SNX_NVN_CORPUS (the shadow path) sees none of them */
static void spv_dump(u32 hash, int stage, const u32* spv, size_t words, const u8* code, u32 window, u64 cb1_gva,
                     u32 cb1_size) {
    static int on = -1;
    static char dir[1024];
    char path[1100];
    const u8* cb1 = cb1_size && cb1_size <= 0x10000 ? gmem_ptr_untracked(cb1_gva, cb1_size) : NULL;
    FILE* f;
    if (on < 0) {
        const char* e = getenv("SNX_NVN_SPV_DUMP");
        on = e && *e && snprintf(dir, sizeof dir, "%s", e) < (int)sizeof dir;
        if (on) LOG("nvn-live: shader modules saved to %s\n", dir);
    }
    if (!on) return;
    snprintf(path, sizeof path, "%s/%08x_%d.spv", dir, hash, stage);
    if ((f = fopen(path, "wb"))) fwrite(spv, 4, words, f), fclose(f);
    snprintf(path, sizeof path, "%s/%08x_%d.bin", dir, hash, stage);
    if (code && (f = fopen(path, "wb"))) fwrite(code, 1, window, f), fclose(f);
    snprintf(path, sizeof path, "%s/%08x_%d.cb1", dir, hash, stage);
    if (cb1 && (f = fopen(path, "wb"))) fwrite(cb1, 1, cb1_size, f), fclose(f);
}
int vk_nvn_shader(int stage, u64 code_gva, u32 window, int compute_stub, u64 cb1_gva, u32 cb1_size, const u32* cs_local,
                  u32 shared, const NvnStageKey* key, char* why, u32 why_size) {
    const u8* code = window ? gmem_ptr_untracked(code_gva, window) : NULL;
    ShaderInfo si;
    size_t words = 0;
    u32* spv;
    int r = -1;
    why[0] = 0;
    if (!code || window > 0x10000 || stage < ST_VS || stage > ST_CS) {
        snprintf(why, why_size, "code not readable");
        return -1;
    }
    vk_lock();
    spv = stage_spirv(stage, code, window, compute_stub, cb1_gva, cb1_size, cs_local, shared, key, &si, &words);
    if (!spv)
        snprintf(why, why_size, "%s", si.first_unsupported[0] ? si.first_unsupported : "not translated");
    else if (g_nmod == MODULES)
        snprintf(why, why_size, "module table full");
    else {
        VkShaderModuleCreateInfo mi = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = spv};
        if (vkCreateShaderModule(g_vk.dev, &mi, NULL, &g_mod[g_nmod].mod) == VK_SUCCESS) {
            u32 h = 2166136261u, w;
            for (w = 0; w < words; w++) h = (h ^ spv[w]) * 16777619u;
            g_mod[g_nmod].si = si;
            g_mod[g_nmod].spv_hash = h;
            r = g_nmod++;
            spv_dump(h, stage, spv, words, code, window, cb1_gva, cb1_size);
        } else
            snprintf(why, why_size, "vkCreateShaderModule refused it");
    }
    free(spv);
    vk_unlock();
    return r;
}
/* ------------------------------------------------------------ shader preparation */
/* The game gives every program long before it draws with it (fight: a median of 78 s), and the keys are predictable: 96%
   of fragment stages draw with an empty key, vertex stages with every clip distance on and attribute 2 either way. So
   workers translate each stage for those keys and compile its library into the driver's pipeline cache, then destroy it.
   A draw's own compile finds the cache. Compute stages go the same way with an empty key, as most dispatches use them:
   their pipelines are made on the draw thread, with no budget to wait in (150-180 ms each on a new driver cache). A
   wrong guess costs what it cost before. Hashes of stages done are kept beside the cache, per driver and scale mode.
   The workers stop in heavy frames: the driver's compiles take the cores the game needs */
#define PREP_QUEUE   32768u
#define PREP_SET     (1u << 17)
#define PREP_HEAVY   200u /* draws in a frame */
#define PREP_VS_CLIP 0xFFu
typedef struct {
    int stage;
    u64 code, cb, hash;
    u32 window, cb_size;
    u32 cs_local[3], shared; /* a compute stage's workgroup size and shared memory, which its SPIR-V holds */
} PrepJob;
static PrepJob* g_prq;
static u32 g_prq_in, g_prq_out, g_prep_heavy;
static u64* g_prseen; /* this run's and earlier runs' hashes */
static PMutex g_prq_lk = PMUTEX_INIT;
static int g_prep_on = -1, g_prep_started, g_prep_run, g_prep_busy;
static u64 g_prep_quiet; /* a pass: when the last stage arrived or was finished */
static FILE* g_prf;
static u64 g_pr_set, g_pr_done, g_pr_gone, g_pr_ns, g_pr_paused;
#define PREP_GONE ~0ull       /* a set entry taken out: probes go past it */
static int prep_seen(u64 h) { /* adds it: 1 when it was there. Lock held */
    u32 i = (u32)h & (PREP_SET - 1), n;
    if (!h || h == PREP_GONE) h = 1;
    for (n = 0; n < PREP_SET; n++, i = (i + 1) & (PREP_SET - 1)) {
        if (g_prseen[i] == h) return 1;
        if (!g_prseen[i]) {
            g_prseen[i] = h;
            return 0;
        }
    }
    return 1;
}
/* takes h out of the set, for a stage whose memory the game reused before a worker got to it: when the game gives the
   same stage again, it is queued again instead of passed over as done. Lock held */
static void prep_forget(u64 h) {
    u32 i = (u32)h & (PREP_SET - 1), n;
    if (!h || h == PREP_GONE) h = 1;
    for (n = 0; n < PREP_SET && g_prseen[i]; n++, i = (i + 1) & (PREP_SET - 1))
        if (g_prseen[i] == h) {
            g_prseen[i] = PREP_GONE;
            return;
        }
}
/* a stage's identity: its code, its constant buffer 1 (cb, NULL for none), and for compute its workgroup size and
   shared memory. The worker checks it again, against what the game may have overwritten since */
static u64 prep_hash(int stage, const u8* code, u32 window, const u8* cb, u32 cb_size, const u32* cs_local,
                     u32 shared) {
    u64 h = vk_hash(code, window) ^ (u64)stage * 0x9E3779B97F4A7C15ull;
    if (cb) h ^= vk_hash(cb, cb_size) * 3;
    if (stage == ST_CS) h ^= vk_hash(cs_local, 3 * sizeof *cs_local) * 5 ^ shared;
    return h;
}
void vk_nvn_prep_add(int stage, u64 code_gva, u32 window, u64 cb1_gva, u32 cb1_size, const u32* cs_local, u32 shared) {
    const u8* code = window <= 0x10000 ? gmem_ptr_untracked(code_gva, window) : NULL;
    const u8* cb = cb1_size && cb1_size <= 0x10000 ? gmem_ptr_untracked(cb1_gva, cb1_size) : NULL;
    u64 h;
    if (g_prep_on < 0) {
        const char* e = getenv("SNX_NVN_PREP");
        g_prep_on = !(e && e[0] == '0');
    }
    if (!g_prep_on || !code || (stage != ST_VS && stage != ST_FS && (stage != ST_CS || !cs_local))) return;
    h = prep_hash(stage, code, window, cb, cb1_size, cs_local, shared);
    pm_lock(&g_prq_lk);
    if (!g_prq) {
        g_prq = calloc(PREP_QUEUE, sizeof *g_prq);
        g_prseen = calloc(PREP_SET, sizeof *g_prseen);
    }
    g_pr_set++;
    g_prep_quiet = plat_ns();
    if (g_prq && g_prseen && !prep_seen(h) && g_prq_in - g_prq_out < PREP_QUEUE) {
        PrepJob* j = &g_prq[g_prq_in++ % PREP_QUEUE];
        j->stage = stage;
        j->code = code_gva;
        j->cb = cb ? cb1_gva : 0;
        j->window = window;
        j->cb_size = cb ? cb1_size : 0;
        j->hash = h;
        if (stage == ST_CS) {
            memcpy(j->cs_local, cs_local, sizeof j->cs_local);
            j->shared = shared;
        }
    }
    pm_unlock(&g_prq_lk);
}
/* j's stage for key: compiled into the cache. Its input mask comes back for the vertex stage's second key */
static int prep_key(const PrepJob* j, const u8* code, const NvnStageKey* key, u32* in_mask) {
    ShaderInfo si;
    size_t words = 0;
    u32* spv;
    VkShaderModule m = VK_NULL_HANDLE;
    vk_lock();
    /* compute code comes without a program header, as cmdlist.c records it: framed as vk_nvn_shader frames it */
    spv = stage_spirv(j->stage, code, j->window, j->stage == ST_CS, j->cb, j->cb_size,
                      j->stage == ST_CS ? j->cs_local : NULL, j->shared, key, &si, &words);
    vk_unlock();
    if (!spv) return 0;
    *in_mask = si.in_mask;
    {
        static int show = -1; /* SNX_NVN_PREP_LOG=1: each prepared stage's SPIR-V hash, to match against draws' */
        if (show < 0) show = getenv("SNX_NVN_PREP_LOG") && getenv("SNX_NVN_PREP_LOG")[0] == '1';
        if (show) {
            u32 h = 2166136261u;
            size_t w;
            for (w = 0; w < words; w++) h = (h ^ spv[w]) * 16777619u;
            LOG("nvn-live: prepared stage %d: %08x (clip %x, int %x, unfed %x)\n", j->stage, h, key->clip,
                key->int_mask, key->unfed);
        }
    }
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = spv};
    if (vkCreateShaderModule(g_vk.dev, &mi, NULL, &m) == VK_SUCCESS) {
        if (j->stage == ST_CS) { /* as vk_nvn_compute_pipeline makes it on the draw thread, there 150-180 ms cold */
            VkPipeline p = VK_NULL_HANDLE;
            VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                        .module = m,
                                                        .pName = "main"},
                                              .layout = g_vk_playout};
            if (vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p) == VK_SUCCESS)
                vkDestroyPipeline(g_vk.dev, p, NULL);
        } else {
            vk_pipeline_prep(m, j->stage == ST_VS);
        }
        vkDestroyShaderModule(g_vk.dev, m, NULL);
    }
    free(spv);
    return 1;
}
static void prep_worker(void* arg) {
    (void)arg;
    if (!g_prep_run) vk_thread_low();
    for (;;) {
        PrepJob j;
        NvnStageKey key;
        const u8* code;
        u32 in_mask = 0;
        u64 t0;
        if (__atomic_load_n(&g_prep_heavy, __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&g_pr_paused, 1, __ATOMIC_RELAXED);
            plat_sleep_ns(50000000);
            continue;
        }
        pm_lock(&g_prq_lk);
        if (g_prq_in == g_prq_out) {
            pm_unlock(&g_prq_lk);
            plat_sleep_ns(100000000);
            continue;
        }
        j = g_prq[g_prq_out++ % PREP_QUEUE];
        g_prep_busy++;
        pm_unlock(&g_prq_lk);
        code = gmem_ptr_untracked(j.code, j.window);
        /* the game may have freed the program since: only what is still the same bytes */
        if (!code || prep_hash(j.stage, code, j.window, j.cb ? gmem_ptr_untracked(j.cb, j.cb_size) : NULL, j.cb_size,
                               j.cs_local, j.shared) != j.hash) {
            pm_lock(&g_prq_lk);
            g_pr_gone++;
            g_prep_busy--;
            prep_forget(j.hash);
            pm_unlock(&g_prq_lk);
            continue;
        }
        t0 = plat_ns();
        memset(&key, 0, sizeof key);
        if (j.stage == ST_VS) key.clip = PREP_VS_CLIP;
        if (prep_key(&j, code, &key, &in_mask) && j.stage == ST_VS && (in_mask & 4)) {
            key.int_mask = 4; /* skinned draws read attribute 2 as integers */
            prep_key(&j, code, &key, &in_mask);
        }
        pm_lock(&g_prq_lk);
        g_pr_done++;
        g_prep_busy--;
        g_pr_ns += plat_ns() - t0;
        g_prep_quiet = plat_ns();
        if (g_prf) {
            fwrite(&j.hash, sizeof j.hash, 1, g_prf);
            if (!(g_pr_done & 255)) {
                fflush(g_prf);
                vk_pipeline_mark_dirty();
            }
        }
        if (g_prq_in == g_prq_out) {
            if (g_prf) fflush(g_prf);
            vk_pipeline_mark_dirty();
        }
        pm_unlock(&g_prq_lk);
    }
}
/* each frame: starts the workers once Vulkan is up, and stops them for a heavy frame */
static void prep_frame(u32 draws) {
    if (g_prep_on == 1 && g_prep_run && g_prep_started) {
        static u64 shown;
        char path[1300];
        FILE* f;
        pm_lock(&g_prq_lk);
        if (g_pr_done != shown) { /* for the launcher's bar */
            shown = g_pr_done;
            snprintf(path, sizeof path, "%s/prepare_progress.txt", vk_pipeline_cache_dir());
            f = fopen(path, "w");
            if (f) {
                fprintf(f, "%llu %u\n", (unsigned long long)g_pr_done, g_prq_in - g_prq_out);
                fclose(f);
            }
        }
        /* the game gives its stages over its first half minute: done once none came or finished for 20 s */
        if (g_prq_in == g_prq_out && !g_prep_busy && plat_ns() - g_prep_quiet > 20000000000ull) {
            if (g_prf) fflush(g_prf);
            LOG("nvn-live: shader preparation finished: %llu stages\n", (unsigned long long)g_pr_done);
            snprintf(path, sizeof path, "%s/prepared.txt", vk_pipeline_cache_dir());
            f = fopen(path, "w");
            if (f) {
                fprintf(f, "%08x %d\n", g_vk.props.driverVersion, prep_mode());
                fclose(f);
            }
            vk_pipeline_mark_dirty();
            g_quit = 1;
        }
        pm_unlock(&g_prq_lk);
        return;
    }
    __atomic_store_n(&g_prep_heavy, draws >= PREP_HEAVY, __ATOMIC_RELAXED);
    if (g_prep_started || g_prep_on != 1 || !g_vk.has_gpl) return;
    g_prep_started = 1;
    {
        char path[1300];
        const char* e = getenv("SNX_NVN_PREP_WORKERS");
        const char* pass = getenv("SNX_NVN_PREP_RUN");
        int i, workers;
        FILE* f;
        u64 h;
        g_prep_run = pass && pass[0] == '1';
        workers = e && atoi(e) > 0 ? atoi(e) : g_prep_run ? vk_cpu_threads() - 2 : vk_cpu_threads() / 2;
        if (workers < 1) workers = 1;
        g_prep_quiet = plat_ns();
        snprintf(path, sizeof path, "%s/gpu.txt", vk_pipeline_cache_dir());
        f = fopen(path, "w");
        if (f) { /* for the launcher: what this run's shaders are for */
            fprintf(f, "%08x %d\n", g_vk.props.driverVersion, prep_mode());
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/prepared_%08x_%d.bin", vk_pipeline_cache_dir(), g_vk.props.driverVersion,
                 prep_mode());
        pm_lock(&g_prq_lk);
        f = fopen(path, "rb");
        if (f) { /* stages done by earlier runs: what the queue holds of them goes without a compile */
            u32 k, n = 0, kept = 0;
            static u64 old[PREP_SET / 2];
            while (n < PREP_SET / 2 && fread(&h, sizeof h, 1, f) == 1) old[n++] = h;
            fclose(f);
            if (g_prseen) {
                /* queue first: the set holds these already, so they are told apart by a second look through old */
                for (k = g_prq_out; k != g_prq_in; k++) {
                    u32 m;
                    int known = 0;
                    h = g_prq[k % PREP_QUEUE].hash;
                    for (m = 0; m < n && !known; m++) known = old[m] == h;
                    if (!known) g_prq[(g_prq_out + kept++) % PREP_QUEUE] = g_prq[k % PREP_QUEUE];
                }
                g_prq_in = g_prq_out + kept;
                for (k = 0; k < n; k++) prep_seen(old[k]);
            }
        }
        g_prf = fopen(path, "ab");
        LOG("nvn-live: shader preparation: %u stages to do on %d workers, %s\n", g_prq_in - g_prq_out, workers, path);
        pm_unlock(&g_prq_lk);
        for (i = 0; i < workers; i++)
            if (plat_thread(prep_worker, NULL, (size_t)1 << 20)) break;
    }
}
int vk_nvn_shader_io(int mod, u32* in_mask, u32* out_mask, u32* frag_outputs) {
    if (mod < 0 || mod >= g_nmod) return 0;
    *in_mask = g_mod[mod].si.in_mask;
    *out_mask = g_mod[mod].si.out_mask;
    *frag_outputs = g_mod[mod].si.sph[18] != 0;
    return 1;
}

/* ------------------------------------------------------------ formats (phase 3 step 3c part 2) */
/* NVNformat to Vulkan, after B2recomp's runtime/nvn_vk_draw.c (comp_fmt, tex_vkfmt, vertex_vkfmt),
   GPL-3.0-or-later. Formats 0x01-0x30 are 1-4 components (12 each) of the kinds in comp_fmt's table, 0x6D-0x78 the
   4-component ones again */
static VkFormat comp_fmt(int comps, int i) {
    static const int b8[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM,
                              VK_FORMAT_R8G8B8A8_UNORM};
    static const int b16[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM,
                               VK_FORMAT_R16G16B16A16_UNORM};
    static const int b32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                               VK_FORMAT_R32G32B32A32_UINT};
    /* width and offset from UNORM (8, 16 bits: SNORM 1, USCALED 2, SSCALED 3, UINT 4, SINT 5, SFLOAT 6) or from
       UINT (32 bits: SINT 1, SFLOAT 2) */
    static const struct {
        u8 w, o;
    } m[12] = {{8, 0}, {8, 1}, {8, 4}, {8, 5}, {16, 6}, {16, 0}, {16, 1}, {16, 4}, {16, 5}, {32, 2}, {32, 0}, {32, 1}};
    return (VkFormat)((m[i].w == 8 ? b8 : m[i].w == 16 ? b16 : b32)[comps - 1] + m[i].o);
}
static int comp_kind(u32 nf, int* comps) { /* the kind (0-11) of a component format, -1 for another */
    if (nf >= 0x01 && nf <= 0x30) return *comps = (int)(nf - 1) / 12 + 1, (int)(nf - 1) % 12;
    if (nf >= 0x6D && nf <= 0x78) return *comps = 4, (int)(nf - 0x6D);
    return -1;
}
u32 vk_nvn_attr_int(u32 nf) {
    int comps, i = comp_kind(nf, &comps);
    if (i >= 0) return i == 2 || i == 7 || i == 10 ? 1 : i == 3 || i == 8 || i == 11 ? 3 : 0;
    return nf == 0x3E ? 1 : nf == 0x6A ? 3 : 0;
}
u32 vk_nvn_color_int(u32 nf) { return vk_nvn_attr_int(nf) != 0; }
static VkFormat vertex_fmt(u32 nf) {
    int comps, i = comp_kind(nf, &comps);
    if (i >= 0) return comp_fmt(comps, i);
    if (nf >= 0x51 && nf <= 0x68) { /* scaled: 8 and 16 bits; Vulkan has no 32-bit ones */
        int t = (int)(nf - 0x51) % 6;
        comps = (int)(nf - 0x51) / 6 + 1;
        return t >= 4 ? VK_FORMAT_UNDEFINED : (VkFormat)(comp_fmt(comps, t < 2 ? 0 : 5) + 2 + (t & 1));
    }
    switch (nf) {
        case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
        case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
        case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}
static int fmt_has(VkFormat f, int buffer, VkFormatFeatureFlags need);
/* AMD renders to neither 24-bit depth format: 32-bit float depth holds every 24-bit value, as format.c does for the
   Maxwell path. SNX_NVN_NO_D24=1 takes it on any GPU, to test that path */
static VkFormat depth24_fmt(VkFormat d24, VkFormat d32) {
    static int no_d24 = -1;
    if (no_d24 < 0) {
        const char* e = getenv("SNX_NVN_NO_D24");
        no_d24 = e && e[0] == '1';
    }
    return !no_d24 && fmt_has(d24, 0, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? d24 : d32;
}
static VkFormat target_fmt(u32 nf) { /* render targets: colour, and depth or stencil */
    int comps, i = comp_kind(nf, &comps);
    if (i >= 0) return comp_fmt(comps == 3 ? 4 : comps, i);
    switch (nf) {
        case 0x31: return VK_FORMAT_S8_UINT;
        case 0x32: return VK_FORMAT_D16_UNORM;
        case 0x33: return depth24_fmt(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT);
        case 0x34: return VK_FORMAT_D32_SFLOAT;
        case 0x35: return depth24_fmt(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT);
        case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case 0x37:
        case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
        case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
        case 0x3A:
        case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
        case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16;
        case 0x99:
        case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
        case 0x9B:
        case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}
static int fmt_has(VkFormat f, int buffer, VkFormatFeatureFlags need) { /* cached by format */
    static u8 seen[2][256][2]; /* [buffer][format][0 known, 1 has] for the core formats */
    VkFormatProperties p;
    if ((u32)f < 256 && seen[buffer][f][0]) return seen[buffer][f][1];
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int has = ((buffer ? p.bufferFeatures : p.optimalTilingFeatures) & need) == need;
    if ((u32)f < 256) seen[buffer][f][0] = 1, seen[buffer][f][1] = (u8)has;
    return has;
}
static VkBlendFactor blend_factor(u32 f) { /* NVNblendFunc, as B2recomp's blend_factor */
    static const u8 m[9] = {
        VK_BLEND_FACTOR_SRC_COLOR,           VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,           VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,           VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (f >= 3 && f <= 0xB) return (VkBlendFactor)m[f - 3];
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(u32 e) { /* NVNblendEquation: 1 add, 2 subtract, 3 reverse subtract, 4 min, 5 max */
    static const u8 m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                            VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return (VkBlendOp)m[e < 6 ? e : 1];
}

/* ------------------------------------------------------------ pipelines (phase 3 step 3c part 2) */
/* A draw's NvnPipeDesc becomes the renderer's PipeKey: its pipeline layout and descriptor set layouts, its dynamic
   state (viewport, scissor, cull, front face, depth and stencil tests, depth bias, blend constants, vertex strides) and
   its pipeline cache. Attribute i reads location i; each stream an attribute uses is a binding, in the order first
   met, and attributes the vertex program reads but the state leaves off read a constant binding placed last, as the
   renderer's draws do. Results are kept per desc (callers hold one lock: cmdlist.c's) */
typedef struct NvnPipe {
    NvnPipeDesc d;
    u64 h;
    VkPipeline p;
    PipeKey k;
    int pending;                   /* a worker is making it: p and why are set when this goes to 0 */
    u64 t_queued, t_begun, t_done; /* ns: into the queue, a worker took it, made */
    u32 nbind, bstream[17];        /* each vertex binding's stream, ~0u for the constant one */
    char why[48];
    struct NvnPipe* next;
} NvnPipe;
static NvnPipe* g_pipe[1024];
/* With real writes pipelines are made off the draw thread, which holds vk_lock for a whole list: a frame of first-time
   pipelines stood still for tens of ms (47 in one frame in play, 2.3 ms each). A draw waits for its pipeline within the
   frame's budget (SNX_NVN_PIPE_BUDGET_MS, default 33, 0 makes them on the draw thread), and past it a draw that only
   draws is left out until the pipeline is there. A left out draw shows: at 3 ms a whole level's materials or a menu's
   background went black for a frame, though the driver's cache held every pipeline. 33 lost none of those, and on
   empty caches (about 60 ms a pipeline) lost a quarter of what 3 did for no more time stalled */
#define PIPE_QUEUE   2048
#define PIPE_WORKERS 4
static NvnPipe* g_pq[PIPE_QUEUE];
static u32 g_pq_in, g_pq_out;
static PMutex g_pq_lk = PMUTEX_INIT;
static PCond g_pq_cv;
static int g_pq_on;
/* since the last log line, under g_pq_lk: pipelines made, their time in the queue and compiling, the longest of each;
   moved to the front; the most queued. Draws' waits are counted by the draw thread */
static struct {
    u64 made, queue_ns, make_ns, queue_max, make_max, front;
    u32 depth;
} g_ps;
static u64 g_pw_n, g_pw_ns, g_pw_max;
static void pipe_worker(void* arg) {
    (void)arg;
    for (;;) {
        NvnPipe* p;
        VkPipeline v;
        u64 q, m;
        pm_lock(&g_pq_lk);
        while (g_pq_in == g_pq_out) pc_wait(&g_pq_cv, &g_pq_lk, -1);
        p = g_pq[g_pq_out++ % PIPE_QUEUE];
        p->t_begun = plat_ns();
        pm_unlock(&g_pq_lk);
        v = vk_pipeline_make(&p->k);
        p->p = v;
        if (!v) snprintf(p->why, sizeof p->why, "vkCreateGraphicsPipelines failed");
        p->t_done = plat_ns();
        q = p->t_begun - p->t_queued;
        m = p->t_done - p->t_begun;
        if (m > 30000000ull) { /* a real compile, not the driver's cache: what shader preparation didn't have */
            const ShaderInfo* v = p->d.mod[0] >= 0 ? &g_mod[p->d.mod[0]].si : NULL;
            const ShaderInfo* f = p->d.mod[4] >= 0 ? &g_mod[p->d.mod[4]].si : NULL;
            LOG_LIMIT(400,
                      "nvn-live: a pipeline took %.1f ms to make: vertex %08x (clip %x, int %x), fragment %08x "
                      "(int rt %x, int tex %llx, unfed %x, dc %llx)\n",
                      (double)m / 1e6, v ? g_mod[p->d.mod[0]].spv_hash : 0, v ? v->clip_enable : 0,
                      v ? v->attr_int_mask : 0, f ? g_mod[p->d.mod[4]].spv_hash : 0, f ? f->rt_int_mask : 0,
                      f ? (unsigned long long)f->tex_int_mask : 0, f ? f->in_unfed_mask : 0,
                      f ? (unsigned long long)f->tex_dc_color_mask : 0);
        }
        pm_lock(&g_pq_lk);
        g_ps.made++;
        g_ps.queue_ns += q;
        g_ps.make_ns += m;
        if (q > g_ps.queue_max) g_ps.queue_max = q;
        if (m > g_ps.make_max) g_ps.make_max = m;
        pm_unlock(&g_pq_lk);
        __atomic_store_n(&p->pending, 0, __ATOMIC_RELEASE);
    }
}
/* a draw stands waiting for p: it goes next. Returns how many were ahead of it */
static u32 pipe_front(NvnPipe* p) {
    static int on = -1;
    u32 i, ahead = 0;
    if (on < 0) {
        const char* e = getenv("SNX_NVN_PIPE_FRONT");
        on = !(e && e[0] == '0');
    }
    pm_lock(&g_pq_lk);
    for (i = g_pq_out; i != g_pq_in; i++)
        if (g_pq[i % PIPE_QUEUE] == p) {
            ahead = i - g_pq_out;
            if (on && ahead) {
                g_pq[i % PIPE_QUEUE] = g_pq[g_pq_out % PIPE_QUEUE];
                g_pq[g_pq_out % PIPE_QUEUE] = p;
                g_ps.front++;
            }
            break;
        }
    pm_unlock(&g_pq_lk);
    return ahead;
}
static u64 pipe_budget_ns(void) {
    static i64 ms = -1;
    if (ms < 0) {
        const char* e = getenv("SNX_NVN_PIPE_BUDGET_MS");
        ms = e ? atoi(e) : 33;
    }
    return (u64)ms * 1000000;
}
static int pipe_enqueue(NvnPipe* p) { /* 1 when a worker has it */
    int queued = 0;
    pm_lock(&g_pq_lk);
    if (!g_pq_on) {
        int i;
        g_pq_on = 1;
        pc_init(&g_pq_cv);
        const char* e = getenv("SNX_NVN_PIPE_WORKERS");
        int workers = e && atoi(e) > 0 ? atoi(e) : PIPE_WORKERS;
        for (i = 0; i < workers; i++)
            if (plat_thread(pipe_worker, NULL, (size_t)1 << 20)) fatal("nvn: could not start a pipeline worker\n");
    }
    if (g_pq_in - g_pq_out < PIPE_QUEUE) {
        p->pending = 1;
        p->t_queued = plat_ns();
        g_pq[g_pq_in++ % PIPE_QUEUE] = p;
        if (g_pq_in - g_pq_out > g_ps.depth) g_ps.depth = g_pq_in - g_pq_out;
        queued = 1;
        pc_broadcast(&g_pq_cv);
    }
    pm_unlock(&g_pq_lk);
    return queued;
}
static int pipe_key(const NvnPipeDesc* d, PipeKey* k, u32* bstream, char* why, u32 why_size) {
    VkShaderModule m[5];
    u32 in_mask = 0, int_mask = 0, sint_mask = 0, s, i, cbind = ~0u;
    VkPrimitiveTopology topo;
    memset(k, 0, sizeof *k);
    for (s = 0; s < 5; s++) {
        int x = d->mod[s];
        m[s] = x >= 0 && x < g_nmod ? g_mod[x].mod : VK_NULL_HANDLE;
        if (x >= g_nmod) return snprintf(why, why_size, "module number out of range"), 0;
    }
    if (!m[0]) return snprintf(why, why_size, "no vertex program"), 0;
    if (!m[1] != !m[2]) return snprintf(why, why_size, "tessellation with one of its programs"), 0;
    if ((m[1] || m[3]) && g_vk_stage_sets < 5) return snprintf(why, why_size, "too few descriptor sets"), 0;
    if (d->samples > 1) return snprintf(why, why_size, "multisampled (%u)", d->samples), 0;
    in_mask = g_mod[d->mod[0]].si.in_mask;
    int_mask = g_mod[d->mod[0]].si.attr_int_mask;
    sint_mask = g_mod[d->mod[0]].si.attr_sint_mask;
    k->vs = m[0];
    k->tcs = m[1];
    k->tes = m[2];
    k->gs = m[3];
    k->fs = m[4];
    switch (d->primitive) { /* NVNdrawPrimitive: the Maxwell topologies' numbers */
        case 0: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 1: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 2:
        case 3: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case 5:
        case 8: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case 6:
        case 9: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        case 10: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; break;
        case 11: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; break;
        case 12: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; break;
        case 13: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; break;
        case 14: topo = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
        default: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }
    if (k->tes) {
        topo = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        k->patch = d->patch ? d->patch : 1;
        if (k->patch > g_vk.props.limits.maxTessellationPatchSize)
            k->patch = g_vk.props.limits.maxTessellationPatchSize;
    } else if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
        return snprintf(why, why_size, "patches without tessellation"), 0;
    if (topo >= VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY && topo < VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && !k->gs)
        return snprintf(why, why_size, "adjacency without a geometry program"), 0;
    k->topo_class = topo >= VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ? topo
                    : topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST             ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                    : topo <= VK_PRIMITIVE_TOPOLOGY_LINE_STRIP             ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k->polygon = !g_vk.feat.fillModeNonSolid ? VK_POLYGON_MODE_FILL
                 : d->polygon == 0           ? VK_POLYGON_MODE_POINT
                 : d->polygon == 1           ? VK_POLYGON_MODE_LINE
                                             : VK_POLYGON_MODE_FILL;
    k->depth_clamp = g_vk.feat.depthClamp && d->depth_clamp;
    /* vertex input */
    for (i = 0; i < 32; i++) {
        u32 nf, st, b;
        VkFormat f;
        if (!(in_mask >> i & 1)) continue;
        nf = i < d->nattr && i < 16 ? d->attr_format[i] : 0;
        st = i < 16 ? d->attr_stream[i] : 0;
        f = nf ? vertex_fmt(nf) : VK_FORMAT_UNDEFINED;
        if (nf && (!f || !fmt_has(f, 1, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)))
            return snprintf(why, why_size, "vertex format %#x", nf), 0;
        if (f && st < 16) {
            for (b = 0; b < k->nbind && bstream[b] != st; b++) {}
            if (b == k->nbind) {
                u32 div = st < d->nstream ? d->stream_divisor[st] : 0;
                if (div > 1 && !g_vk.has_divisor) div = 1;
                bstream[b] = st;
                k->bind[b].divisor = div;
                k->nbind++;
            }
            k->attr[k->nattr].loc = i;
            k->attr[k->nattr].binding = b;
            k->attr[k->nattr].format = (u32)f;
            k->attr[k->nattr].offset = d->attr_offset[i];
        } else { /* 0, 0, 0, 1: the integer copy at 16 for integer inputs */
            int is_int = (int_mask >> i) & 1;
            if (cbind == ~0u) {
                cbind = k->nbind;
                bstream[cbind] = ~0u;
                k->bind[cbind].divisor = 0;
                k->nbind++;
            }
            k->attr[k->nattr].loc = i;
            k->attr[k->nattr].binding = cbind;
            k->attr[k->nattr].format =
                is_int ? ((sint_mask >> i) & 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                       : VK_FORMAT_R32G32B32A32_SFLOAT;
            k->attr[k->nattr].offset = is_int ? 16 : 0;
        }
        k->nattr++;
        if (k->nbind > g_vk.props.limits.maxVertexInputBindings ||
            k->nattr > g_vk.props.limits.maxVertexInputAttributes)
            return snprintf(why, why_size, "past the device's vertex input limits"), 0;
    }
    /* attachments and blending */
    if (d->ncolor > 8) return snprintf(why, why_size, "%u colour targets", d->ncolor), 0;
    k->ncolor = d->ncolor;
    for (i = 0; i < d->ncolor; i++) {
        const u8* b = d->blend[i];
        u32 cm = d->chanmask >> (i * 4) & 15;
        VkFormat f;
        if (!d->color_format[i]) continue;
        f = target_fmt(d->color_format[i]);
        if (!f || !fmt_has(f, 0, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
            return snprintf(why, why_size, "colour target format %#x", d->color_format[i]), 0;
        k->color[i] = (u32)f;
        k->blend[i].mask = cm; /* r, g, b, a: VK_COLOR_COMPONENT_R_BIT.. in the same order */
        if ((d->blend_on >> i & 1) && !vk_nvn_color_int(d->color_format[i])) {
            k->blend[i].en = 1;
            k->blend[i].sc = blend_factor(b[0]);
            k->blend[i].dc = blend_factor(b[1]);
            k->blend[i].sa = blend_factor(b[2]);
            k->blend[i].da = blend_factor(b[3]);
            k->blend[i].cop = blend_op(b[4]);
            k->blend[i].aop = blend_op(b[5]);
        }
    }
    /* dither=3: a fading object blends by the alpha its stage now writes as the second source (dither.c), where the
       game draws it unblended into one target, the most dual-source blending allows */
    if (d->mod[4] >= 0 && g_mod[d->mod[4]].si.dither_blend && d->ncolor == 1 && k->color[0] && !k->blend[0].en &&
        g_vk.props.limits.maxFragmentDualSrcAttachments >= 1) {
        k->blend[0].en = 1;
        k->blend[0].sc = k->blend[0].sa = VK_BLEND_FACTOR_SRC1_ALPHA;
        k->blend[0].dc = k->blend[0].da = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        k->blend[0].cop = k->blend[0].aop = VK_BLEND_OP_ADD;
    }
    if (d->depth_format) {
        VkFormat f = target_fmt(d->depth_format);
        if (!f || !fmt_has(f, 0, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            return snprintf(why, why_size, "depth target format %#x", d->depth_format), 0;
        k->depth = f == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : f;
        k->stencil = f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT
                         ? f
                         : VK_FORMAT_UNDEFINED;
    }
    return 1;
}
static int g_rp_real; /* g_rp.real, which is defined further on */
static int g_rp_win;  /* a host window is open (vk_nvn_live_window): g_rp.cb is recording and may be submitted */
static NvnPipe* pipe_get(const NvnPipeDesc* d, int* made) { /* the cache entry for d, made now when new */
    u64 h = vk_hash(d, sizeof *d);
    NvnPipe* p;
    PipeKey k;
    *made = 0;
    for (p = g_pipe[h & 1023]; p; p = p->next)
        if (p->h == h && !memcmp(&p->d, d, sizeof *d)) break;
    if (!p) {
        p = calloc(1, sizeof *p);
        if (!p) return NULL;
        p->d = *d;
        p->h = h;
        if (pipe_key(d, &k, p->bstream, p->why, sizeof p->why)) {
            p->nbind = k.nbind;
            p->k = k;
            if (g_rp_real && pipe_budget_ns() && pipe_enqueue(p))
                *made = 1;
            else {
                u64 t0 = plat_ns(), t;
                vk_lock(); /* the renderer's pipeline table and cache */
                p->p = vk_pipeline_get(&k);
                vk_unlock();
                t = plat_ns() - t0;
                if (t > 5000000ull)
                    LOG_LIMIT(400,
                              "nvn-live: a pipeline took %.1f ms to make on the draw thread: vertex %08x, fragment "
                              "%08x\n",
                              (double)t / 1e6, d->mod[0] >= 0 ? g_mod[d->mod[0]].spv_hash : 0,
                              d->mod[4] >= 0 ? g_mod[d->mod[4]].spv_hash : 0);
                if (!p->p) snprintf(p->why, sizeof p->why, "vkCreateGraphicsPipelines failed");
                *made = p->p != VK_NULL_HANDLE;
            }
        }
        p->next = g_pipe[h & 1023];
        g_pipe[h & 1023] = p;
    }
    return p;
}
int vk_nvn_pipeline(const NvnPipeDesc* d, int* made, char* why, u32 why_size) {
    NvnPipe* p = pipe_get(d, made);
    if (!p) return snprintf(why, why_size, "out of memory"), 0;
    if (__atomic_load_n(&p->pending, __ATOMIC_ACQUIRE)) return 1;
    if (!p->p) snprintf(why, why_size, "%s", p->why);
    return p->p != VK_NULL_HANDLE;
}
static VkPipeline g_cpipe[MODULES]; /* compute pipelines by module (the replay dispatches with them) */
int vk_nvn_compute_pipeline(int mod, int* made, char* why, u32 why_size) {
    VkPipeline* pipe = g_cpipe;
    static u8 tried[MODULES];
    *made = 0;
    if (mod < 0 || mod >= g_nmod) return snprintf(why, why_size, "no compute module"), 0;
    if (!tried[mod]) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = g_mod[mod].mod,
                                                    .pName = "main"},
                                          .layout = g_vk_playout};
        u64 t0 = plat_ns(), t;
        tried[mod] = 1;
        vk_lock();
        if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &pipe[mod])))
            pipe[mod] = VK_NULL_HANDLE;
        vk_unlock();
        *made = pipe[mod] != VK_NULL_HANDLE;
        t = plat_ns() - t0;
        if (t > 5000000ull) /* made on the draw thread: what shader preparation missed */
            LOG_LIMIT(400, "nvn-live: a compute pipeline took %.1f ms to make on the draw thread: %08x\n",
                      (double)t / 1e6, g_mod[mod].spv_hash);
    }
    if (!pipe[mod]) snprintf(why, why_size, "vkCreateComputePipelines failed");
    return pipe[mod] != VK_NULL_HANDLE;
}

/* n bytes at off of buf copied on the GPU (a command buffer and fence of its own, submitted under vk_lock) into host
   memory, and compared with guest memory at cpu + off: 1 equal, 0 different, -1 when the check couldn't run */
int vk_nvn_pool_check(int which, u64 off, u32 n) {
    VkBuffer rb = VK_NULL_HANDLE;
    VmaAllocation rm = NULL;
    VmaAllocationInfo ri;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int result = -1;
    VkBuffer buf;
    u64 cpu;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = n,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (which < 0 || which >= IMPORTS || !import_wait(which)) return -1;
    vk_lock();
    if (which >= g_nimport || off + n > g_import[which].size) {
        vk_unlock();
        return -1;
    }
    buf = g_import[which].buf;
    cpu = g_import[which].cpu;
    vk_unlock();
    if (vmaCreateBuffer(g_vk.vma, &bci, &aci, &rb, &rm, &ri) != VK_SUCCESS) return -1;
    if (vkCreateCommandPool(g_vk.dev, &pci, NULL, &pool) != VK_SUCCESS || vkCreateFence(g_vk.dev, &fci, NULL, &fence))
        goto done;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkAllocateCommandBuffers(g_vk.dev, &cai, &cb) != VK_SUCCESS || vkBeginCommandBuffer(cb, &cbi) != VK_SUCCESS)
        goto done;
    VkBufferCopy region = {.srcOffset = off, .dstOffset = 0, .size = n};
    vkCmdCopyBuffer(cb, buf, rb, 1, &region);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) goto done;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_lock(); /* the queue is the renderer's: its submissions hold the same lock */
    vk_queue_lock();
    VkResult r = vkQueueSubmit(g_vk.gfx, 1, &si, fence);
    vk_queue_unlock();
    vk_unlock();
    if (r != VK_SUCCESS || vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) goto done;
    vmaInvalidateAllocation(g_vk.vma, rm, 0, n);
    result = !memcmp(ri.pMappedData, GP(cpu + off), n);
done:
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    if (pool) vkDestroyCommandPool(g_vk.dev, pool, NULL);
    vmaDestroyBuffer(g_vk.vma, rb, rm);
    return result;
}

/* ------------------------------------------------------------ descriptors (phase 3 step 3c part 2c) */
/* Where NVN's programs find what they bind, as B2recomp's front end and translator place it (runtime/nvn_vk_draw.c,
   mx_translate.c; SDK 3.6) and the slot table below checks for SDK 13.3.2: constant buffer 0 holds the storage
   buffers' {address, size} at 0x110 + 0x100 * the hardware stage (vertex 0, tess control 1, tess eval 2, geometry 3,
   fragment 4) and 0x310 for compute, 16 bytes each; 1 the program's constants; 2 the bound texture handles at word
   8 + 2 * unit; 3 and up uniform buffer bindings 0 and up. Textures resolve through the renderer: the handle's TIC
   (bits 0-19) and TSC (bits 20-31) from the pools, vk_tex_from_tic, the view and sampler its draws use */
static const u32 g_hw_stage[6] = {0, 4, 3, 1, 2, 0}; /* NVNshaderStage -> the hardware's order */
#define SLOTS 64
static struct {
    u8 kind, stage, bank; /* kind: 0 texture, 1 image, 2 storage buffer address */
    u32 word;
    u64 n;
} g_slot[SLOTS];
static u32 g_nslot, g_slot_full;
static u64 g_null_views; /* textures bound as their declared type's null texture */
static void slot_seen(int kind, int stage, u32 bank, u32 word) {
    u32 i;
    for (i = 0; i < g_nslot; i++)
        if (g_slot[i].kind == kind && g_slot[i].stage == stage && g_slot[i].bank == bank && g_slot[i].word == word) {
            g_slot[i].n++;
            return;
        }
    if (g_nslot == SLOTS) {
        g_slot_full++;
        return;
    }
    g_slot[g_nslot].kind = (u8)kind;
    g_slot[g_nslot].stage = (u8)stage;
    g_slot[g_nslot].bank = (u8)bank;
    g_slot[g_nslot].word = word;
    g_slot[g_nslot++].n = 1;
}
void vk_nvn_slots_dump(void) {
    static const char* const kind[3] = {"texture handle", "image handle", "storage buffer address"};
    u32 i;
    for (i = 0; i < g_nslot; i++)
        LOG("nvn-res: %s in stage %u's constant buffer %u word %u (byte %#x): %llu bindings\n", kind[g_slot[i].kind],
            g_slot[i].stage, g_slot[i].bank, g_slot[i].word, g_slot[i].word * 4, (unsigned long long)g_slot[i].n);
    if (g_slot_full) LOG("nvn-res: slot table full, %u more not listed\n", g_slot_full);
    LOG("nvn-res: textures bound as the declared type's null texture (not viewable as declared): %llu\n",
        (unsigned long long)g_null_views);
}

/* the 32-bit word at word of constant buffer bank as b binds it: 1, or 0 with *why */
static int bank_word(const NvnStageBind* b, u32 bank, u32 word, u32* out, const char** why) {
    u64 a = 0, size = 0, off;
    int n;
    if (bank == 2) { /* the host's: bound texture handles from word 8, image handles from word 72 (the slot table) */
        u32 unit = (word - 8) / 2;
        if (word < 8 || (word & 1) || unit >= 40) return *why = "constant buffer 2 word outside the units", 0;
        *out = unit < 32 ? (u32)b->tex[unit] : (u32)b->img[unit - 32];
        return 1;
    }
    if (bank == 1)
        a = b->cb1, size = b->cb1_size;
    else if (bank >= 3 && bank < 19)
        a = b->ubo[bank - 3].addr, size = b->ubo[bank - 3].size;
    else
        return *why = "a handle in constant buffer 0", 0;
    if (!a || (u64)word * 4 + 4 > size) return *why = "a handle past its constant buffer", 0;
    n = import_of(a + (u64)word * 4, 4, &off);
    if (n < 0) return *why = "a handle outside the pools", 0;
    memcpy(out, GP(g_import[n].cpu + off), 4);
    return 1;
}
/* textures by handle, kept for the frame (vk_nvn_frame): the renderer's texture, its descriptors, and a view and
   sampler for a declaration (view type, number type, shadow, raw) */
#define TCACHE 4096u
static struct {
    u64 key;
    u32 gen;
    Surf* t;
    u32 tic[8], tsc[8];
    VkImageView view;
    VkSampler smp;
    int null_view; /* view is the null texture of the declared type */
    const char* why;
} g_tc[TCACHE];
static void rp_live_miss(void); /* a texture to look up: live frames leave their pass first (it may upload) */
static u32 g_tgen = 1;
void vk_nvn_frame(void) { g_tgen++; }
static u64 mix64(u64 h, u64 v) { return (h ^ v) * 0x100000001b3ull + (v >> 29); }
/* st NULL: the texture and its descriptors only (for keys). An image (image set) has no sampler: its view is 2D
   and the texture must be one a shader can store to. 1, or 0 with the reason in *why */
static u32 g_view_logged, g_fr_miss;
static u64 g_fr_tex, g_fr_astc;       /* this frame: texture lookups, ns in them; ASTC texels decoded so far */
static u64 g_fr_null, g_fr_null_area; /* this frame: lookups of textures put off to a later present, the largest */
static int tex_get(const NvnStageBind* b, u32 handle, const ShTex* st, int num, int image, int* slot,
                   const char** why) {
    u64 key = mix64(mix64(mix64(1469598103934665603ull, handle), b->tex_pool), b->smp_pool);
    u32 i, n, id = handle & 0xFFFFF, sid = image ? 0 : handle >> 20;
    if (image) key = mix64(key, 2ull << 40);
    if (st)
        key = mix64(key, 1ull << 40 | (u64)vk_tex_view_type(st) << 8 | (u64)num << 4 | (u64)st->shadow << 1 |
                             (st->flags & 1));
    if (!key) key = 1;
    for (i = (u32)key & (TCACHE - 1), n = 0; n < 8; n++, i = (i + 1) & (TCACHE - 1)) {
        if (g_tc[i].key == key && g_tc[i].gen == g_tgen) {
            *slot = (int)i;
            *why = g_tc[i].why;
            return !g_tc[i].why;
        }
        if (g_tc[i].gen != g_tgen) break;
    }
    if (n == 8) i = (u32)key & (TCACHE - 1); /* a crowded run: its first entry goes */
    *slot = (int)i;
    rp_live_miss();
    u64 tex_t0 = plat_ns();
    g_fr_miss++;
    g_tc[i].key = key;
    g_tc[i].gen = g_tgen;
    g_tc[i].t = NULL;
    g_tc[i].view = VK_NULL_HANDLE;
    g_tc[i].smp = VK_NULL_HANDLE;
    g_tc[i].null_view = 0;
    g_tc[i].why = NULL;
    if (!b->tex_pool || id >= b->tex_count)
        g_tc[i].why = "texture id past its pool";
    else if (!image && (!b->smp_pool || sid >= b->smp_count))
        g_tc[i].why = "sampler id past its pool";
    else {
        memcpy(g_tc[i].tic, GP(b->tex_pool + (u64)id * 32), 32);
        if (image)
            memset(g_tc[i].tsc, 0, 32);
        else
            memcpy(g_tc[i].tsc, GP(b->smp_pool + (u64)sid * 32), 32);
        if (g_rp_real) { /* c7: the texture cache reads guest memory: a GPU-written range under it lands first. The
                            size is an upper bound (16 bytes a texel, every layer) */
            const u32* t = g_tc[i].tic;
            u64 gva = ((u64)(t[2] & 0xFFFF) << 32) | t[1];
            u64 bytes = (u64)((t[4] & 0xFFFF) + 1) * ((t[5] & 0xFFFF) + 1) * 16 * (1 + ((t[5] >> 16) & 0x3FFF));
            g_land_why = 1;
            own_land_gpu(gva, bytes < (256ull << 20) ? bytes : 256ull << 20);
        }
        vk_lock();
        g_vk_tex_later = 0;
        g_tc[i].t = st && (st->flags & 1) ? vk_tex_from_tic_raw(g_tc[i].tic) : vk_tex_from_tic(g_tc[i].tic);
        if (!g_tc[i].t && !image) {
            /* empty: decoded at a later present, or a descriptor with no texture (all zero: a slot the game never
               filled), which the renderer's stage_set binds as the null texture and still draws */
            if (g_vk_tex_later) {
                u64 area = (u64)((g_tc[i].tic[4] & 0xFFFF) + 1) * ((g_tc[i].tic[5] & 0xFFFF) + 1);
                g_fr_null++;
                if (area > g_fr_null_area) g_fr_null_area = area;
            } else
                LOG_LIMIT(8, "nvn-res: no texture from TIC %08x %08x %08x %08x %08x %08x %08x %08x: the null texture\n",
                          g_tc[i].tic[0], g_tc[i].tic[1], g_tc[i].tic[2], g_tc[i].tic[3], g_tc[i].tic[4],
                          g_tc[i].tic[5], g_tc[i].tic[6], g_tc[i].tic[7]);
            if (st) {
                VkImageViewType want = vk_tex_view_type(st);
                g_tc[i].smp = vk_tex_bind_sampler(g_tc[i].tsc, st, num, NULL, want);
                g_tc[i].null_view = 1;
                g_tc[i].view = vk_null_view(want, st->shadow ? 3 : num);
                if (!g_tc[i].view || !g_tc[i].smp) g_tc[i].why = "no null texture";
            }
        } else if (!g_tc[i].t)
            g_tc[i].why = "image: vk_tex_from_tic made no texture";
        else if (image) { /* as the renderer's fragment storage images */
            static const VkComponentMapping identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            if (!g_tc[i].t->storage_capable)
                g_tc[i].why = "image: the texture was not made for storage";
            else if (!(g_tc[i].view = vk_tex_view(g_tc[i].t, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0)))
                g_tc[i].why = "image: no 2D view";
        } else if (st) {
            VkImageViewType want = vk_tex_view_type(st);
            g_tc[i].view = vk_tex_bind_view(g_tc[i].t, want, g_tc[i].tic, g_tc[i].tsc, num);
            /* a texture that can't be viewed as declared (a cube sampler on a 2D target, while loading) is the
               declared type's null texture, as the renderer's stage_set binds it */
            g_tc[i].smp = vk_tex_bind_sampler(g_tc[i].tsc, st, num, g_tc[i].view ? g_tc[i].t : NULL, want);
            if (!g_tc[i].view) {
                g_tc[i].null_view = 1;
                g_tc[i].view = vk_null_view(want, st->shadow ? 3 : num);
            }
            if (!g_tc[i].view)
                g_tc[i].why = "no view of the texture as declared, and no null texture";
            else if (!g_tc[i].smp)
                g_tc[i].why = "no sampler";
            if (g_tc[i].null_view && g_view_logged < 8) {
                const Surf* t = g_tc[i].t;
                g_view_logged++;
                LOG("nvn-res: no view: handle %08x, TIC word 0 %08x, declared view type %d number %d shadow %d; texture "
                    "made as view type %d, %ux%u layers %u, format %d, integer %d signed %d depth %d stencil %d\n",
                    handle, g_tc[i].tic[0], (int)want, num, st->shadow, (int)t->type, t->w, t->h, t->layers,
                    (int)t->f.vk, t->f.is_int, t->f.sint, t->f.depth, t->f.stencil);
            }
        }
        vk_unlock();
    }
    g_fr_tex += plat_ns() - tex_t0;
    *why = g_tc[i].why;
    return !g_tc[i].why;
}
static int tex_handle(const NvnStageBind* b, const ShTex* st, u32* handle, const char** why) {
    u32 bank = st->bindless ? st->bl_cbuf : 2, second = 0;
    if (!bank_word(b, bank, st->cbuf_off, handle, why)) return 0;
    if (st->bl2) { /* the texture's and the sampler's parts read apart */
        if (!bank_word(b, st->bl2_cbuf, st->bl2_off, &second, why)) return 0;
        *handle |= second;
    }
    return 1;
}
void vk_nvn_tex_key(int mod, const NvnStageBind* b, NvnStageKey* key) {
    const ShaderInfo* si;
    int i, nt, slot;
    u32 nd = 0;
    if (mod < 0 || mod >= g_nmod) return;
    si = &g_mod[mod].si;
    nt = si->ntex > SH_MAXTEX ? SH_MAXTEX : si->ntex;
    for (i = 0; i < nt; i++) { /* as the renderer's fs_get_keyed */
        const ShTex* st = &si->tex[i];
        const char* why;
        u32 handle, ty;
        const Surf* t;
        if ((st->flags & SNX_TEX_INDEXED) || !tex_handle(b, st, &handle, &why) ||
            !tex_get(b, handle, NULL, 0, 0, &slot, &why))
            continue;
        t = g_tc[slot].t;
        if (st->shadow) {
            if (t && !t->f.depth && !t->f.is_int) {
                u32 func = (g_tc[slot].tsc[0] >> 9) & 1 ? (g_tc[slot].tsc[0] >> 10) & 7 : 3;
                key->dc_mask |= 1ull << i;
                if (nd < SH_DC_COLOR_MAX) key->dc_funcs |= func << (3 * nd);
                nd++;
            }
            continue;
        }
        ty = (g_tc[slot].tic[0] >> 7) & 7;
        if (vk_tic_reads_stencil(g_tc[slot].tic) || (t ? t->f.is_int : (ty == 3 || ty == 4))) {
            key->tex_int |= 1ull << i;
            if (t ? t->f.sint : ty == 3) key->tex_sint |= 1ull << i;
        }
    }
    for (i = 0; i < si->nimg && i < 8; i++) { /* storage images stored in the other kind than the bindless default */
        ShTex as;
        const char* why;
        u32 handle, tic[8];
        if (si->img[i].kind == 1) continue; /* atomics require an unsigned integer image */
        if (si->stage == ST_CS && si->img[i].type != 3 && si->img[i].type != 4) continue; /* 2D and 2D arrays */
        memset(&as, 0, sizeof as);
        as.cbuf_off = si->img[i].cbuf_off;
        as.bindless = si->img[i].bindless;
        as.bl_cbuf = si->img[i].bl_cbuf;
        if (!tex_handle(b, &as, &handle, &why) || !b->tex_pool || (handle & 0xFFFFF) >= b->tex_count) continue;
        memcpy(tic, GP(b->tex_pool + (u64)(handle & 0xFFFFF) * 32), 32);
        if (!!vk_tic_format(tic[0], 0).is_int != !!si->img[i].bindless) key->rt_int |= 1u << (8 + i);
    }
}
/* why a buffer range can't be a descriptor in the imported pools, or NULL */
static const char* range_bad(u64 a, u64 size, u64 align) {
    u64 off;
    int n;
    if (!a || !size) return "not bound";
    n = import_of(a, size, &off);
    if (n < 0) return "outside the imported pools";
    if (align > 1 && (off & (align - 1))) return "offset not aligned";
    return NULL;
}
u32 vk_nvn_stage_check(int mod, const NvnStageBind* b, u64* ok, u64* fail, void (*why)(const char*)) {
    char line[96];
    const ShaderInfo* si;
    u64 ualign = g_vk.props.limits.minUniformBufferOffsetAlignment;
    u32 i, fails = 0, base;
    int nt, slot;
#define BAD(kind, what)                                                                                                \
    do {                                                                                                               \
        fail[kind]++;                                                                                                  \
        fails++;                                                                                                       \
        why(what);                                                                                                     \
    } while (0)
    if (mod < 0 || mod >= g_nmod) return 0;
    si = &g_mod[mod].si;
    for (i = 0; i < 18; i++) { /* constant buffers */
        const char* w;
        if (!(si->cbuf_mask >> i & 1)) continue;
        if (i == 0 || i == 2) { /* made on the host */
            ok[i == 0 ? NVN_B_CB0 : NVN_B_CB2]++;
            continue;
        }
        if (i == 1)
            w = !b->cb1 ? "not known" : range_bad(b->cb1, b->cb1_size, 1);
        else if (!b->ubo[i - 3].addr || !b->ubo[i - 3].size) { /* nothing bound: zeros, as B2recomp binds */
            ok[NVN_B_UBO_ZERO]++;
            continue;
        } else {
            u64 size = b->ubo[i - 3].size;
            if (si->cbuf_max[i] && size > si->cbuf_max[i]) size = si->cbuf_max[i];
            w = range_bad(b->ubo[i - 3].addr, size, ualign);
        }
        if (w) {
            snprintf(line, sizeof line, "constant buffer %s: %s", i == 1 ? "1 (program)" : "3+ (uniform)", w);
            BAD(i == 1 ? NVN_B_CB1 : NVN_B_UBO, line);
        } else
            ok[i == 1 ? NVN_B_CB1 : NVN_B_UBO]++;
    }
    base = b->nvn_stage == 5 ? 0x310 : 0x110 + 0x100 * g_hw_stage[b->nvn_stage % 6];
    for (i = 0; i < (u32)si->ngbuf && i < 16; i++) { /* storage buffers, by their address's place in buffer 0 */
        u32 off = si->gbuf_cbuf_off[i], k = (off - base) / 16;
        const char* w;
        slot_seen(2, b->nvn_stage, 0, off / 4);
        if (off < base || (off - base) % 16 || k >= 16) {
            BAD(NVN_B_SSBO, "storage buffer address outside the stage's table in buffer 0");
            continue;
        }
        w = range_bad(b->ssbo[k].addr, b->ssbo[k].size, 1); /* buffer 0 can hold the aligned address */
        if (w) {
            snprintf(line, sizeof line, "storage buffer: %s", w);
            BAD(NVN_B_SSBO, line);
        } else
            ok[NVN_B_SSBO]++;
    }
    nt = si->ntex > SH_MAXTEX ? SH_MAXTEX : si->ntex;
    for (i = 0; i < (u32)nt; i++) {
        const ShTex* st = &si->tex[i];
        ShTex plain;
        const char* w = NULL;
        u32 handle;
        int num;
        slot_seen(0, b->nvn_stage, st->bindless ? st->bl_cbuf : 2, st->cbuf_off);
        if (st->flags & SNX_TEX_INDEXED) {
            BAD(NVN_B_TEX, "texture: indexed table");
            continue;
        }
        if (st->flags & 2) {
            BAD(NVN_B_TEX, "texture: multisample");
            continue;
        }
        if (st->shadow && (si->tex_dc_color_mask >> i & 1)) { /* compared in the shader */
            plain = *st;
            plain.shadow = 0;
            st = &plain;
        }
        num = st->flags & 1 ? 1 : st->shadow || !(si->tex_int_mask >> i & 1) ? 0 : (si->tex_sint_mask >> i & 1) ? 2 : 1;
        if (!tex_handle(b, st, &handle, &w) || !tex_get(b, handle, st, num, 0, &slot, &w)) {
            snprintf(line, sizeof line, "texture: %s", w);
            BAD(NVN_B_TEX, line);
        } else {
            ok[NVN_B_TEX]++;
            g_null_views += (u64)g_tc[slot].null_view;
        }
    }
    for (i = 0; i < (u32)si->nimg; i++) { /* storage images: handles from constant buffer 2 word 72 + 2 * unit */
        ShTex as;
        const char* w = NULL;
        u32 handle;
        memset(&as, 0, sizeof as);
        as.cbuf_off = si->img[i].cbuf_off;
        as.bindless = si->img[i].bindless;
        as.bl_cbuf = si->img[i].bl_cbuf;
        slot_seen(1, b->nvn_stage, as.bindless ? as.bl_cbuf : 2, as.cbuf_off);
        if (!tex_handle(b, &as, &handle, &w) || !tex_get(b, handle, NULL, 0, 1, &slot, &w)) {
            snprintf(line, sizeof line, "%s", w);
            BAD(NVN_B_IMG, line);
        } else
            ok[NVN_B_IMG]++;
    }
#undef BAD
    return fails;
}

/* ------------------------------------------------------------ replay of one frame (phase 3 step 3c part 3) */
/* The frame's draws go into a command buffer of the replay's own and render targets of its own: an image per target
   texture (by GPU address, made at first use, cleared, kept in GENERAL), which a texture of the same address sampled
   later in the frame is in place of. Everything else comes from the renderer's objects (pipelines, textures, samplers,
   null textures) and the imported pools (vertex, index, indirect, storage buffers); uniform data, constant buffers 0
   and 2 (made on the host) and storage buffers a program writes are copied into the replay's staging, so nothing the
   game or the renderer reads changes. At the end, after the renderer's commands so far (which upload the textures),
   the replay's run, and the presented target is read back */
u64 vk_nvn_cpu_of(u64 gpu) {
    u64 off;
    int n = import_of(gpu, 1, &off);
    return n < 0 ? 0 : g_import[n].cpu + off;
}
#define RP_TARGETS 256
#define RP_STAGE   (256ull << 20)
#define RP_USED    2048
#define VK_RP_WRITES                                                                                                   \
    (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |                                         \
     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |                                   \
     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
typedef struct {
    u64 gpu;
    u32 w, h, layers; /* the game's size: an image's identity, whatever its own size */
    u32 sw, sh;       /* the image's */
    float sx, sy;     /* sw / w, sh / h */
    u64 composed;     /* window textures: 1 + the frame FSR last wrote it in */
    VkFormat f;       /* the image's: the format first drawn as */
    VkImageAspectFlags aspect;
    int storage; /* made with VK_IMAGE_USAGE_STORAGE_BIT */
    int fb;      /* live frames: 1 + the target holding its copy for a draw that samples it while drawing to it */
    u64 fb_at;   /* g_rp.drawn at that copy */
    u64 used;    /* g_rp.frame when last drawn to or sampled */
    VkImage img;
    VmaAllocation mem;
    struct {
        VkFormat f;
        VkImageViewType type;
        VkImageAspectFlags aspect;
        VkComponentMapping map;
        u32 layer, nlayers;
        VkImageView v;
    } view[12];
    int nview;
} RpTarget;
static struct {
    int on;
    int live; /* SNX_NVN_LIVE: cb is the renderer's, images stay from frame to frame */
    int real; /* SNX_NVN_RELEASE_ONLY: writes go to the imported pools themselves */
    int debug_frame;
    u8* last_p;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool dpool[64]; /* RP_POOL_SETS sets each, one more whenever the last is used up */
    int npool;
    VkBuffer sbuf;
    VmaAllocation smem;
    u8* sp;
    u64 sat;
    RpTarget t[RP_TARGETS];
    int nt;
    int col[8], dep;   /* the targets set: indices in t, -1 for none */
    u32 coll[8], depl; /* and the layer each draws to */
    VkFormat colf[8];
    u32 ncol;
    int rendering, dirty;
    u32 rw, rh;
    float psx, psy; /* the pass's image / game pixels */
    int fsr_win;    /* 1 + the window target the final draw of FSR 1 stands in for */
    int fs_src;     /* the draw's first fragment texture that is a target, -1 for none */
    u64 fsr_frames;
    Surf* used[RP_USED]; /* the renderer's textures sampled: in SHADER_READ_ONLY_OPTIMAL when the replay runs */
    u32 nused;
    u64 draws, drawn, clears, cleared, skipped, dispatches, dispatched;
    u64 frame; /* live frames: presents so far */
    u32 chunk; /* draws and dispatches since the last submission of a window */
    u64 pipe_wait, deferred,
        deferred_seen; /* ns waited for pipelines this frame; draws left out for one in the making */
    u32 fr_deferred;   /* this frame's, and the largest target one of them drew to */
    u64 fr_deferred_area;
    u64 last_present, worst_ns, slow_frames; /* the longest present to present since the last stats, those over 50 ms */
    u64 fr_win, fr_lock;                     /* this frame: ns in windows, waiting for vk_lock to open one */
    u32 fr_draws;
    u64 fr_first, fr_last; /* this frame: the first window's start, the last one's end */
    u64 p_frames, p_over, p_win, p_lock, p_before, p_after, p_busy; /* the same since the last stats, in ns */
    u64 p_at60, p_at30; /* frames the game paced at 60 (swap interval 1, the ones p_over counts) and at 30 or less */
    u64 p_slept, p_restarts; /* the pacer's counters at the last stats */
    int fr_gpu_busy;         /* the GPU still had earlier work when the frame's first window opened */
    u64 win_start, win_ns, win_ns_seen, frame_seen;
    u32 kinds[4]; /* the draws of the pass so far, by NvnDrawCall.kind */
    char why[16][64];
    u64 why_n[16];
} g_rp;
#define RSKIP(why)                                                                                                     \
    do {                                                                                                               \
        vk_nvn_replay_skip(why);                                                                                       \
        return;                                                                                                        \
    } while (0)
/* SNX_NVN_DRAWLOG=path: a measure for temporal upscalers, off otherwise. Two consecutive frames every
   SNX_NVN_DRAWLOG_EVERY (300), each draw a line: what it is, a hash of the bytes its vertex stages read, the samples that
   passed (occlusion query), and for an indirect draw the arguments the GPU ended up with */
#define DL_ROWS  8192u
#define DL_TEXT  1500u
#define DL_SEEN  8192u
#define DL_WROTE 4096u
#define DL_UPD   4096u
static struct {
    int every; /* 0 off, -1 not read yet */
    FILE* f;
    VkQueryPool q;
    int open; /* this frame is measured: its queries were reset */
    int cam;  /* its camera candidate was written */
    u32 n, nwrote, nupd;
    struct {
        u64 indirect, parameter;
        u32 kind, max, stride;
        char text[DL_TEXT];
    }* row;
    struct {
        u64 gpu, size;
    } wrote[DL_WROTE]; /* storage ranges this frame's compute and draws write */
    struct {
        u64 gpu, size, hash;
    } upd[DL_UPD]; /* this frame's UpdateUniformBuffer payloads, which reach guest memory on the GPU */
    struct {
        u64 gpu, size, hash;
    } seen[DL_SEEN];
} g_dl = {.every = -1};
static u64 dl_mix(u64 h, u64 v) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
static u64 dl_bytes(const u8* p, u64 n) {
    u64 h = n, i, v;
    for (i = 0; i + 8 <= n; i += 8) {
        memcpy(&v, p + i, 8);
        h = dl_mix(h, v);
    }
    for (; i < n; i++) h = dl_mix(h, p[i]);
    return h;
}
static u64 dl_range(u64 gpu, u64 size) { /* a range's bytes, once a frame */
    u64 at = (gpu >> 4 ^ size * 31) % DL_SEEN, off;
    u32 k;
    int n;
    for (k = 0; k < 64; k++, at = (at + 1) % DL_SEEN) {
        if (g_dl.seen[at].gpu == gpu && g_dl.seen[at].size == size) return g_dl.seen[at].hash;
        if (!g_dl.seen[at].gpu) break;
    }
    if ((n = import_of(gpu, size, &off)) < 0) return 0;
    {
        u64 h = dl_bytes((const u8*)GP(g_import[n].cpu + off), size);
        if (k < 64) {
            g_dl.seen[at].gpu = gpu;
            g_dl.seen[at].size = size;
            g_dl.seen[at].hash = h;
        }
        return h;
    }
}
static u64 dl_ubo(u64 gpu, u64 size) { /* its bytes and the payloads written to it so far this frame */
    u64 h = dl_range(gpu, size);
    u32 k;
    for (k = 0; k < g_dl.nupd; k++)
        if (g_dl.upd[k].gpu >= gpu && g_dl.upd[k].gpu < gpu + size)
            h = dl_mix(h, g_dl.upd[k].hash + g_dl.upd[k].gpu - gpu);
    return h;
}
static void dl_written(u64 gpu, u64 size) {
    if (g_dl.open && g_dl.nwrote < DL_WROTE) {
        g_dl.wrote[g_dl.nwrote].gpu = gpu;
        g_dl.wrote[g_dl.nwrote++].size = size;
    }
}
static int dl_was_written(u64 gpu, u64 size) {
    u32 k;
    for (k = 0; k < g_dl.nwrote; k++)
        if (gpu < g_dl.wrote[k].gpu + g_dl.wrote[k].size && g_dl.wrote[k].gpu < gpu + size) return 1;
    return 0;
}
static void dl_update(u64 dst, const void* data, u32 size) {
    if (g_dl.open && g_dl.nupd < DL_UPD) {
        g_dl.upd[g_dl.nupd].gpu = dst;
        g_dl.upd[g_dl.nupd].size = size;
        g_dl.upd[g_dl.nupd++].hash = dl_bytes((const u8*)data, size);
    }
}
static void dl_window(void) { /* a window opens, outside a pass */
    if (g_dl.every < 0) {
        const char* e = getenv("SNX_NVN_DRAWLOG");
        const char* every = getenv("SNX_NVN_DRAWLOG_EVERY");
        VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_OCCLUSION,
                                    .queryCount = DL_ROWS};
        g_dl.every = 0;
        if (!e || !e[0]) return;
        g_dl.row = malloc(DL_ROWS * sizeof *g_dl.row);
        g_dl.f = fopen(e, "w");
        if (!g_dl.row || !g_dl.f || !VK_OK(vkCreateQueryPool(g_vk.dev, &ci, NULL, &g_dl.q))) return;
        g_dl.every = every ? atoi(every) : 300;
        fprintf(g_dl.f, "D,frame,seq,kind,prim,tess,gs,vs,fs,tw,th,dw,dh,dtest,dwrite,dfunc,stencil,blend,chan,ncolor,"
                        "count,inst,base_inst,base_vert,max,stride,index,indirect,param,args_written,verts_written,"
                        "streams,buffers,samples\nS,frame,seq,i,count,inst,first,base_vert,base_inst\n");
    }
    if (!g_dl.every || g_dl.open || !g_rp.frame || g_rp.frame % (u64)g_dl.every > 1) return;
    vkCmdResetQueryPool(g_rp.cb, g_dl.q, 0, DL_ROWS);
    g_dl.open = 1;
}
static int dl_row(const NvnPipeDesc* d, const NvnStageBind* bind, const NvnDrawDyn* y, const NvnDrawCall* call) {
    int c0 = g_rp.ncol ? g_rp.col[0] : -1, dp = g_rp.dep, s, vw = 0;
    u32 i, st;
    size_t at;
    char* t;
    if (!g_dl.open || g_dl.n == DL_ROWS) return -1;
    t = g_dl.row[g_dl.n].text;
    g_dl.row[g_dl.n].indirect = call->indirect;
    g_dl.row[g_dl.n].parameter = call->parameter;
    g_dl.row[g_dl.n].kind = (u32)call->kind;
    g_dl.row[g_dl.n].max = call->max_draws;
    g_dl.row[g_dl.n].stride = call->stride ? call->stride : 20;
    for (st = 0; st < 16; st++)
        if (y->vbuf[st].addr && y->stride[st] &&
            dl_was_written(y->vbuf[st].addr, y->vbuf[st].size ? y->vbuf[st].size : 1))
            vw = 1;
    at = (size_t)snprintf(
        t, DL_TEXT,
        "D,%llu,%u,%d,%u,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%x,%x,%u,%u,%u,%u,%d,%u,%u,%llx,%llx,%llx,%d,%d,",
        (unsigned long long)g_rp.frame, g_dl.n, call->kind, call->primitive, d->mod[1] >= 0 || d->mod[2] >= 0,
        d->mod[3] >= 0, d->mod[0], d->mod[4], c0 >= 0 ? g_rp.t[c0].w : 0, c0 >= 0 ? g_rp.t[c0].h : 0,
        dp >= 0 ? g_rp.t[dp].w : 0, dp >= 0 ? g_rp.t[dp].h : 0, y->depth_test, y->depth_write, y->depth_func,
        y->stencil_test, d->blend_on, d->chanmask, d->ncolor, call->count, call->instances, call->base_instance,
        call->base_vertex, call->max_draws, call->stride, (unsigned long long)call->index,
        (unsigned long long)call->indirect, (unsigned long long)call->parameter,
        call->kind >= 2 && dl_was_written(call->indirect, 20), vw);
    for (st = 0; st < 16 && at < DL_TEXT - 128; st++) { /* address/size/stride/divisor/hash */
        u64 size = y->vbuf[st].size > (16ull << 20) ? 16ull << 20 : y->vbuf[st].size;
        if (y->vbuf[st].addr)
            at += (size_t)snprintf(t + at, DL_TEXT - at, "%u/%llx/%llx/%u/%u/%llx|", st,
                                   (unsigned long long)y->vbuf[st].addr, (unsigned long long)y->vbuf[st].size,
                                   y->stride[st], d->stream_divisor[st],
                                   (unsigned long long)dl_range(y->vbuf[st].addr, size));
    }
    t[at++] = ',';
    for (s = 0; s < 3; s += 2) { /* vertex and tessellation evaluation: stage, u or s, slot, address, bytes, hash */
        const ShaderInfo* si;
        u32 base;
        if (d->mod[s] < 0) continue;
        si = &g_mod[d->mod[s]].si;
        for (i = 3; i < 19 && at < DL_TEXT - 128; i++) {
            const NvnRange* r = &bind[s].ubo[i - 3];
            u64 size = r->size;
            if (!(si->cbuf_mask >> i & 1) || !r->addr || !size) continue;
            if (si->cbuf_max[i] && size > si->cbuf_max[i]) size = si->cbuf_max[i];
            at += (size_t)snprintf(t + at, DL_TEXT - at, "%du%u/%llx/%llx/%llx|", s, i - 3, (unsigned long long)r->addr,
                                   (unsigned long long)size, (unsigned long long)dl_ubo(r->addr, size));
            if (s == 0 && i == 3 && size == 0x80 && dp >= 0 && y->depth_write && g_rp.t[dp].w * 9 > g_rp.t[dp].h * 15 &&
                !g_dl.cam) { /* the frame's first scene draw: the 16 floats twice, is this the camera? */
                u64 cpu = vk_nvn_cpu_of(r->addr);
                float m[32];
                u32 j;
                g_dl.cam = 1;
                if (cpu) {
                    memcpy(m, GP(cpu), sizeof m);
                    fprintf(g_dl.f, "C,%llu", (unsigned long long)g_rp.frame);
                    for (j = 0; j < 32; j++) fprintf(g_dl.f, ",%.6g", (double)m[j]);
                    fputc('\n', g_dl.f);
                }
            }
        }
        base = bind[s].nvn_stage == 5 ? 0x310 : 0x110 + 0x100 * g_hw_stage[bind[s].nvn_stage % 6];
        for (i = 0; i < (u32)si->ngbuf && i < 16 && at < DL_TEXT - 128; i++) {
            u32 k = (si->gbuf_cbuf_off[i] - base) / 16;
            const NvnRange* r;
            u64 size;
            if (si->gbuf_cbuf_off[i] < base || k >= 16) continue;
            r = &bind[s].ssbo[k];
            size = r->size > (16ull << 20) ? 16ull << 20 : r->size;
            if (r->addr && size)
                at += (size_t)snprintf(t + at, DL_TEXT - at, "%ds%u/%llx/%llx/%llx|", s, k, (unsigned long long)r->addr,
                                       (unsigned long long)r->size, (unsigned long long)dl_range(r->addr, size));
        }
    }
    t[at] = 0;
    return (int)g_dl.n++;
}
static void dl_flush(void) { /* a present: everything of the frame was submitted */
    u64* r;
    u32 k, j;
    if (!g_dl.open) return;
    vk_wait_submit(vk_last_submit());
    r = calloc(g_dl.n + 1, 16);
    if (r && g_dl.n)
        vkGetQueryPoolResults(g_vk.dev, g_dl.q, 0, g_dl.n, (size_t)g_dl.n * 16, r, 16,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    for (k = 0; k < g_dl.n; k++) {
        const u64 cnt = g_dl.row[k].parameter ? vk_nvn_cpu_of(g_dl.row[k].parameter) : 0;
        const u64 args = g_dl.row[k].indirect ? vk_nvn_cpu_of(g_dl.row[k].indirect) : 0;
        u32 n = g_dl.row[k].kind == 2;
        fprintf(g_dl.f, "%s,%lld\n", g_dl.row[k].text, r && r[k * 2 + 1] ? (long long)r[k * 2] : -1ll);
        if (cnt) {
            memcpy(&n, GP(cnt), 4);
            if (n > g_dl.row[k].max) n = g_dl.row[k].max;
        }
        for (j = 0; args && j < n; j++) {
            u32 a[5];
            memcpy(a, GP(args + (u64)j * g_dl.row[k].stride), sizeof a);
            fprintf(g_dl.f, "S,%llu,%u,%u,%u,%u,%u,%d,%u\n", (unsigned long long)g_rp.frame, k, j, a[0], a[1], a[2],
                    (int)a[3], a[4]);
        }
    }
    fflush(g_dl.f);
    free(r);
    memset(g_dl.seen, 0, sizeof g_dl.seen);
    g_dl.n = g_dl.nwrote = g_dl.nupd = 0;
    g_dl.open = g_dl.cam = 0;
}
/* Storage buffer ranges the replayed frame writes: each copied once into staging at its first write, and every later
   binding inside it, read or written, uses the copy, so the replay sees its own writes (compute filling the light
   lists the forward pass reads) and guest memory never changes. 1 when bi is a copy (its buffer NULL when staging ran
   out), 0 to bind the pool itself */
#define RP_WRITTEN 256
static struct {
    u64 gpu, size, off;
    VkBuffer buf;
    u8* p; /* the copy, mapped */
} g_rpw[RP_WRITTEN];
static u32 g_nrpw, g_rpw_full;
void vk_nvn_replay_skip(const char* why) {
    u32 k;
    g_rp.skipped++;
    for (k = 0; k < 16 && g_rp.why_n[k] && strcmp(g_rp.why[k], why); k++) {}
    if (k < 16) {
        if (!g_rp.why_n[k]) snprintf(g_rp.why[k], sizeof g_rp.why[k], "%s", why);
        g_rp.why_n[k]++;
    }
}
static u8* rp_alloc(u64 size, u64* off) { /* staging, aligned for every descriptor and vertex use */
    u64 align = vk_stage_align(), at = (g_rp.sat + align - 1) & ~(align - 1);
    if (!size) size = 16;
    if (g_rp.live) { /* the frame's staging ring; sbuf follows it when it grows */
        u8* p = vk_stage(size, align, &g_rp.sbuf, off);
        if (p) g_rp.sat += size;
        return g_rp.last_p = p;
    }
    if (at + size > RP_STAGE) return NULL;
    g_rp.sat = at + size;
    *off = at;
    return g_rp.sp + at;
}
static void rp_barrier(void) { /* everything before, in submission order too, before everything after */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(g_rp.cb, &di);
}
/* The dynamic state, pipeline and buffers set so far in this pass: a draw that needs the same sets nothing, a third of
   a frame's recording otherwise. Forgotten when the pass ends: uploads, compute, the renderer and submissions, whatever
   could change them, happen outside one */
typedef struct {
    VkViewport vp;
    VkRect2D sc;
    u32 b[8];
    u32 s[2][7];
    float f[8];
} RpDyn;
static struct {
    int valid;
    RpDyn d;
    VkPipeline pipe;
    float pc[8];
    VkBuffer ib;
    u64 io;
    u32 it;
} g_rpd;
/* SNX_NVN_PASSTIME=n: every n frames, GPU time of each rendering pass and compute dispatch, one line in the log. A frame
   sampled while the last one's results are out is skipped. An interval counts for the mark that ends it; time between
   two windows of a frame isn't counted */
#define PT_MARKS 1024u
static u32 g_ft[256]; /* frames of each length in ms since the last report, for its 1% low */
enum { PT_OPEN, PT_BETWEEN, PT_PASS, PT_COMPUTE, PT_COPIES };
static struct {
    int every; /* 0 off, -1 not read yet */
    int open;  /* marks are being written */
    int out;   /* the frame ended, its results aren't in yet */
    VkQueryPool q;
    u32 n, drawn_at;
    int mod;         /* the shader of the job the next mark ends: a compute module, or a pass's last fragment module */
    u64 bound;       /* the bytes of storage buffers that job bound */
    u64 hit0, miss0; /* the reads counted at the last mark */
    u64 frame, astc;
    struct {
        u32 kind, w, h, draws, hit, miss;
        int mod;
        u64 bound;
    } tag[PT_MARKS];
} g_pt = {.every = -1};
static u64 g_mir_hits, g_mir_miss; /* reads from a video memory copy, and from the pool */
static void pt_mark(u32 kind, u32 w, u32 h, u32 draws) {
    if (!g_pt.open || g_pt.n == PT_MARKS) return;
    g_pt.tag[g_pt.n].kind = kind;
    g_pt.tag[g_pt.n].w = w;
    g_pt.tag[g_pt.n].h = h;
    g_pt.tag[g_pt.n].draws = draws;
    g_pt.tag[g_pt.n].mod = g_pt.mod;
    g_pt.tag[g_pt.n].bound = g_pt.bound;
    g_pt.tag[g_pt.n].hit = g_mir_hits >= g_pt.hit0 ? (u32)(g_mir_hits - g_pt.hit0) : 0; /* the report zeroes them */
    g_pt.tag[g_pt.n].miss = g_mir_miss >= g_pt.miss0 ? (u32)(g_mir_miss - g_pt.miss0) : 0;
    g_pt.hit0 = g_mir_hits;
    g_pt.miss0 = g_mir_miss;
    g_pt.mod = -1;
    g_pt.bound = 0;
    vkCmdWriteTimestamp2(g_rp.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pt.q, g_pt.n++);
}
static void pt_window(void) { /* a window opens, outside a pass */
    if (g_pt.every < 0) {
        const char* e = getenv("SNX_NVN_PASSTIME");
        VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = PT_MARKS};
        g_pt.every = e ? atoi(e) : 0;
        if (g_pt.every > 0 && !VK_OK(vkCreateQueryPool(g_vk.dev, &ci, NULL, &g_pt.q))) g_pt.every = 0;
    }
    if (g_pt.every <= 0) return;
    if (!g_pt.open && !g_pt.out && g_rp.frame % (u64)g_pt.every == 0) {
        vkCmdResetQueryPool(g_rp.cb, g_pt.q, 0, PT_MARKS);
        g_pt.open = 1;
        g_pt.n = 0;
        g_pt.frame = g_rp.frame;
        g_pt.astc = g_vk_astc_px;
    }
    pt_mark(PT_OPEN, 0, 0, 0);
}
static void pt_read(void) {
    static u64 v[PT_MARKS];
    static struct {
        u32 kind, w, h, draws, n;
        double ms;
    } m[PT_MARKS];
    char line[512];
    double per[5] = {0, 0, 0, 0, 0}, total = 0;
    u32 count[5] = {0, 0, 0, 0, 0}, k, j, nm = 0;
    size_t at;
    if (g_pt.open && g_rp.frame != g_pt.frame) { /* the sampled frame was presented */
        g_pt.open = 0;
        g_pt.out = 1;
    }
    if (!g_pt.out) return;
    if (g_pt.n < 2) {
        g_pt.out = 0;
        return;
    }
    if (vkGetQueryPoolResults(g_vk.dev, g_pt.q, 0, g_pt.n, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (k = 1; k < g_pt.n; k++) {
        u32 kind = g_pt.tag[k].kind;
        double ms;
        if (kind == PT_OPEN) continue;
        ms = (double)(v[k] - v[k - 1]) * (double)g_vk.props.limits.timestampPeriod / 1e6;
        per[kind] += ms;
        count[kind]++;
        total += ms;
        for (j = 0; j < nm; j++)
            if (m[j].kind == kind && m[j].w == g_pt.tag[k].w && m[j].h == g_pt.tag[k].h) break;
        if (j == nm) {
            memset(&m[nm], 0, sizeof m[nm]);
            m[nm].kind = kind;
            m[nm].w = g_pt.tag[k].w;
            m[nm++].h = g_pt.tag[k].h;
        }
        m[j].ms += ms;
        m[j].n++;
        m[j].draws += g_pt.tag[k].draws;
    }
    at = (size_t)snprintf(
        line, sizeof line,
        "nvn-pass: frame %llu, GPU %.1f ms: passes %.1f (%u), compute %.1f (%u), between %.1f, copies %.1f; "
        "ASTC %.1f Mpx; largest:",
        (unsigned long long)g_pt.frame, total, per[PT_PASS], count[PT_PASS], per[PT_COMPUTE], count[PT_COMPUTE],
        per[PT_BETWEEN], per[PT_COPIES], (double)(g_vk_astc_px - g_pt.astc) / 1e6);
    for (k = 0; k < 7; k++) {
        u32 best = nm;
        for (j = 0; j < nm; j++)
            if (m[j].n && (best == nm || m[j].ms > m[best].ms)) best = j;
        if (best == nm || m[best].ms < 0.2 || at >= sizeof line - 64) break;
        if (m[best].kind == PT_PASS)
            at += (size_t)snprintf(line + at, sizeof line - at, " %ux%u x%u %u draws %.1f;", m[best].w, m[best].h,
                                   m[best].n, m[best].draws, m[best].ms);
        else if (m[best].kind == PT_COMPUTE)
            at += (size_t)snprintf(line + at, sizeof line - at, " compute %ux%u x%u %.1f;", m[best].w, m[best].h,
                                   m[best].n, m[best].ms);
        else
            at += (size_t)snprintf(line + at, sizeof line - at, " %s x%u %.1f;",
                                   m[best].kind == PT_COPIES ? "copies" : "between", m[best].n, m[best].ms);
        m[best].n = 0;
    }
    LOG("%s\n", line);
    { /* the largest single jobs */
        char top[768];
        double done = 1e18;
        size_t put = (size_t)snprintf(top, sizeof top, "nvn-top: frame %llu:", (unsigned long long)g_pt.frame);
        for (j = 0; j < 6 && put < sizeof top - 128; j++) {
            double most = 0;
            u32 at_most = 0;
            for (k = 1; k < g_pt.n; k++) {
                u32 kind = g_pt.tag[k].kind;
                double ms = (double)(v[k] - v[k - 1]) * (double)g_vk.props.limits.timestampPeriod / 1e6;
                if ((kind == PT_PASS || kind == PT_COMPUTE) && ms > most && ms < done) most = ms, at_most = k;
            }
            if (!at_most || most < 0.5) break;
            done = most;
            {
                int mod = g_pt.tag[at_most].mod;
                const ShaderInfo* si = mod >= 0 && mod < g_nmod ? &g_mod[mod].si : NULL;
                put += (size_t)snprintf(top + put, sizeof top - put, " %s %ux%u%s%u, shader %08x",
                                        g_pt.tag[at_most].kind == PT_PASS ? "pass" : "compute", g_pt.tag[at_most].w,
                                        g_pt.tag[at_most].h, g_pt.tag[at_most].kind == PT_PASS ? " draws " : " local ",
                                        g_pt.tag[at_most].kind == PT_PASS ? g_pt.tag[at_most].draws
                                        : si                              ? si->cs_local[0]
                                                                          : 0,
                                        si ? g_mod[mod].spv_hash : 0);
                put += (size_t)snprintf(
                    top + put, sizeof top - put, ", images %d, buffers %d %.1f MiB, reads %u copies %u pools, %.1f ms;",
                    si ? si->nimg : 0, si ? si->ngbuf : 0, (double)g_pt.tag[at_most].bound / 1048576.0,
                    g_pt.tag[at_most].hit, g_pt.tag[at_most].miss, most);
            }
        }
        LOG("%s\n", top);
    }
    g_pt.out = 0;
}
static void rp_end_rendering(void) {
    g_rpd.valid = 0;
    g_rpd.pipe = VK_NULL_HANDLE;
    g_rpd.ib = VK_NULL_HANDLE;
    if (!g_rp.rendering) return;
    vkCmdEndRendering(g_rp.cb);
    pt_mark(PT_PASS, g_rp.rw, g_rp.rh, (u32)g_rp.drawn - g_pt.drawn_at);
    g_rp.rendering = 0;
}
static void rp_live_miss(void) {
    if (g_rp.live) rp_end_rendering();
}
static VkImageAspectFlags fmt_aspect(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
#define RP_POOL_SETS 256
static int rp_pool_add(void) { /* the sets' layouts hold the indexed texture arrays too: pools of a few hundred */
    VkDescriptorPoolSize sz[VK_POOL_TYPES];
    VkDescriptorPoolCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = RP_POOL_SETS,
                                      .poolSizeCount = VK_POOL_TYPES,
                                      .pPoolSizes = sz};
    if (g_rp.npool == 64) return 0;
    vk_pool_sizes(sz, RP_POOL_SETS);
    if (vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &g_rp.dpool[g_rp.npool]) != VK_SUCCESS) return 0;
    g_rp.npool++;
    return 1;
}
static int rp_set_alloc(VkDescriptorSetLayout layout, VkDescriptorSet* set) {
    if (g_rp.live) return (*set = vk_desc_alloc(layout)) != VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &layout};
    ai.descriptorPool = g_rp.dpool[g_rp.npool - 1];
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, set) == VK_SUCCESS) return 1;
    if (!rp_pool_add()) return 0;
    ai.descriptorPool = g_rp.dpool[g_rp.npool - 1];
    return vkAllocateDescriptorSets(g_vk.dev, &ai, set) == VK_SUCCESS;
}
int vk_nvn_replay_begin(void) {
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = RP_STAGE,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo ai;
    int i;
    if (g_rp.on) return 1;
    memset(&g_rp, 0, sizeof g_rp);
    g_nrpw = g_rpw_full = 0;
    g_rp.dep = -1;
    g_rp.psx = g_rp.psy = 1.0f;
    for (i = 0; i < 8; i++) g_rp.col[i] = -1;
    if (!rp_pool_add() || vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_rp.sbuf, &g_rp.smem, &ai) != VK_SUCCESS) {
        LOG("nvn-replay: no descriptor pool or staging\n");
        return 0;
    }
    vk_mem_tag(g_rp.smem, VKM_STAGE);
    g_rp.sp = ai.pMappedData;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_rp.pool) != VK_SUCCESS ||
        vkCreateFence(g_vk.dev, &fci, NULL, &g_rp.fence) != VK_SUCCESS ||
        (cai.commandPool = g_rp.pool, vkAllocateCommandBuffers(g_vk.dev, &cai, &g_rp.cb) != VK_SUCCESS) ||
        vkBeginCommandBuffer(g_rp.cb, &cbi) != VK_SUCCESS) {
        LOG("nvn-replay: no command buffer\n");
        return 0;
    }
    rp_barrier(); /* after the renderer's work submitted before it: its texture uploads */
    g_rp.on = 1;
    LOG("nvn-replay: replay begins (%llu MiB staging)\n", (unsigned long long)(RP_STAGE >> 20));
    return 1;
}
static int rp_target_vk(const NvnTarget* t, VkFormat f, int scale);
static void rp_retire(int i);
static int rp_bound(int i);
/* What the resolution scale takes: the game's frame and the half sizes of its chains, every step of its dynamic
   resolution (960x540 to 1600x900) among them, by their shape. Shadow maps, atlases and small bloom levels stay. The
   window's textures take resolution itself, the rest resolution / fsr */
static float rp_scale_of(u32 w, u32 h, u32 layers, float* xs) {
    *xs = 1.0f;
    if (layers > 1) return 1.0f;
    if (w == g_rp_win_w && h == g_rp_win_h) return *xs = g_vk_scale;
    if (w >= 320 && w * 100 >= h * 170 && w * 100 <= h * 180) {
        *xs = rp_scene_scale() * rp_xstretch(); /* wider on x only (aspect_ratio) */
        return rp_scene_scale();
    }
    return 1.0f;
}
static int rp_target(const NvnTarget* t) { return rp_target_vk(t, target_fmt(t->format), 1); }
/* the replay's image for a target texture in format f, made and cleared at first use; storage-capable when f can be.
   An image is its address and size: pools reuse addresses, so 6398ef000 is an 800x448 target and a 720x408 storage
   image at once, and each keeps its image until its texture is finalized. scale 0 keeps the game's size: storage
   images, whose programs say their pixels themselves. scale 2 takes the size in g_rp_want */
static u32 g_rp_want[2];
static int rp_target_vk(const NvnTarget* t, VkFormat f, int scale) {
    RpTarget* r;
    int i;
    for (i = 0; i < g_rp.nt; i++)
        if (g_rp.t[i].gpu == t->gpu && g_rp.t[i].w == t->w && g_rp.t[i].h == t->h) {
            g_rp.t[i].used = g_rp.frame;
            return i;
        }
    for (i = 0; i < g_rp.nt && g_rp.t[i].img; i++) {}
    if (i == RP_TARGETS) { /* the image unused longest goes: not one of this frame, nor a feedback copy */
        int oldest = -1;
        for (i = 0; i < RP_TARGETS; i++)
            if (!rp_bound(i) && !(g_rp.t[i].gpu >> 63) && g_rp.t[i].used != g_rp.frame &&
                (oldest < 0 || g_rp.t[i].used < g_rp.t[oldest].used))
                oldest = i;
        if (oldest < 0) return -1;
        LOG_LIMIT(8, "nvn-live: image %llx %ux%u, unused for %llu frames, goes for %llx %ux%u\n",
                  (unsigned long long)g_rp.t[oldest].gpu, g_rp.t[oldest].w, g_rp.t[oldest].h,
                  (unsigned long long)(g_rp.frame - g_rp.t[oldest].used), (unsigned long long)t->gpu, t->w, t->h);
        rp_retire(oldest);
        i = oldest;
    }
    if (!f || !t->w || !t->h) return -1;
    r = &g_rp.t[i];
    memset(r, 0, sizeof *r);
    r->gpu = t->gpu;
    r->w = r->sw = t->w;
    r->h = r->sh = t->h;
    r->used = g_rp.frame;
    r->layers = t->layers ? (t->layers > 256 ? 256 : t->layers) : 1;
    if (scale == 2) {
        r->sw = g_rp_want[0];
        r->sh = g_rp_want[1];
    } else if (scale) {
        float xs, s = rp_scale_of(t->w, t->h, r->layers, &xs);
        r->sw = (u32)((float)t->w * xs + 0.5f);
        r->sh = (u32)((float)t->h * s + 0.5f);
    }
    r->sx = (float)r->sw / (float)r->w;
    r->sy = (float)r->sh / (float)r->h;
    r->f = f;
    r->aspect = fmt_aspect(f);
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = r->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = f,
        .extent = {r->sw, r->sh, 1},
        .mipLevels = 1,
        .arrayLayers = r->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (r->aspect != VK_IMAGE_ASPECT_COLOR_BIT                  ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                  : fmt_has(f, 0, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                                          : 0) |
                 (r->aspect == VK_IMAGE_ASPECT_COLOR_BIT && fmt_has(f, 0, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
                      ? VK_IMAGE_USAGE_STORAGE_BIT
                      : 0),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ii, &aci, &r->img, &r->mem, NULL) != VK_SUCCESS) return -1;
    vk_mem_tag(r->mem, VKM_TARGET);
    r->storage = (ii.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    rp_end_rendering();
    vk_barrier(g_rp.cb, r->img, r->aspect, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageSubresourceRange range = {r->aspect, 0, 1, 0, r->layers};
    if (r->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        vkCmdClearColorImage(g_rp.cb, r->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    } else {
        VkClearDepthStencilValue depth1 = {1.0f, 0};
        vkCmdClearDepthStencilImage(g_rp.cb, r->img, VK_IMAGE_LAYOUT_GENERAL, &depth1, 1, &range);
    }
    rp_barrier();
    if (i == g_rp.nt) g_rp.nt++;
    return i;
}
static void rp_retire(int i) { /* its views and image once the GPU is done with them */
    RpTarget* r = &g_rp.t[i];
    int k = 0;
    u32 c;
    if (r->fb) rp_retire(r->fb - 1);
    for (; k + 2 < r->nview; k += 2) vk_defer_image(VK_NULL_HANDLE, NULL, r->view[k].v, r->view[k + 1].v);
    vk_defer_image(r->img, r->mem, k < r->nview ? r->view[k].v : VK_NULL_HANDLE,
                   k + 1 < r->nview ? r->view[k + 1].v : VK_NULL_HANDLE);
    memset(r, 0, sizeof *r);
    for (c = 0; c < 8; c++)
        if (g_rp.col[c] == i) g_rp.col[c] = -1, g_rp.dirty = 1;
    if (g_rp.dep == i) g_rp.dep = -1, g_rp.dirty = 1;
}
/* a view of target i: one layer (an attachment, or a 2D sampler) or all of them (an array sampler, layer ~0u) */
static VkImageView rp_view(int i, VkFormat f, VkImageViewType type, VkImageAspectFlags aspect,
                           const VkComponentMapping* map, u32 layer) {
    u32 nlayers;
    static const VkComponentMapping identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    RpTarget* r = &g_rp.t[i];
    int k;
    if (!map) map = &identity;
    if (r->aspect != VK_IMAGE_ASPECT_COLOR_BIT) f = r->f; /* depth formats view only as themselves */
    if (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) type = VK_IMAGE_VIEW_TYPE_2D;
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && layer == ~0u)
        layer = 0, nlayers = r->layers;
    else {
        if (layer == ~0u || layer >= r->layers) layer = 0;
        nlayers = 1;
    }
    for (k = 0; k < r->nview; k++)
        if (r->view[k].f == f && r->view[k].type == type && r->view[k].aspect == aspect && r->view[k].layer == layer &&
            r->view[k].nlayers == nlayers && !memcmp(&r->view[k].map, map, sizeof *map))
            return r->view[k].v;
    if (r->nview == 12) return r->view[11].v;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = r->img,
                                .viewType = type,
                                .format = f,
                                .components = *map,
                                .subresourceRange = {aspect, 0, 1, layer, nlayers}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &r->view[r->nview].v) != VK_SUCCESS) return VK_NULL_HANDLE;
    r->view[r->nview].layer = layer;
    r->view[r->nview].nlayers = nlayers;
    r->view[r->nview].f = f;
    r->view[r->nview].type = type;
    r->view[r->nview].aspect = aspect;
    r->view[r->nview].map = *map;
    return r->view[r->nview++].v;
}
/* the target whose scale the pass set now has: attachments of a pass are one size, so one scale */
static const RpTarget* rp_pass_target(void) {
    u32 i;
    for (i = 0; i < g_rp.ncol && i < 8; i++)
        if (g_rp.col[i] >= 0) return &g_rp.t[g_rp.col[i]];
    return g_rp.dep >= 0 ? &g_rp.t[g_rp.dep] : NULL;
}
static void rp_fsr_restore(void) { /* a final draw given up on: the window is the target again */
    if (!g_rp.fsr_win) return;
    g_rp.col[0] = g_rp.fsr_win - 1;
    g_rp.fsr_win = 0;
    g_rp.dirty = 1;
}
void vk_nvn_replay_targets(const NvnTarget* color, u32 n, const NvnTarget* depth) {
    int col[8], dep = -1;
    VkFormat colf[8];
    u32 coll[8], depl = depth ? depth->layer : 0, i;
    if (!g_rp.on) return;
    rp_fsr_restore();
    if (n > 8) n = 8;
    for (i = 0; i < 8; i++) {
        col[i] = i < n && color[i].gpu ? rp_target(&color[i]) : -1;
        colf[i] = col[i] >= 0 ? target_fmt(color[i].format) : VK_FORMAT_UNDEFINED;
        coll[i] = col[i] >= 0 ? color[i].layer : 0;
    }
    if (depth && depth->gpu) dep = rp_target(depth);
    if (n != g_rp.ncol || dep != g_rp.dep || depl != g_rp.depl || memcmp(col, g_rp.col, sizeof col) ||
        memcmp(colf, g_rp.colf, sizeof colf) || memcmp(coll, g_rp.coll, sizeof coll))
        g_rp.dirty = 1;
    memcpy(g_rp.coll, coll, sizeof coll);
    g_rp.depl = depl;
    g_rp.ncol = n;
    g_rp.dep = dep;
    memcpy(g_rp.col, col, sizeof col);
    memcpy(g_rp.colf, colf, sizeof colf);
}
static void rp_begin(void) {
    VkRenderingAttachmentInfo ca[8], da;
    u32 i, w = ~0u, h = ~0u;
    VkImageAspectFlags za = g_rp.dep >= 0 ? g_rp.t[g_rp.dep].aspect : 0;
    const RpTarget* scaled_by = rp_pass_target();
    if (g_rp.rendering && !g_rp.dirty) return;
    rp_end_rendering();
    rp_barrier(); /* what the frame drew so far, before anything samples it */
    for (i = 0; i < g_rp.ncol; i++) {
        memset(&ca[i], 0, sizeof ca[i]);
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (g_rp.col[i] < 0) continue;
        ca[i].imageView =
            rp_view(g_rp.col[i], g_rp.colf[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, NULL, g_rp.coll[i]);
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (g_rp.t[g_rp.col[i]].sw < w) w = g_rp.t[g_rp.col[i]].sw;
        if (g_rp.t[g_rp.col[i]].sh < h) h = g_rp.t[g_rp.col[i]].sh;
    }
    memset(&da, 0, sizeof da);
    da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (g_rp.dep >= 0) {
        da.imageView = rp_view(g_rp.dep, g_rp.t[g_rp.dep].f, VK_IMAGE_VIEW_TYPE_2D, za, NULL, g_rp.depl);
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (g_rp.t[g_rp.dep].sw < w) w = g_rp.t[g_rp.dep].sw;
        if (g_rp.t[g_rp.dep].sh < h) h = g_rp.t[g_rp.dep].sh;
    }
    /* no attachments: fragment shaders that only store (the menus' per-tile flags, read by the lighting after them).
       The area is the device's limit, so the viewport and scissor alone say which fragments run */
    if (w == ~0u) w = g_vk.props.limits.maxFramebufferWidth, h = g_vk.props.limits.maxFramebufferHeight;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {w, h}},
                          .layerCount = 1,
                          .colorAttachmentCount = g_rp.ncol,
                          .pColorAttachments = ca,
                          .pDepthAttachment = (za & VK_IMAGE_ASPECT_DEPTH_BIT) ? &da : NULL,
                          .pStencilAttachment = (za & VK_IMAGE_ASPECT_STENCIL_BIT) ? &da : NULL};
    if (!g_rp.live || g_rp.debug_frame) { /* the passes: what each draws into, and how many draws the one before had */
        static u64 last_drawn;
        LOG("nvn-replay: pass at draw %llu (previous pass %llu draws: arrays %u, elements %u, indirect %u, multi "
            "indirect %u): colour %d %d %d %d (of %u, layers %u %u %u %u) depth %d, %ux%u\n",
            (unsigned long long)g_rp.draws, (unsigned long long)(g_rp.drawn - last_drawn), g_rp.kinds[0], g_rp.kinds[1],
            g_rp.kinds[2], g_rp.kinds[3], g_rp.col[0], g_rp.col[1], g_rp.col[2], g_rp.col[3], g_rp.ncol, g_rp.coll[0],
            g_rp.coll[1], g_rp.coll[2], g_rp.coll[3], g_rp.dep, w, h);
        memset(g_rp.kinds, 0, sizeof g_rp.kinds);
        last_drawn = g_rp.drawn;
    }
    pt_mark(PT_BETWEEN, 0, 0, 0);
    g_pt.drawn_at = (u32)g_rp.drawn;
    vkCmdBeginRendering(g_rp.cb, &ri);
    g_rp.rendering = 1;
    g_rp.dirty = 0;
    g_rp.rw = w;
    g_rp.rh = h;
    g_rp.psx = scaled_by ? scaled_by->sx : 1.0f;
    g_rp.psy = scaled_by ? scaled_by->sy : 1.0f;
}
static int rp_copy(VkDescriptorBufferInfo* bi, u64 gpu, u64 have, u64 size) { /* size bytes, from gpu's first have */
    u64 off, src;
    u8* p = rp_alloc(size, &off);
    int n = have ? import_of(gpu, have, &src) : -1;
    if (!p) return 0;
    memset(p, 0, size);
    g_land_why = 2;
    if (n >= 0) own_land_range(n, src, have < size ? have : size); /* read on the CPU: the GPU's bytes first */
    if (n >= 0) memcpy(p, GP(g_import[n].cpu + src), have < size ? have : size);
    *bi = (VkDescriptorBufferInfo){g_rp.sbuf, off, size};
    return 1;
}
/* Video memory copies of the pools the game asks the GPU to cache. The imported pools are system memory: every vertex,
   index and constant the GPU read crossed PCIe, 17-22 ms of GPU time a frame at 1% memory load, in the street. The pool
   stays the truth, and every write still goes to it. A block is read from its copy only while it is known equal: the
   write watch and the host's own writes (rp_stamp) take blocks back, and a block written again and again stays in the
   pool. A bad guess costs the old speed, never a wrong byte */
#define MIR_SHIFT  16
#define MIR_QUEUE  16384u
#define MIR_BUDGET (64ull << 20) /* bytes copied a window */
#define MIR_HOT    4             /* windows the GPU wrote it in that keep a block in the pool */
#define MIR_CHUNK  (4ull << 20)  /* the write watch's unit of asking */
#define MIR_ARENA  (16ull << 20) /* memory for a sparse copy's blocks is taken in these */
/* MIR_GPU: a block storage buffers the GPU writes are bound to (mir_own). Its copy is the truth and the pool is behind
   until it lands (own_merge), as Witch Time keeps compute-written buffers (runtime/nvn_vk_draw.c, "CB",
   GPL-3.0-or-later): read from the copy like a valid block, back in guest memory word by word against the snapshot
   taken when it was filled, so a word the game wrote meanwhile stays the game's */
enum { MIR_NONE, MIR_QUEUED, MIR_VALID, MIR_GPU };
static struct {
    int want;
    VkBuffer buf;
    VmaAllocation mem;
    u8** snap;   /* per block of MIR_GPU: guest memory as it was filled */
    u32* ofr;    /* per block: the frame the GPU last wrote it in */
    u8* landing; /* per block: an idle landing's copy is on its way (own_idle) */
    u8* st;      /* per block */
    u8* hot;     /* windows it was written in, halved every so often */
    u8* at;      /* the last of them */
    u8* read;    /* read since it was last written */
    u8* chunk;   /* per MIR_CHUNK: some block of it was copied */
    u8* bound;   /* per block of a sparse copy: it has memory */
    u64 size;    /* a sparse copy's size to the driver; 0: the copy is one whole allocation */
    u32 wseen;
} g_mir[IMPORTS];
static VmaAllocation g_mar; /* the arena blocks are bound from, and what is left of it */
static VmaAllocationInfo g_mar_info;
static u64 g_mar_left, g_mir_bind_ns, g_mir_bound;
static VkMemoryRequirements g_mar_req;
static u8 g_mir_window;
static u64 g_mir_at; /* the offset in its pool of the range the watch is walking */
static struct {
    u32 pool, blk;
} g_mq[MIR_QUEUE];
static u32 g_nmq;
static u64 g_mir_bytes, g_mir_taken, g_mir_scan_ns;
static struct {
    u32 pool, blk;
} g_lq[MIR_QUEUE];         /* GPU blocks to land now (own_land_now) */
static u32 g_nlq, g_own_n; /* g_own_n: blocks MIR_GPU */
static u64 g_own_made, g_own_fill, g_own_fail, g_own_lost, g_land_forced, g_land_idle, g_land_bytes, g_land_ns;
static int own_on(void) { /* SNX_NVN_CB=0: written storage buffers in the pools, as before c7 */
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SNX_NVN_CB");
        on = !(e && e[0] == '0');
        LOG("nvn-live: storage buffers the GPU writes %s\n",
            on ? "in video memory, landing in guest memory when needed" : "in the pools (SNX_NVN_CB=0)");
    }
    return on;
}
static void own_lq(u32 p, u32 b) {
    u32 k;
    for (k = 0; k < g_nlq; k++)
        if (g_lq[k].pool == p && g_lq[k].blk == b) return;
    if (g_nlq == MIR_QUEUE) return; /* more than 1 GiB owned: the rest lands at the next window */
    g_lq[g_nlq].pool = p;
    g_lq[g_nlq++].blk = b;
}
static struct {
    u32 pool, blk;
} g_opq[MIR_QUEUE]; /* GPU blocks the game wrote: its bytes to put into the copy (own_patch) */
static u32 g_nopq;
static u64 g_patch_blocks, g_patch_bytes;
static void own_pq(u32 p, u32 b) {
    if (g_nopq < MIR_QUEUE) {
        g_opq[g_nopq].pool = p;
        g_opq[g_nopq++].blk = b;
    } else
        own_lq(p, b);
}
static void mir_want(int which, int want) { g_mir[which].want = want; }
void vk_nvn_pool_mirror(int which, u64 flags) {
    const char* e = getenv("SNX_NVN_VRAM");
    /* an integrated GPU reads the pools from the same RAM a copy would take, so copies only cost memory, and their
       sparse binds are what such drivers test least (issue 2, device lost on Iris Xe). SNX_NVN_VRAM=1 copies anyway */
    int on = e && e[0] ? e[0] != '0' : g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    /* GPU_CACHED, not shader code (0x40) or compressible render targets (0x80) */
    int want = (flags & 0x20) && !(flags & 0xC0) && on;
    static int said;
    if (!on && !said) {
        said = 1;
        LOG("nvn-live: no video memory copies of pools (%s)\n", e && e[0] ? "SNX_NVN_VRAM=0" : "integrated GPU");
    }
    if (which < 0 || which >= IMPORTS) return;
    vk_lock();
    import_publish();
    pm_lock(&g_imp_lk);
    g_import[which].want_mirror = want;
    if (g_import[which].state == IMP_READY) mir_want(which, want);
    pm_unlock(&g_imp_lk);
    vk_unlock();
}
static void mir_queue(int pool, u64 b) {
    if (g_nmq == MIR_QUEUE) return;
    g_mir[pool].st[b] = MIR_QUEUED;
    g_mq[g_nmq].pool = (u32)pool;
    g_mq[g_nmq++].blk = (u32)b;
}
/* the host wrote [off, off + n) of a pool: blocks the GPU writes stay there */
static void mir_take(int pool, u64 off, u64 n) {
    u64 b, last = (off + n - 1) >> MIR_SHIFT;
    if (!g_mir[pool].st || !n) return;
    for (b = off >> MIR_SHIFT; b <= last; b++) {
        if (g_mir[pool].st[b] ==
            MIR_GPU) { /* the callers land first (own_land_gpu): a write to the pool would be lost */
            g_own_lost++;
            LOG_LIMIT(8, "nvn-live: a GPU-owned block of pool %d written in the pool (block %llu)\n", pool,
                      (unsigned long long)b);
            continue;
        }
        if (g_mir[pool].st[b] != MIR_NONE) g_mir_taken++;
        g_mir[pool].st[b] = MIR_NONE;
        if (g_mir[pool].at[b] != g_mir_window) { /* a block counts once a window */
            g_mir[pool].at[b] = g_mir_window;
            if (g_mir[pool].hot[b] < 16) g_mir[pool].hot[b]++;
        }
    }
}
/* the CPU wrote it, before the submit whose window opens: a block read since the last write is copied at once, so a
   list reading what the CPU just wrote (constants, a frame's vertices) finds it in video memory */
static void mir_cpu_wrote(int pool, u64 off, u64 n) {
    u64 b, last = (off + n - 1) >> MIR_SHIFT;
    for (b = off >> MIR_SHIFT; b <= last; b++) {
        if (g_mir[pool].st[b] == MIR_QUEUED) continue;
        if (g_mir[pool].st[b] == MIR_GPU) { /* lands at once, merged, so the GPU reads the game's new words */
            u64 bo = b << MIR_SHIFT,
                bs = bo + (1ull << MIR_SHIFT) > g_import[pool].size ? g_import[pool].size - bo : 1ull << MIR_SHIFT;
            /* a write before the snapshot (a landing's own merge among them) left it equal: nothing to do. Else the
               game's changed bytes go into the copy (own_patch), the GPU's stay */
            if (memcmp(GP(g_import[pool].cpu + bo), g_mir[pool].snap[b], bs)) own_pq((u32)pool, (u32)b);
            continue;
        }
        if (g_mir[pool].st[b] == MIR_VALID) g_mir_taken++;
        g_mir[pool].st[b] = MIR_NONE;
        if (g_mir[pool].read[b] && g_mir[pool].hot[b] < MIR_HOT) mir_queue(pool, b);
        g_mir[pool].read[b] = 0;
    }
}
static int mir_make(int i) {
    u64 blocks = (g_import[i].size + (1u << MIR_SHIFT) - 1) >> MIR_SHIFT;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = g_import[i].size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    const char* e = getenv("SNX_NVN_VRAM_SPARSE");
    g_mir[i].st = calloc(blocks, 1);
    g_mir[i].hot = calloc(blocks, 1);
    g_mir[i].at = calloc(blocks, 1);
    g_mir[i].read = calloc(blocks, 1);
    g_mir[i].chunk = calloc(g_import[i].size / MIR_CHUNK + 1, 1);
    g_mir[i].bound = calloc(blocks, 1);
    g_mir[i].snap = calloc(blocks, sizeof *g_mir[i].snap);
    g_mir[i].ofr = calloc(blocks, sizeof *g_mir[i].ofr);
    g_mir[i].landing = calloc(blocks, 1);
    if (g_vk.has_sparse &&
        !(e && e[0] == '0')) { /* its offsets are the pool's, memory goes only where blocks are copied */
        VkBufferCreateInfo sci = bci;
        VkBuffer b = VK_NULL_HANDLE;
        VkMemoryRequirements rq;
        sci.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        if (vkCreateBuffer(g_vk.dev, &sci, NULL, &b) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(g_vk.dev, b, &rq);
            if (rq.alignment <= 1u << MIR_SHIFT) {
                g_mir[i].buf = b;
                g_mir[i].size = rq.size;
                g_mar_req = rq;
            } else
                vkDestroyBuffer(g_vk.dev, b, NULL);
        }
    }
    if (!g_mir[i].st || !g_mir[i].hot || !g_mir[i].at || !g_mir[i].read || !g_mir[i].chunk || !g_mir[i].bound ||
        !g_mir[i].snap || !g_mir[i].ofr || !g_mir[i].landing ||
        (!g_mir[i].buf && vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_mir[i].buf, &g_mir[i].mem, NULL) != VK_SUCCESS)) {
        LOG("nvn-live: no video memory copy of a pool of %llu MiB\n", (unsigned long long)(g_import[i].size >> 20));
        free(g_mir[i].st);
        free(g_mir[i].hot);
        free(g_mir[i].at);
        free(g_mir[i].read);
        free(g_mir[i].chunk);
        free(g_mir[i].bound);
        free(g_mir[i].snap);
        free(g_mir[i].ofr);
        free(g_mir[i].landing);
        memset(&g_mir[i], 0, sizeof g_mir[i]);
        return 0;
    }
    if (g_mir[i].mem) vk_mem_tag(g_mir[i].mem, VKM_COPY);
    LOG("nvn-live: video memory copy of a pool of %llu MiB%s\n", (unsigned long long)(g_import[i].size >> 20),
        g_mir[i].size ? ", sparse" : "");
    return 1;
}
/* memory for one block of a sparse copy, from the arena: 0 when video memory is out */
static int mir_page(VkDeviceMemory* mem, u64* off) {
    u64 page = 1ull << MIR_SHIFT;
    if (g_mar_left < page) {
        VkMemoryRequirements rq = g_mar_req;
        VmaAllocationCreateInfo aci = {.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        rq.size = MIR_ARENA;
        rq.alignment = page;
        if (vmaAllocateMemory(g_vk.vma, &rq, &aci, &g_mar, &g_mar_info) != VK_SUCCESS) return 0;
        vk_mem_tag(g_mar, VKM_COPY);
        g_mar_left = MIR_ARENA;
    }
    *mem = g_mar_info.deviceMemory;
    *off = g_mar_info.offset + MIR_ARENA - g_mar_left;
    g_mar_left -= page;
    return 1;
}
/* The window's new blocks get their memory before the commands that copy into them are submitted. The queue's order
   doesn't cover a bind, so it's waited for. Under vk_lock */
static struct {
    u32 pool;
    VkSparseMemoryBind b;
} g_mb[(MIR_BUDGET >> MIR_SHIFT) + 1];
static u32 g_nmb;
static void mir_bind(void) {
    VkSparseBufferMemoryBindInfo info[IMPORTS];
    static VkSparseMemoryBind binds[(MIR_BUDGET >> MIR_SHIFT) + 1];
    VkBindSparseInfo bsi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pBufferBinds = info};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    u64 t0;
    u32 n = 0, k;
    int i;
    if (!g_nmb) return;
    t0 = plat_ns();
    for (i = 0; i < g_nimport; i++) {
        u32 first = n;
        for (k = 0; k < g_nmb; k++)
            if (g_mb[k].pool == (u32)i) binds[n++] = g_mb[k].b;
        if (n > first)
            info[bsi.bufferBindCount++] = (VkSparseBufferMemoryBindInfo){g_mir[i].buf, n - first, binds + first};
    }
    int bound = 0;
    if (vkCreateFence(g_vk.dev, &fci, NULL, &fence) == VK_SUCCESS) {
        vk_queue_lock();
        bound = vkQueueBindSparse(g_vk.gfx, 1, &bsi, fence) == VK_SUCCESS;
        vk_queue_unlock();
    }
    if (!bound || vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        fatal("nvn: binding video memory to a pool copy failed\n");
    vkDestroyFence(g_vk.dev, fence, NULL);
    g_mir_bound += g_nmb;
    g_nmb = 0;
    g_mir_bind_ns += plat_ns() - t0;
}
static void mir_cpu_at(int arg, u64 off, u64 n) { mir_cpu_wrote(arg >> 8, g_mir_at + off, n); }

/* ---- c7: storage buffers the GPU writes, in video memory (MIR_GPU). Ported from Witch Time's compute buffers ---- */
static u64 own_bsize(int p, u64 b) {
    u64 off = b << MIR_SHIFT, size = 1ull << MIR_SHIFT;
    return off + size > g_import[p].size ? g_import[p].size - off : size;
}
/* records the copy of block b of pool p into its video memory copy, now: 0 when there is no memory for it. Outside a
   pass; the caller binds (mir_bind) and puts a barrier after */
static int mir_fill_block(int p, u64 b) {
    u64 off = b << MIR_SHIFT, size = own_bsize(p, b);
    VkBufferCopy r = {off, off, size};
    if (!g_mir[p].chunk[off / MIR_CHUNK]) { /* its first block: the watch starts on it, before the copy */
        u64 at = off / MIR_CHUNK * MIR_CHUNK, n = MIR_CHUNK;
        if (at + n > g_import[p].size) n = g_import[p].size - at;
        g_mir[p].chunk[off / MIR_CHUNK] = 1;
        vk_written_since(GP(g_import[p].cpu + at), n, vk_ww_now());
    }
    if (g_mir[p].size && !g_mir[p].bound[b]) {
        VkDeviceMemory mem;
        u64 at, whole = 1ull << MIR_SHIFT;
        if (g_nmb == sizeof g_mb / sizeof g_mb[0]) mir_bind();
        if (!mir_page(&mem, &at)) return 0;
        if (off + whole > g_mir[p].size) whole = g_mir[p].size - off;
        g_mir[p].bound[b] = 1;
        g_mb[g_nmb].pool = (u32)p;
        g_mb[g_nmb++].b = (VkSparseMemoryBind){off, whole, mem, at, 0};
    }
    vkCmdCopyBuffer(g_rp.cb, g_import[p].buf, g_mir[p].buf, 1, &r);
    g_mir_bytes += size;
    return 1;
}
/* the GPU's bytes of block b, res, into guest memory: a byte the GPU changed against the snapshot goes in where the
   guest still holds the snapshot's byte (as writeback.c's vk_write_changed, on the pool's host memory). The block is
   the pool's again */
static void own_merge(int p, u64 b, const u8* res) {
    u64 off = b << MIR_SHIFT, size = own_bsize(p, b), i;
    u8* g = GP(g_import[p].cpu + off);
    const u8* s = g_mir[p].snap[b];
    for (i = 0; i + 8 <= size; i += 8) {
        u64 rv, sv;
        memcpy(&rv, res + i, 8);
        memcpy(&sv, s + i, 8);
        if (rv == sv) continue;
        if (!memcmp(g + i, s + i, 8)) {
            memcpy(g + i, res + i, 8);
            continue;
        }
        for (u64 k = i; k < i + 8; k++)
            if (res[k] != s[k] && g[k] == s[k]) g[k] = res[k];
    }
    for (; i < size; i++)
        if (res[i] != s[i] && g[i] == s[i]) g[i] = res[i];
    if (g_import[p].gpu) gpu_mem_written(g_import[p].gpu + off, size);
    free(g_mir[p].snap[b]);
    g_mir[p].snap[b] = NULL;
    g_mir[p].st[b] = MIR_NONE; /* the copy may hold the GPU's byte where the game's won: copied again when read */
    g_mir[p].landing[b] = 0;
    g_own_n--;
    g_land_bytes += size;
}
/* lands the blocks queued in g_lq now: copies to host memory, a submit and a wait, the merge. Mid-window too: what was
   recorded before goes with the submit, and recording goes on in a new command buffer */
static VkBuffer g_lb;
static VmaAllocation g_lb_mem;
static u8* g_lb_map;
static u64 g_lb_cap;
static int own_staging(VkBuffer* buf, VmaAllocation* mem, u8** map, u64* cap, u64 need) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo info;
    if (*cap >= need) return 1;
    if (*buf) vk_defer_buffer(*buf, *mem); /* no copy names it any more: the last landing waited for its submit */
    *buf = VK_NULL_HANDLE;
    *cap = 0;
    bci.size = (need + (4u << 20) - 1) & ~((4ull << 20) - 1);
    if (vmaCreateBuffer(g_vk.vma, &bci, &aci, buf, mem, &info) != VK_SUCCESS) return 0;
    *map = info.pMappedData;
    *cap = bci.size;
    return 1;
}
static void own_land_now(void) {
    u64 t0 = plat_ns(), gate;
    u32 k, n = g_nlq;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    if (!n || !g_rp_win) return; /* outside a window: kept for the next one's scan */
    if (!own_staging(&g_lb, &g_lb_mem, &g_lb_map, &g_lb_cap, (u64)n << MIR_SHIFT))
        fatal("nvn: no host memory to land %u blocks of GPU-written storage buffers\n", n);
    rp_end_rendering();
    rp_barrier();
    for (k = 0; k < n; k++) {
        VkBufferCopy r = {(u64)g_lq[k].blk << MIR_SHIFT, (u64)k << MIR_SHIFT, own_bsize(g_lq[k].pool, g_lq[k].blk)};
        vkCmdCopyBuffer(g_rp.cb, g_mir[g_lq[k].pool].buf, g_lb, 1, &r);
    }
    vkCmdPipelineBarrier2(g_rp.cb, &di);
    gate = vk_submit_number();
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_wait_submit(gate);
    g_rp.cb = vk_cmd();
    rp_barrier();
    vmaInvalidateAllocation(g_vk.vma, g_lb_mem, 0, VK_WHOLE_SIZE);
    for (k = 0; k < n; k++)
        if (g_mir[g_lq[k].pool].st[g_lq[k].blk] == MIR_GPU)
            own_merge((int)g_lq[k].pool, g_lq[k].blk, g_lb_map + ((u64)k << MIR_SHIFT));
    g_nlq = 0;
    g_land_forced += n;
    g_land_by[g_land_why] += n;
    g_land_ns += plat_ns() - t0;
}
/* [off, off + size) of pool p is about to be read or written where the copy isn't: its GPU blocks land first */
static void own_land_range(int p, u64 off, u64 size) {
    u64 b, last;
    if (!g_own_n || p < 0 || !g_mir[p].st || !size) return;
    last = (off + size - 1) >> MIR_SHIFT;
    for (b = off >> MIR_SHIFT; b <= last; b++)
        if (g_mir[p].st[b] == MIR_GPU) own_lq((u32)p, (u32)b);
    own_land_now();
}
static void own_land_gpu(u64 gpu, u64 size) {
    u64 off;
    int p;
    if (!g_own_n || !size) return;
    p = import_of(gpu, size, &off);
    if (p >= 0) {
        own_land_range(p, off, size);
        return;
    }
    for (p = 0; p < g_nimport; p++) /* across pools: each one's part */
        if (g_import[p].gpu && gpu < g_import[p].gpu + g_import[p].size && gpu + size > g_import[p].gpu) {
            u64 lo = gpu > g_import[p].gpu ? gpu - g_import[p].gpu : 0;
            u64 hi = gpu + size - g_import[p].gpu < g_import[p].size ? gpu + size - g_import[p].gpu : g_import[p].size;
            own_land_range(p, lo, hi - lo);
        }
}
/* The game wrote GPU blocks (the window's scan): the bytes it changed against the snapshot go into the copy from
   staging, and into the snapshot, so the GPU reads them and a later landing doesn't take them for its own. The GPU's
   bytes stay, no wait. A byte both changed is the game's, as the merge decides. At a window's opening, before its
   commands */
#define OWN_RUNS 2048u /* changed runs a block patches; more and it lands instead */
static void own_patch(void) {
    static VkBufferCopy r[OWN_RUNS];
    u32 k;
    int any = 0;
    for (k = 0; k < g_nopq; k++) {
        int p = (int)g_opq[k].pool;
        u64 b = g_opq[k].blk, bo = b << MIR_SHIFT, bs = own_bsize(p, b), i = 0, total = 0, at;
        static u8 cur[1u << MIR_SHIFT]; /* the game's bytes now: it may go on writing while this looks (vk_lock) */
        const u8* s = g_mir[p].snap[b];
        u32 n = 0, j;
        u8* q;
        if (g_mir[p].st[b] != MIR_GPU || !s) continue;
        memcpy(cur, GP(g_import[p].cpu + bo), bs);
        /* only the bytes the game changed: one it didn't may hold the GPU's result in the copy */
        while (i < bs && n < OWN_RUNS) {
            u64 start;
            while (i + 8 <= bs && !memcmp(cur + i, s + i, 8)) i += 8;
            while (i < bs && cur[i] == s[i]) i++;
            if (i >= bs) break;
            start = i;
            while (i < bs && cur[i] != s[i]) i++;
            r[n].srcOffset = total; /* relative to the staging bytes until they have a place */
            r[n].dstOffset = bo + start;
            r[n++].size = i - start;
            total += i - start;
        }
        if (i < bs && n == OWN_RUNS) { /* scattered writes: it lands */
            own_lq((u32)p, (u32)b);
            continue;
        }
        if (!n) continue;
        if (!(q = rp_alloc(total, &at))) {
            own_lq((u32)p, (u32)b);
            continue;
        }
        for (j = 0; j < n; j++) {
            memcpy(q + r[j].srcOffset, cur + (r[j].dstOffset - bo), r[j].size);
            memcpy(g_mir[p].snap[b] + (r[j].dstOffset - bo), cur + (r[j].dstOffset - bo), r[j].size);
            r[j].srcOffset += at;
        }
        g_mir[p].landing[b] = 0; /* an idle landing's copy holds the old bytes: its merge would undo the game's */
        if (!any) {
            rp_end_rendering();
            rp_barrier();
            any = 1;
        }
        vkCmdCopyBuffer(g_rp.cb, g_rp.sbuf, g_mir[p].buf, n, r);
        g_patch_blocks++;
        g_patch_bytes += total;
    }
    g_nopq = 0;
    if (any) rp_barrier();
}
/* 1 when [off, off + size) of pool p has a GPU block */
static int own_touches(int p, u64 off, u64 size) {
    u64 b, last;
    if (!g_own_n || p < 0 || !g_mir[p].st || !size) return 0;
    last = (off + size - 1) >> MIR_SHIFT;
    for (b = off >> MIR_SHIFT; b <= last; b++)
        if (g_mir[p].st[b] == MIR_GPU) return 1;
    return 0;
}
/* 1 when [off, off + size) of pool p can be bound from its video memory copy for the GPU to write: its blocks are the
   GPU's from now on, filled from the pool (with a snapshot) when they weren't. 0: bind the pool, nothing owned */
static int mir_own(int p, u64 off, u64 size) {
    u64 b, first, last;
    int filled = 0;
    if (!own_on() || !g_mir[p].buf || !g_mir[p].want || !g_mir[p].snap || !size) return 0;
    first = off >> MIR_SHIFT;
    last = (off + size - 1) >> MIR_SHIFT;
    for (b = first; b <= last; b++) {
        u64 bs;
        if (g_mir[p].st[b] == MIR_GPU) continue;
        bs = own_bsize(p, b);
        if (!filled) {
            rp_end_rendering();
            rp_barrier();
            filled = 1;
        }
        /* filled again even when valid: the snapshot has to be what the copy holds. A game write between the two
           lands as the game's (the merge keeps a byte where guest and snapshot differ) */
        g_mir[p].snap[b] = malloc(bs);
        if (!g_mir[p].snap[b] || !mir_fill_block(p, b)) {
            free(g_mir[p].snap[b]);
            g_mir[p].snap[b] = NULL;
            g_mir[p].st[b] = MIR_NONE;
            g_own_fail++;
            mir_bind();
            rp_barrier();
            g_land_why = 3;
            own_land_range(p, off, size); /* what this range already owns goes back: the pool is bound */
            return 0;
        }
        memcpy(g_mir[p].snap[b], GP(g_import[p].cpu + (b << MIR_SHIFT)), bs);
        g_mir[p].st[b] = MIR_GPU;
        g_own_n++;
        g_own_made++;
        g_own_fill += bs;
    }
    if (filled) {
        mir_bind();
        rp_barrier();
    }
    for (b = first; b <= last; b++) {
        g_mir[p].ofr[b] = (u32)g_rp.frame;
        g_mir[p].landing[b] = 0; /* written again: an idle landing on its way is out of date */
    }
    return 1;
}
/* An idle GPU block (no write for two frames) lands without a wait: its copy goes to host memory with this window,
   and once that submit is done the merge, at a later window. The game reading compute's output from guest memory
   finds it there two frames after the last write at most */
#define OWN_IDLE 64u
static VkBuffer g_ib;
static VmaAllocation g_ib_mem;
static u8* g_ib_map;
static u64 g_ib_cap;
static struct {
    u32 pool, blk;
    u64 gate;     /* 0: merged or dropped */
} g_il[OWN_IDLE]; /* entry k's copy is in slot k of g_ib */
static u32 g_nil;
static void own_idle(void) {
    u32 k, open = 0;
    int p;
    for (k = 0; k < g_nil; k++) { /* landings whose copy is done */
        u32 pl = g_il[k].pool, b = g_il[k].blk;
        if (!g_il[k].gate) continue;
        if (!vk_submit_done(g_il[k].gate)) {
            open++;
            continue;
        }
        g_il[k].gate = 0;
        if (g_mir[pl].st[b] == MIR_GPU && g_mir[pl].landing[b]) { /* not written since, not landed otherwise */
            vmaInvalidateAllocation(g_vk.vma, g_ib_mem, (u64)k << MIR_SHIFT, 1ull << MIR_SHIFT);
            own_merge((int)pl, b, g_ib_map + ((u64)k << MIR_SHIFT));
            g_land_idle++;
        }
    }
    if (open) return; /* the slots are given out again only when all are free */
    g_nil = 0;
    if (!g_own_n || !own_staging(&g_ib, &g_ib_mem, &g_ib_map, &g_ib_cap, (u64)OWN_IDLE << MIR_SHIFT)) return;
    for (p = 0; p < g_nimport && g_nil < OWN_IDLE; p++) {
        u64 b, blocks;
        if (!g_mir[p].st || !g_mir[p].snap) continue;
        blocks = (g_import[p].size + (1u << MIR_SHIFT) - 1) >> MIR_SHIFT;
        for (b = 0; b < blocks && g_nil < OWN_IDLE; b++) {
            VkBufferCopy r;
            if (g_mir[p].st[b] != MIR_GPU || g_mir[p].landing[b] || (u32)g_rp.frame - g_mir[p].ofr[b] < 2) continue;
            r = (VkBufferCopy){b << MIR_SHIFT, (u64)g_nil << MIR_SHIFT, own_bsize(p, b)};
            vkCmdCopyBuffer(g_rp.cb, g_mir[p].buf, g_ib, 1, &r);
            g_mir[p].landing[b] = 1;
            g_il[g_nil].pool = (u32)p;
            g_il[g_nil].blk = (u32)b;
            g_il[g_nil++].gate = vk_submit_number();
        }
    }
}
/* a window opens: what the CPU wrote goes back to the pool, and the blocks asked for are copied. Under vk_lock, outside
   a pass */
static void rp_mirror_window(void) {
    u64 t0 = plat_ns(), budget = MIR_BUDGET;
    u32 k, kept = 0;
    int i, copied = 0;
    g_mir_window++;
    for (i = 0; i < g_nimport; i++) {
        u64 c, chunks = (g_import[i].size + MIR_CHUNK - 1) / MIR_CHUNK;
        u32 seen = g_mir[i].wseen;
        if (!g_mir[i].want || (!g_mir[i].buf && !mir_make(i))) continue;
        /* only the chunks with blocks copied: the watch's cost goes with the range asked about */
        for (c = 0; c < chunks && g_mir[i].want;) {
            u64 at, n;
            int w;
            if (!g_mir[i].chunk[c]) {
                c++;
                continue;
            }
            at = c * MIR_CHUNK;
            while (c < chunks && g_mir[i].chunk[c]) c++;
            n = c * MIR_CHUNK - at;
            if (at + n > g_import[i].size) n = g_import[i].size - at;
            g_mir_at = at;
            w = vk_written_since(GP(g_import[i].cpu + at), n, seen);
            if (w < 0) { /* no tracking: nothing can be known equal */
                u64 b, blocks = (g_import[i].size + (1u << MIR_SHIFT) - 1) >> MIR_SHIFT;
                for (b = 0; b < blocks; b++)
                    if (g_mir[i].st[b] == MIR_GPU) own_lq((u32)i, (u32)b);
                g_land_why = 4;
                own_land_now(); /* the GPU's blocks first: they are the truth */
                memset(g_mir[i].st, MIR_NONE, blocks);
                g_mir[i].want = 0;
            } else if (w)
                vk_written_each(GP(g_import[i].cpu + at), n, seen, mir_cpu_at, i << 8);
        }
    }
    g_mir_scan_ns += plat_ns() - t0;
    own_patch(); /* GPU blocks the game wrote: its bytes into the copy */
    g_land_why = 0;
    own_land_now(); /* those that couldn't be patched: merged, its words stay */
    own_idle();
    for (k = 0; k < g_nmq; k++) {
        u32 p = g_mq[k].pool, b = g_mq[k].blk;
        u64 off = (u64)b << MIR_SHIFT, size = 1ull << MIR_SHIFT;
        if (g_mir[p].st[b] != MIR_QUEUED) continue;
        if (budget < size) {
            g_mq[kept++] = g_mq[k];
            continue;
        }
        if (off + size > g_import[p].size) size = g_import[p].size - off;
        if (!g_mir[p].chunk[off / MIR_CHUNK]) { /* its first block: the watch starts on it, before the copy */
            u64 at = off / MIR_CHUNK * MIR_CHUNK, n = MIR_CHUNK;
            if (at + n > g_import[p].size) n = g_import[p].size - at;
            g_mir[p].chunk[off / MIR_CHUNK] = 1;
            vk_written_since(GP(g_import[p].cpu + at), n, vk_ww_now());
        }
        if (g_mir[p].size && !g_mir[p].bound[b]) {
            VkDeviceMemory mem;
            u64 at, whole = 1ull << MIR_SHIFT;
            if (!mir_page(&mem, &at)) { /* no room: the block stays in the pool */
                g_mir[p].st[b] = MIR_NONE;
                g_mir[p].hot[b] = 255;
                continue;
            }
            if (off + whole > g_mir[p].size) whole = g_mir[p].size - off;
            g_mir[p].bound[b] = 1;
            g_mb[g_nmb].pool = p;
            g_mb[g_nmb++].b = (VkSparseMemoryBind){off, whole, mem, at, 0};
        }
        {
            VkBufferCopy r = {off, off, size};
            if (!copied) rp_end_rendering();
            vkCmdCopyBuffer(g_rp.cb, g_import[p].buf, g_mir[p].buf, 1, &r);
        }
        g_mir[p].st[b] = MIR_VALID;
        budget -= size;
        g_mir_bytes += size;
        copied = 1;
    }
    g_nmq = kept;
    mir_bind();
    for (i = 0; i < g_nimport; i++)
        if (g_mir[i].buf) g_mir[i].wseen = vk_ww_now(); /* after every scan of this window, the starting ones too */
    if (copied) rp_barrier();
}
/* SNX_NVN_VRAM_CHECK=1: blocks the copies call equal, read back and compared with guest memory every 300 frames. One
   that differs and that the write watch hasn't seen written is what must never happen. Under vk_lock, between windows */
#define MIR_CHECK 256u
static void rp_mirror_check(void) {
    static int on = -1;
    static VkBuffer buf;
    static VmaAllocation mem;
    static u8* map;
    static u32 cursor;
    static u64 checked, same, written, stale;
    struct {
        u32 pool, blk;
    } at[MIR_CHECK];
    u32 n = 0, k;
    u64 gate, scanned = 0, total = 0;
    int i;
    if (on < 0) {
        const char* e = getenv("SNX_NVN_VRAM_CHECK");
        on = e && e[0] == '1';
        if (on) {
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = (u64)MIR_CHECK << MIR_SHIFT,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                    VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                           .usage = VMA_MEMORY_USAGE_AUTO};
            VmaAllocationInfo info;
            if (vmaCreateBuffer(g_vk.vma, &bci, &aci, &buf, &mem, &info) != VK_SUCCESS) on = 0;
            map = info.pMappedData;
        }
    }
    if (!on) return;
    for (i = 0; i < g_nimport; i++)
        if (g_mir[i].st) total += (g_import[i].size + (1u << MIR_SHIFT) - 1) >> MIR_SHIFT;
    if (!total) return;
    while (n < MIR_CHECK && scanned < total) { /* the next valid blocks after the last looked at */
        u64 c = cursor++ % total;
        scanned++;
        for (i = 0; i < g_nimport; i++) {
            u64 blocks;
            if (!g_mir[i].st) continue;
            blocks = (g_import[i].size + (1u << MIR_SHIFT) - 1) >> MIR_SHIFT;
            if (c >= blocks) {
                c -= blocks;
                continue;
            }
            if (g_mir[i].st[c] == MIR_VALID) {
                at[n].pool = (u32)i;
                at[n++].blk = (u32)c;
            }
            break;
        }
    }
    if (!n) return;
    vk_render_end();
    g_rp.cb = vk_cmd();
    rp_barrier();
    for (k = 0; k < n; k++) {
        u64 off = (u64)at[k].blk << MIR_SHIFT, size = 1ull << MIR_SHIFT;
        VkBufferCopy r;
        if (off + size > g_import[at[k].pool].size) size = g_import[at[k].pool].size - off;
        r = (VkBufferCopy){off, (u64)k << MIR_SHIFT, size};
        vkCmdCopyBuffer(g_rp.cb, g_mir[at[k].pool].buf, buf, 1, &r);
    }
    gate = vk_submit_number();
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_wait_submit(gate);
    vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    for (k = 0; k < n; k++) {
        u32 pool = at[k].pool;
        u64 off = (u64)at[k].blk << MIR_SHIFT, size = 1ull << MIR_SHIFT;
        const u8* guest;
        if (off + size > g_import[pool].size) size = g_import[pool].size - off;
        guest = GP(g_import[pool].cpu + off);
        checked++;
        if (g_mir[pool].st[at[k].blk] != MIR_VALID) continue;
        if (!memcmp(guest, map + ((u64)k << MIR_SHIFT), size))
            same++;
        else if (vk_written_since(guest, size, g_mir[pool].wseen) > 0)
            written++; /* since the window's scan: the next one takes it back */
        else {
            stale++;
            LOG_LIMIT(32, "nvn-live: STALE video memory copy: pool %u block %u (GPU %llx)\n", pool, at[k].blk,
                      (unsigned long long)(g_import[pool].gpu + off));
        }
    }
    LOG_LIMIT(400, "nvn-live: video memory check: %llu blocks read back, equal %llu, written since %llu, STALE %llu\n",
              (unsigned long long)checked, (unsigned long long)same, (unsigned long long)written,
              (unsigned long long)stale);
}
/* the buffer to read [off, off + size) of a pool from: its copy when every block is there, else the pool */
static VkBuffer rp_read_buf(int pool, u64 off, u64 size) {
    u64 b, last;
    int all = 1, gpu = 0;
    if (!g_mir[pool].buf || !g_mir[pool].want || !size) return g_import[pool].buf;
    last = (off + size - 1) >> MIR_SHIFT;
    for (b = off >> MIR_SHIFT; b <= last; b++) {
        g_mir[pool].read[b] = 1;
        gpu |= g_mir[pool].st[b] == MIR_GPU;
        if (g_mir[pool].st[b] != MIR_VALID && g_mir[pool].st[b] != MIR_GPU) {
            all = 0;
            if (g_mir[pool].st[b] == MIR_NONE && g_mir[pool].hot[b] < MIR_HOT) mir_queue(pool, b);
        }
    }
    if (!all && gpu) { /* part of it only in the copy: the rest is copied there now, else the GPU's blocks land */
        int ok = 1;
        rp_end_rendering();
        rp_barrier();
        for (b = off >> MIR_SHIFT; b <= last && ok; b++)
            if (g_mir[pool].st[b] != MIR_VALID && g_mir[pool].st[b] != MIR_GPU) {
                ok = mir_fill_block(pool, b);
                if (ok) g_mir[pool].st[b] = MIR_VALID;
            }
        mir_bind();
        rp_barrier();
        if (!ok) {
            g_land_why = 5;
            own_land_range(pool, off, size);
            return g_import[pool].buf;
        }
        all = 1;
    }
    if (all)
        g_mir_hits++;
    else
        g_mir_miss++;
    return all ? g_mir[pool].buf : g_import[pool].buf;
}
/* What the host wrote into guest memory goes to the texture cache once it is there: a lookup between the recording
   and the GPU's run would read the old bytes and take them for current. The ranges of the windows since the last
   stamp wait for the submission that holds them */
#define RP_STAMPS 1024
static struct {
    u64 gpu, size;
} g_stamp[RP_STAMPS];
static u32 g_nstamp, g_nstamp_sent;
static u64 g_stamp_gate;
static void rp_stamp(u64 gpu, u64 size) {
    u64 at;
    int pool = import_of(gpu, size, &at);
    u32 k;
    if (pool >= 0) mir_take(pool, at, size);
    for (k = g_nstamp_sent; k < g_nstamp; k++)
        if (g_stamp[k].gpu == gpu && g_stamp[k].size == size) return;
    if (g_nstamp == RP_STAMPS) { /* full: now, a frame late at worst */
        gpu_mem_written(gpu, size);
        return;
    }
    g_stamp[g_nstamp].gpu = gpu;
    g_stamp[g_nstamp++].size = size;
}
static void rp_stamps_done(void) { /* a window opens */
    u32 k;
    if (!g_nstamp_sent || !vk_submit_done(g_stamp_gate)) return;
    for (k = 0; k < g_nstamp_sent; k++) gpu_mem_written(g_stamp[k].gpu, g_stamp[k].size);
    memmove(g_stamp, g_stamp + g_nstamp_sent, (g_nstamp - g_nstamp_sent) * sizeof *g_stamp);
    g_nstamp -= g_nstamp_sent;
    g_nstamp_sent = 0;
}
static int rp_ssbo(u64 gpu, u64 size, int written, VkDescriptorBufferInfo* bi) {
    u32 k;
    if (g_rp.real) {
        if (written) {
            rp_stamp(gpu, size);
            dl_written(gpu, size);
        }
        return 0;
    }
    for (k = 0; k < g_nrpw; k++)
        if (gpu >= g_rpw[k].gpu && gpu + size <= g_rpw[k].gpu + g_rpw[k].size) {
            *bi = (VkDescriptorBufferInfo){g_rpw[k].buf, g_rpw[k].off + (gpu - g_rpw[k].gpu), size};
            return 1;
        }
    if (!written) return 0;
    if (g_nrpw == RP_WRITTEN || size > (64u << 20)) {
        g_rpw_full++;
        bi->buffer = VK_NULL_HANDLE;
        return 1;
    }
    if (!rp_copy(bi, gpu, size, size)) {
        bi->buffer = VK_NULL_HANDLE;
        return 1;
    }
    g_rpw[g_nrpw].gpu = gpu;
    g_rpw[g_nrpw].size = size;
    g_rpw[g_nrpw].buf = bi->buffer;
    g_rpw[g_nrpw].p = g_rp.last_p;
    g_rpw[g_nrpw++].off = bi->offset;
    return 1;
}
static int rp_debug(void);
/* the image in place of the texture a TIC describes, -1 for none: one the replay drew at its address, of its size.
   Live images stay from frame to frame, so another texture can come to that address: the same memory holding an
   atlas after a target of another size */
static int rp_target_of(const u32* tic, u64 gva) {
    u32 k, w = (tic[4] & 0xFFFF) + 1, h = (tic[5] & 0xFFFF) + 1;
    static u64 lo = 1, hi; /* SNX_NVN_NOSUB=lo-hi: none in those addresses */
    if (lo == 1) {
        const char* e = getenv("SNX_NVN_NOSUB");
        char* end;
        lo = hi = 0;
        if (e) {
            lo = strtoull(e, &end, 16);
            hi = *end ? strtoull(end + 1, NULL, 16) : lo + 1;
        }
    }
    if (rp_debug() & 16 || (gva >= lo && gva < hi)) return -1; /* 16: no substitution */
    for (k = 0; k < (u32)g_rp.nt; k++)
        if (g_rp.t[k].gpu == gva && g_rp.t[k].w == w && g_rp.t[k].h == h) {
            g_rp.t[k].used = g_rp.frame;
            return (int)k;
        }
    return -1;
}
/* a stage's descriptor set for the replay; 0 with *why when a binding can't be made */
/* SNX_NVN_REPLAY_DEBUG: 1 no culling, 2 no depth test, 4 no scissor, 8 no alpha test, 16 textures of this frame's
   targets from the renderer instead of the replay's images, 32 depth EQUAL as LEQUAL, 64 no compute, 128 constant
   buffer 1 as far as the program reads */
static int rp_debug(void) {
    static int dbg = -1;
    if (dbg < 0) {
        const char* e = getenv("SNX_NVN_REPLAY_DEBUG");
        dbg = e ? atoi(e) : 0;
    }
    return dbg;
}
static int rp_stage_set(int mod, const NvnStageBind* b, int stage, VkDescriptorSet* set, const char** why) {
    static VkWriteDescriptorSet w[SNX_VKB_END];
    static VkDescriptorBufferInfo bi[SNX_VKB_END];
    static VkDescriptorImageInfo ii[SNX_VKB_END];
    const ShaderInfo* si = &g_mod[mod].si;
    u64 salign = g_vk.props.limits.minStorageBufferOffsetAlignment, sb_addr[16] = {0}, sb_size[16] = {0}, off;
    u32 i, n = 0, base = b->nvn_stage == 5 ? 0x310 : 0x110 + 0x100 * g_hw_stage[b->nvn_stage % 6];
    int nt = si->ntex > SH_MAXTEX ? SH_MAXTEX : si->ntex, slot;
    float* rs_slots = NULL;
    if (!rp_set_alloc(g_vk_dslayout[shader_vk_set(stage)], set)) return *why = "descriptor pools used up", 0;
#define WB(binding, type)                                                                                              \
    (w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,                                  \
                                     .dstSet = *set,                                                                   \
                                     .dstBinding = (binding),                                                          \
                                     .descriptorCount = 1,                                                             \
                                     .descriptorType = (type),                                                         \
                                     .pBufferInfo = &bi[binding]})
    for (i = 0; i < (u32)si->ngbuf && i < 16; i++) { /* storage buffers: from the bound range's aligned start */
        u32 k = (si->gbuf_cbuf_off[i] - base) / 16;
        u64 at, delta;
        int pool;
        if (si->gbuf_cbuf_off[i] < base || k >= 16 || !b->ssbo[k].addr || !b->ssbo[k].size)
            return *why = "storage buffer not bound", 0;
        pool = import_of(b->ssbo[k].addr, b->ssbo[k].size, &at);
        if (pool < 0) return *why = "storage buffer outside the pools", 0;
        delta = at & (salign - 1);
        sb_addr[i] = b->ssbo[k].addr - delta;
        sb_size[i] = b->ssbo[k].size + delta;
        if (g_rp.real && (si->gbuf_wmask >> i & 1)) {
            /* c7: written, so from the video memory copy, the GPU's blocks until they land (mir_own): over the pool
               every access of it crossed PCIe, 15 ms for one game compute job after a verse exit */
            if (mir_own(pool, at - delta, sb_size[i])) {
                bi[SNX_VKB_SSBO + i] = (VkDescriptorBufferInfo){g_mir[pool].buf, at - delta, sb_size[i]};
                dl_written(sb_addr[i], sb_size[i]);
                WB(SNX_VKB_SSBO + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                continue;
            }
            g_land_why = 6;
            own_land_range(pool, at - delta, sb_size[i]); /* written in the pool: what the copy owns of it first */
        }
        if (!rp_ssbo(sb_addr[i], sb_size[i], si->gbuf_wmask >> i & 1, &bi[SNX_VKB_SSBO + i]))
            bi[SNX_VKB_SSBO + i] = (VkDescriptorBufferInfo){
                si->gbuf_wmask >> i & 1 ? g_import[pool].buf : rp_read_buf(pool, at - delta, sb_size[i]), at - delta,
                sb_size[i]};
        else if (!bi[SNX_VKB_SSBO + i].buffer)
            return *why = "staging used up (written storage buffer)", 0;
        WB(SNX_VKB_SSBO + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    if (si->ngbuf && stage != ST_CS) { /* no page maps: every slot's table entry 0xffffffff */
        u8* p = rp_alloc(48 * 4, &off);
        if (!p) return *why = "staging used up", 0;
        memset(p, 0, 48 * 4);
        memset(p, 0xFF, 16 * 4);
        bi[SNX_VKB_SMAP] = (VkDescriptorBufferInfo){g_rp.sbuf, off, 48 * 4};
        WB(SNX_VKB_SMAP, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    for (i = 0; i < 18; i++) { /* constant buffers, the bytes the program reads */
        u32 size = si->cbuf_max[i] ? (si->cbuf_max[i] + 15) & ~15u : 0x10000;
        if (!(si->cbuf_mask >> i & 1)) continue;
        if (size > g_vk.props.limits.maxUniformBufferRange) size = g_vk.props.limits.maxUniformBufferRange;
        if (i == 0 || i == 2) { /* the host's: storage buffer addresses, bound handles */
            u8* p = rp_alloc(size, &off);
            u32 j;
            if (!p) return *why = "staging used up", 0;
            memset(p, 0, size);
            if (i == 0)
                for (j = 0; j < (u32)si->ngbuf && j < 16; j++) {
                    u32 at = si->gbuf_cbuf_off[j],
                        v[4] = {(u32)sb_addr[j], (u32)(sb_addr[j] >> 32), (u32)sb_size[j], 0};
                    if (at + 16 <= size) memcpy(p + at, v, 16);
                }
            else
                for (j = 0; j < 40 && (8 + 2 * j + 2) * 4 <= size; j++) {
                    u64 h = j < 32 ? b->tex[j] : b->img[j - 32];
                    memcpy(p + (8 + 2 * j) * 4, &h, 8);
                }
            bi[SNX_VKB_CB + i] = (VkDescriptorBufferInfo){g_rp.sbuf, off, size};
        } else {
            u64 a = i == 1 ? b->cb1 : b->ubo[i - 3].addr, have = i == 1 ? b->cb1_size : b->ubo[i - 3].size;
            /* with real writes the pool itself where it can be bound: a compute pass of the frame may fill it. The
               range is what the game bound: past it robust buffer access reads zero, as the zeros of a copy, and a
               program of indexed reads (declared 64 KiB) would otherwise cost a 64 KiB copy a draw: 99% of staging */
            u64 bind = have < size ? have : size;
            int pool = g_rp.real && !(rp_debug() & 256) && a && bind ? import_of(a, bind, &off) : -1;
            if (i == 1 && (rp_debug() & 128)) have = size; /* 128: constant buffer 1 as far as the program reads */
            if (pool >= 0 && !(off & (g_vk.props.limits.minUniformBufferOffsetAlignment - 1)))
                bi[SNX_VKB_CB + i] = (VkDescriptorBufferInfo){rp_read_buf(pool, off, bind), off, bind};
            else if (!rp_copy(&bi[SNX_VKB_CB + i], a, a ? have : 0, size))
                return *why = "staging used up", 0;
        }
        WB(SNX_VKB_CB + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }
    if (stage == ST_FS) { /* the game's pixels of this pass's, then each texture slot's image / game ratio */
        const RpTarget* pt = rp_pass_target(); /* set before the pass begins: g_rp.psx may be the last pass's */
        float px = pt ? pt->sx : 1.0f, py = pt ? pt->sy : 1.0f;
        u32 size = rp_scaling() ? 16 * (1 + SH_MAXTEX) : 16;
        float rs[4] = {1 / px, 1 / py, px, 0};
        u8* p = rp_alloc(size, &off);
        if (!p) return *why = "staging used up", 0;
        memcpy(p, rs, 16);
        if (size > 16) {
            rs_slots = (float*)(p + 16);
            for (i = 0; i < 4 * SH_MAXTEX; i++) rs_slots[i] = 1;
        }
        bi[SNX_VKB_RS] = (VkDescriptorBufferInfo){g_rp.sbuf, off, size};
        WB(SNX_VKB_RS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }
#undef WB
    for (i = 0; i < (u32)nt; i++) {
        const ShTex* st = &si->tex[i];
        ShTex plain;
        u32 handle;
        int num, target;
        u64 gva;
        VkImageView view;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (st->flags & (SNX_TEX_INDEXED | 2)) return *why = "indexed or multisample texture", 0;
        if (st->shadow && (si->tex_dc_color_mask >> i & 1)) {
            plain = *st;
            plain.shadow = 0;
            st = &plain;
        }
        num = st->flags & 1 ? 1 : st->shadow || !(si->tex_int_mask >> i & 1) ? 0 : (si->tex_sint_mask >> i & 1) ? 2 : 1;
        if (!tex_handle(b, st, &handle, why) || !tex_get(b, handle, st, num, 0, &slot, why)) return 0;
        view = g_tc[slot].view;
        gva = ((u64)(g_tc[slot].tic[2] & 0xFFFF) << 32) | g_tc[slot].tic[1];
        target = rp_target_of(g_tc[slot].tic, gva);
        if (g_rp.debug_frame && target >= 0) /* SNX_NVN_LIVE_DUMP's frame: what each draw samples of the host's */
            LOG("nvn-live: draw %llu stage %d tex %u: image %d (GPU %llx, format %d), bound %d, copy %d, TIC %08x "
                "%08x, view type %d, colour %d %d depth %d (%ux%u)\n",
                (unsigned long long)g_rp.draws, stage, i, target, (unsigned long long)g_rp.t[target].gpu,
                (int)g_rp.t[target].f, rp_bound(target), g_rp.t[target].fb, g_tc[slot].tic[0], g_tc[slot].tic[2],
                (int)vk_tex_view_type(st), g_rp.col[0], g_rp.col[1], g_rp.dep, g_rp.t[target].w, g_rp.t[target].h);
        if (target >= 0 && g_rp.t[target].fb && rp_bound(target)) target = g_rp.t[target].fb - 1;
        if (target >= 0) { /* drawn earlier in this frame: the replay's image */
            static const VkComponentSwizzle sw[8] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                                                     VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                     VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A,
                                                     VK_COMPONENT_SWIZZLE_ONE,  VK_COMPONENT_SWIZZLE_ONE};
            const u32* tic = g_tc[slot].tic;
            VkComponentMapping map = {sw[(tic[0] >> 19) & 7], sw[(tic[0] >> 22) & 7], sw[(tic[0] >> 25) & 7],
                                      sw[(tic[0] >> 28) & 7]};
            RpTarget* r = &g_rp.t[target];
            if (rs_slots) {
                rs_slots[4 * i] = r->sx;
                rs_slots[4 * i + 1] = r->sy;
            }
            /* as the renderer's vk_tex_bind_view: the header's swizzle is in memory component order (B, G, R, A
               formats swap R and B), depth reads as R, R, R, 1, and the view is in the header's format, sRGB when the
               header and the sampler ask for it; the image's own format when the header's can't view it */
            VkFmt tf = vk_tic_format(tic[0], 0);
            int srgb = ((tic[4] >> 22) & 1) && ((g_tc[slot].tsc[0] >> 13) & 1), k2;
            VkComponentSwizzle* mc[4] = {&map.r, &map.g, &map.b, &map.a};
            if (tf.bgra)
                for (k2 = 0; k2 < 4; k2++)
                    *mc[k2] = *mc[k2] == VK_COMPONENT_SWIZZLE_R   ? VK_COMPONENT_SWIZZLE_B
                              : *mc[k2] == VK_COMPONENT_SWIZZLE_B ? VK_COMPONENT_SWIZZLE_R
                                                                  : *mc[k2];
            if (r->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
                map = (VkComponentMapping){VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                           VK_COMPONENT_SWIZZLE_ONE};
            view = VK_NULL_HANDLE;
            if (r->aspect == VK_IMAGE_ASPECT_COLOR_BIT && tf.vk && !tf.conv && !tf.depth)
                view = rp_view(target, vk_srgb_as(tf.vk, srgb), vk_tex_view_type(st), VK_IMAGE_ASPECT_COLOR_BIT, &map,
                               ~0u);
            if (!view) /* a depth target reads as depth, or as its stencil when the header asks for that */
                view = rp_view(target, r->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? vk_srgb_as(r->f, srgb) : r->f,
                               vk_tex_view_type(st),
                               (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && vk_tic_reads_stencil(tic)
                                   ? VK_IMAGE_ASPECT_STENCIL_BIT
                               : r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                       : r->aspect,
                               &map, ~0u);
            layout = VK_IMAGE_LAYOUT_GENERAL;
        } else if (!g_rp.live && !g_tc[slot].null_view && g_tc[slot].t) {
            u32 u;
            for (u = 0; u < g_rp.nused && g_rp.used[u] != g_tc[slot].t; u++) {}
            if (u == g_rp.nused) {
                if (u == RP_USED) return *why = "too many textures", 0;
                g_rp.used[g_rp.nused++] = g_tc[slot].t;
            }
        }
        if (!view) return *why = "no view", 0;
        ii[SNX_VKB_TEX + i] = (VkDescriptorImageInfo){g_tc[slot].smp, view, layout};
        w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = *set,
                                        .dstBinding = SNX_VKB_TEX + i,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .pImageInfo = &ii[SNX_VKB_TEX + i]};
    }
    for (i = 0; i < (u32)si->nimg && i < 8; i++) {
        /* storage images: the replay's image of that address (made from the descriptor when no draw made it), so the
           store lands where later reads of the frame find it and the renderer's images never change */
        ShTex as;
        NvnTarget nt;
        u32 handle, tic[8];
        int target;
        VkImageView view;
        memset(&as, 0, sizeof as);
        as.cbuf_off = si->img[i].cbuf_off;
        as.bindless = si->img[i].bindless;
        as.bl_cbuf = si->img[i].bl_cbuf;
        if (!tex_handle(b, &as, &handle, why)) return 0;
        if (!b->tex_pool || (handle & 0xFFFFF) >= b->tex_count) return *why = "image id past its pool", 0;
        memcpy(tic, GP(b->tex_pool + (u64)(handle & 0xFFFFF) * 32), 32);
        nt =
            (NvnTarget){((u64)(tic[2] & 0xFFFF) << 32) | tic[1], (tic[4] & 0xFFFF) + 1, (tic[5] & 0xFFFF) + 1, 0, 1, 0};
        target = rp_target_vk(&nt, vk_tic_format(tic[0], 0).vk, 0);
        if (target >= 0 && g_rp.t[target].sw != nt.w)
            WARN_LIMIT(8, "nvn-live: stage %d stores to a scaled image %llx (%ux%u): only part of it is written\n",
                       stage, (unsigned long long)nt.gpu, nt.w, nt.h);
        if (si->img[i].kind != 1 && !!vk_tic_format(tic[0], 0).is_int != !!si->img[i].kind)
            WARN_LIMIT(4, "nvn-live: stage %d image %llx (type %d, format %d) has the other store kind\n", stage,
                       (unsigned long long)nt.gpu, si->img[i].type, (int)vk_tic_format(tic[0], 0).vk);
        if (target < 0 || !g_rp.t[target].storage) return *why = "image: no storage-capable replay image", 0;
        view = rp_view(target, si->img[i].kind == 1 ? VK_FORMAT_R32_UINT : g_rp.t[target].f, VK_IMAGE_VIEW_TYPE_2D,
                       VK_IMAGE_ASPECT_COLOR_BIT, NULL, 0);
        if (!view) return *why = "image: no view", 0;
        ii[SNX_VKB_IMG + i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = *set,
                                        .dstBinding = SNX_VKB_IMG + i,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                        .pImageInfo = &ii[SNX_VKB_IMG + i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, n, w, 0, NULL);
    return 1;
}
/* Live frames record into the renderer's command buffer: what a stage's textures need (an upload, a layout change)
   comes outside the host's pass, and before any set of the draw exists, as an upload may submit. Takes up the
   command buffer the renderer has after it */
static int rp_bound(int i) { /* drawn to now */
    u32 c;
    for (c = 0; c < g_rp.ncol && c < 8; c++)
        if (g_rp.col[c] == i) return 1;
    return g_rp.dep == i;
}
/* The game samples a target while it is the one drawn to (the chapter menu's medallions read the scene behind them).
   The console reads what is in memory; in Vulkan that is a feedback loop, which came out black. The draw samples a
   copy instead, taken outside the pass when something was drawn since the last one */
static void rp_feedback_copy(int i) {
    RpTarget* r = &g_rp.t[i];
    RpTarget* c;
    if (!r->fb) {
        NvnTarget t;
        int n;
        memset(&t, 0, sizeof t);
        t.gpu = r->gpu | 1ull << 63; /* no texture's address */
        t.w = r->w;
        t.h = r->h;
        t.layers = r->layers;
        n = rp_target_vk(&t, r->f, r->sw != r->w);
        r = &g_rp.t[i];
        if (n < 0) return;
        r->fb = n + 1;
        r->fb_at = ~0ull;
    }
    if (r->fb_at == g_rp.drawn) return;
    r->fb_at = g_rp.drawn;
    c = &g_rp.t[r->fb - 1];
    rp_end_rendering();
    rp_barrier();
    VkImageCopy ic = {.srcSubresource = {r->aspect, 0, 0, r->layers},
                      .dstSubresource = {r->aspect, 0, 0, r->layers},
                      .extent = {r->sw, r->sh, 1}};
    vkCmdCopyImage(g_rp.cb, r->img, VK_IMAGE_LAYOUT_GENERAL, c->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    rp_barrier();
}
static void rp_tex_ready(int mod, const NvnStageBind* b) {
    const ShaderInfo* si = &g_mod[mod].si;
    int nt = si->ntex > SH_MAXTEX ? SH_MAXTEX : si->ntex, i, slot;
    for (i = 0; i < nt; i++) {
        const ShTex* st = &si->tex[i];
        const char* why = NULL;
        ShTex plain;
        u32 handle;
        int num, target;
        u64 gva;
        Surf* t;
        if (st->flags & (SNX_TEX_INDEXED | 2)) continue;
        if (st->shadow && (si->tex_dc_color_mask >> i & 1)) {
            plain = *st;
            plain.shadow = 0;
            st = &plain;
        }
        num = st->flags & 1 ? 1 : st->shadow || !(si->tex_int_mask >> i & 1) ? 0 : (si->tex_sint_mask >> i & 1) ? 2 : 1;
        if (!tex_handle(b, st, &handle, &why) || !tex_get(b, handle, st, num, 0, &slot, &why)) continue;
        t = g_tc[slot].t;
        gva = ((u64)(g_tc[slot].tic[2] & 0xFFFF) << 32) | g_tc[slot].tic[1];
        target = rp_target_of(g_tc[slot].tic, gva);
        if (target >= 0 && g_rp.fs_src < 0 && si->stage == ST_FS) g_rp.fs_src = target;
        if (target >= 0 && rp_bound(target)) {
            g_rp.cb = vk_cmd();
            rp_feedback_copy(target);
        }
        if (g_rp.debug_frame && target < 0) { /* textures from guest memory: all zero means nothing wrote them */
            u64 cpu = vk_nvn_cpu_of(gva), j, n = 1u << 16, nz = 0;
            int inside = -1;
            u32 q;
            const u8* m = cpu ? GP(cpu) : NULL;
            for (j = 0; m && j < n && !nz; j++) nz = m[j];
            for (q = 0; q < (u32)g_rp.nt; q++)
                if (g_rp.t[q].img && gva > g_rp.t[q].gpu &&
                    gva - g_rp.t[q].gpu < (u64)g_rp.t[q].w * g_rp.t[q].h * 16 * g_rp.t[q].layers)
                    inside = (int)q;
            if (m && !nz)
                LOG("nvn-live: zero texture %llx, TIC %08x %ux%u levels-word %08x, stage tex %d, near image %d (GPU "
                    "%llx, %ux%u)\n",
                    (unsigned long long)gva, g_tc[slot].tic[0], (g_tc[slot].tic[4] & 0xFFFF) + 1,
                    (g_tc[slot].tic[5] & 0xFFFF) + 1, g_tc[slot].tic[7], i, inside,
                    inside >= 0 ? (unsigned long long)g_rp.t[inside].gpu : 0ull, inside >= 0 ? g_rp.t[inside].w : 0,
                    inside >= 0 ? g_rp.t[inside].h : 0);
        }
        if (target >= 0 || g_tc[slot].null_view || !t) continue;
        if (t->astc_pending || t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || (t->access & VK_RP_WRITES) ||
            (VK_SAMPLED_STAGES & ~t->stage))
            rp_end_rendering(); /* a layout change, or the ASTC batch's encodes and copies, come outside the pass */
        vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    g_rp.cb = vk_cmd();
}
static VkPrimitiveTopology rp_topology(u32 primitive, int tess) {
    static const VkPrimitiveTopology m[15] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                              VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                              VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                              VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                                              VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
                                              VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
                                              VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    return tess             ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
           : primitive < 15 ? m[primitive]
                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static i32 rp_edge(double v) { /* a scaled edge within what a render area can be */
    v = floor(v + 0.5);
    return v < -2147483647.0 ? -2147483647 : v > 2147483647.0 ? 2147483647 : (i32)v;
}
static VkRect2D rp_scissor(const NvnDrawDyn* y) { /* the scissor within the render area */
    i32 x0 = 0, y0 = 0, x1 = (i32)g_rp.rw, y1 = (i32)g_rp.rh;
    if (y->scissor[2] >= 0) {
        i32 sx0 = rp_edge((double)y->scissor[0] * g_rp.psx), sy0 = rp_edge((double)y->scissor[1] * g_rp.psy);
        i32 sx1 = rp_edge(((double)y->scissor[0] + y->scissor[2]) * g_rp.psx);
        i32 sy1 = rp_edge(((double)y->scissor[1] + y->scissor[3]) * g_rp.psy);
        if (sx0 > x0) x0 = sx0;
        if (sy0 > y0) y0 = sy0;
        if (sx1 < x1) x1 = sx1;
        if (sy1 < y1) y1 = sy1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
}
static void rp_dynamic(const NvnDrawDyn* y, const NvnPipeDesc* d, u32 primitive) {
    VkCommandBuffer cb = g_rp.cb;
    VkImageAspectFlags za = g_rp.dep >= 0 ? g_rp.t[g_rp.dep].aspect : 0;
    int depth = (za & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, stencil = (za & VK_IMAGE_ASPECT_STENCIL_BIT) != 0, f;
    float x = (float)y->viewport[0], top = (float)y->viewport[1], vw = (float)y->viewport[2],
          vh = (float)y->viewport[3];
    u32 ccw = y->front_ccw;
    VkViewport vp;
    VkRect2D sc = rp_scissor(y);
    if (vw <= 0 || vh <= 0)
        x = top = 0, vw = (float)g_rp.rw, vh = (float)g_rp.rh;
    else
        x *= g_rp.psx, vw *= g_rp.psx, top *= g_rp.psy, vh *= g_rp.psy;
    /* upper left: clip y +1 is the viewport's first row (B2recomp's nvn_vk_draw.c); lower left: its last, as OpenGL,
       which turns the winding the other way in the framebuffer */
    if (y->upper_left)
        vp = (VkViewport){x, top + vh, vw, -vh, clamp01(y->depth_range[0]), clamp01(y->depth_range[1])};
    else {
        vp = (VkViewport){x, top, vw, vh, clamp01(y->depth_range[0]), clamp01(y->depth_range[1])};
        ccw = !ccw;
    }
    if (rp_debug() & 4) sc = (VkRect2D){{0, 0}, {g_rp.rw, g_rp.rh}}; /* 4: no scissor */
    if (rp_debug() & 2) depth = 0;                                   /* 2: no depth test */
    {
        RpDyn n;
        int all = !g_rpd.valid;
        memset(&n, 0, sizeof n);
        n.vp = vp;
        n.sc = sc;
        n.b[0] = rp_debug() & 1 ? 0 : y->cull & 3; /* 1: no culling */
        n.b[1] = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        n.b[2] = (u32)rp_topology(primitive, d->mod[2] >= 0);
        n.b[3] = depth && y->depth_test;
        n.b[4] = depth && y->depth_write;
        n.b[5] = (rp_debug() & 32) && y->depth_func == 3 ? VK_COMPARE_OP_LESS_OR_EQUAL /* 32: EQUAL as LEQUAL */
                                                         : (VkCompareOp)((y->depth_func - 1) & 7); /* NVN NEVER 1 .. */
        n.b[6] = stencil && y->stencil_test;
        n.b[7] = (y->offset_enables & 4) != 0; /* NVN's fill offset */
        for (f = 0; f < 2; f++) {              /* NVN KEEP 1 .. DECR_WRAP 8 */
            n.s[f][0] = (y->stencil_op[f][0] - 1) & 7;
            n.s[f][1] = (y->stencil_op[f][2] - 1) & 7;
            n.s[f][2] = (y->stencil_op[f][1] - 1) & 7;
            n.s[f][3] = (y->stencil_func[f] - 1) & 7;
            n.s[f][4] = y->stencil_ref[f];
            n.s[f][5] = y->stencil_vmask[f];
            n.s[f][6] = y->stencil_wmask[f];
        }
        n.f[0] = y->offset[1];
        n.f[1] = g_vk.feat.depthBiasClamp ? y->offset[2] : 0.0f;
        n.f[2] = y->offset[0];
        n.f[3] = g_vk.feat.wideLines && y->line_width > 0 ? y->line_width * g_rp.psx : 1.0f;
        memcpy(&n.f[4], y->blend_color, 16);
#define DYN_NEW(field) (all || memcmp(&n.field, &g_rpd.d.field, sizeof n.field))
        if (DYN_NEW(vp)) vkCmdSetViewportWithCount(cb, 1, &n.vp);
        if (DYN_NEW(sc)) vkCmdSetScissorWithCount(cb, 1, &n.sc);
        if (DYN_NEW(b[0])) vkCmdSetCullMode(cb, n.b[0]);
        if (DYN_NEW(b[1])) vkCmdSetFrontFace(cb, (VkFrontFace)n.b[1]);
        if (DYN_NEW(b[2])) vkCmdSetPrimitiveTopology(cb, (VkPrimitiveTopology)n.b[2]);
        if (DYN_NEW(b[3])) vkCmdSetDepthTestEnable(cb, n.b[3]);
        if (DYN_NEW(b[4])) vkCmdSetDepthWriteEnable(cb, n.b[4]);
        if (DYN_NEW(b[5])) vkCmdSetDepthCompareOp(cb, (VkCompareOp)n.b[5]);
        if (DYN_NEW(b[6])) vkCmdSetStencilTestEnable(cb, n.b[6]);
        for (f = 0; f < 2; f++)
            if (DYN_NEW(s[f])) {
                VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
                vkCmdSetStencilOp(cb, face, (VkStencilOp)n.s[f][0], (VkStencilOp)n.s[f][1], (VkStencilOp)n.s[f][2],
                                  (VkCompareOp)n.s[f][3]);
                vkCmdSetStencilReference(cb, face, n.s[f][4]);
                vkCmdSetStencilCompareMask(cb, face, n.s[f][5]);
                vkCmdSetStencilWriteMask(cb, face, n.s[f][6]);
            }
        if (DYN_NEW(b[7])) vkCmdSetDepthBiasEnable(cb, n.b[7]);
        if (all || memcmp(n.f, g_rpd.d.f, 12)) vkCmdSetDepthBias(cb, n.f[0], n.f[1], n.f[2]);
        if (all || n.f[3] != g_rpd.d.f[3]) vkCmdSetLineWidth(cb, n.f[3]);
        if (all || memcmp(&n.f[4], &g_rpd.d.f[4], 16)) vkCmdSetBlendConstants(cb, y->blend_color);
#undef DYN_NEW
        if (all) { /* never change */
            vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
            vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
            vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
            vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
        }
        g_rpd.d = n;
        g_rpd.valid = 1;
    }
}
/* SNX_NVN_SUBMIT_EVERY=n (default 100, 0 off): with real writes, a list goes to the GPU every n draws and dispatches while
   the rest is still recorded, as vk_frame_check does for the renderer. Street: 28.9 to 39.3 FPS in matched runs
   (diagnostics/nvn-phase5-20261030). At the start of a draw, before anything of it is set up */
static void rp_chunk(void) {
    static int every = -1;
    u64 frame;
    if (every < 0) {
        const char* e = getenv("SNX_NVN_SUBMIT_EVERY");
        every = e ? atoi(e) : 100;
    }
    if (!g_rp.real || !every || (int)++g_rp.chunk < every) return;
    g_rp.chunk = 0;
    rp_end_rendering();
    rp_barrier();
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    frame = vk_frame_id();
    vk_frame_check(); /* a window of many submissions hands its pools and rings back here */
    if (frame != vk_frame_id()) vk_nvn_frame();
    g_rp.cb = vk_cmd();
    rp_barrier();
}
/* The game's final draw: the first of a frame into its window, covering it from a picture of the scene. It stays the
   game's, tone mapping and all, but goes to an image of the scene's size, and FSR 1 takes that to the window. What
   the game draws after it, the HUD, lands on the window at full size */
static int rp_final_draw(const NvnDrawDyn* y, const NvnDrawCall* call) {
    const RpTarget* w = g_rp.ncol && g_rp.col[0] >= 0 ? &g_rp.t[g_rp.col[0]] : NULL;
    const RpTarget* s = g_rp.fs_src >= 0 ? &g_rp.t[g_rp.fs_src] : NULL;
    return rp_fsr_on() && w && s && g_rp.dep < 0 && !g_rp.fsr_win && w->w == g_rp_win_w && w->h == g_rp_win_h &&
           !(w->gpu >> 62) && w->storage && w->f == VK_FORMAT_R8G8B8A8_UNORM && w->composed != g_rp.frame + 1 &&
           call->count <= 6 && s != w && s->w >= 640 && s->h >= 360 && s->w * 100 <= s->h * 180 &&
           s->w * 100 >= s->h * 170 && (u32)y->viewport[2] == w->w && (u32)y->viewport[3] == w->h;
}
static void rp_fsr_begin(void) { /* the draw's target becomes an image of the scene's size */
    const RpTarget* s = &g_rp.t[g_rp.fs_src];
    int win = g_rp.col[0], in;
    NvnTarget t;
    memset(&t, 0, sizeof t);
    t.gpu = 1ull << 62 | (u64)s->sw << 24 | s->sh;
    t.w = g_rp_win_w;
    t.h = g_rp_win_h;
    t.layers = 1;
    g_rp_want[0] = s->sw;
    g_rp_want[1] = s->sh;
    in = rp_target_vk(&t, g_rp.t[win].f, 2);
    if (in < 0) return;
    g_rp.fsr_win = win + 1;
    g_rp.col[0] = in;
    g_rp.dirty = 1;
}
static void rp_fsr_end(void) {
    int win = g_rp.fsr_win - 1, in = g_rp.col[0], tmp;
    NvnTarget t;
    memset(&t, 0, sizeof t);
    t.gpu = 3ull << 61; /* the upscaled picture before sharpening: the window's size, and in no pass */
    t.w = g_rp_win_w;
    t.h = g_rp_win_h;
    t.layers = 1;
    rp_end_rendering();
    tmp = rp_target_vk(&t, g_rp.t[win].f, 1);
    if (tmp >= 0) {
        RpTarget* w = &g_rp.t[win];
        VkImageView a = rp_view(in, w->f, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, NULL, 0),
                    b = rp_view(tmp, w->f, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, NULL, 0),
                    c = rp_view(win, w->f, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, NULL, 0);
        rp_barrier();
        vk_fsr1_run(g_rp.cb, a, g_rp.t[in].sw, g_rp.t[in].sh, b, c, w->sw, w->sh,
                    __atomic_load_n(&g_opt_fsr_sharpness, __ATOMIC_RELAXED));
        rp_barrier();
        w->composed = g_rp.frame + 1;
        g_rp.fsr_frames++;
        LOG_LIMIT(32, "nvn-live: FSR 1, %ux%u to %ux%u\n", g_rp.t[in].sw, g_rp.t[in].sh, w->sw, w->sh);
    }
    g_rp.col[0] = win;
    g_rp.fsr_win = 0;
    g_rp.dirty = 1;
}
void vk_nvn_replay_draw(const NvnPipeDesc* d, const NvnStageBind* bind, const NvnDrawDyn* y, const NvnDrawCall* call) {
    static const int st_of[5] = {ST_VS, ST_TCS, ST_TES, ST_GS, ST_FS};
    VkDescriptorSet set[5] = {0};
    VkBuffer vb[17], ib = VK_NULL_HANDLE, xb = VK_NULL_HANDLE, pb = VK_NULL_HANDLE;
    VkDeviceSize vo[17], vs[17];
    u64 io = 0, xo = 0, po = 0;
    const char* why = NULL;
    float pc[8] = {0};
    int made, s, dl;
    u32 b;
    NvnPipe* p;
    if (!g_rp.on) return;
    rp_fsr_restore();
    g_rp.draws++;
    g_rp.fr_draws++;
    rp_chunk();
    p = pipe_get(d, &made);
    if (p && __atomic_load_n(&p->pending, __ATOMIC_ACQUIRE)) {
        u64 t0 = plat_ns(), waited;
        u32 ahead = pipe_front(p);
        /* what a draw stores can't wait for a later frame: the game reads it. Nor can a full-screen pass, whose
           absence changes a whole frame's brightness where a mesh's is one thing missing */
        int writes = call->count <= 6;
        for (s = 0; s < 5; s++)
            if (d->mod[s] >= 0 && (g_mod[d->mod[s]].si.gbuf_wmask || g_mod[d->mod[s]].si.nimg)) writes = 1;
        while (__atomic_load_n(&p->pending, __ATOMIC_ACQUIRE) &&
               (writes || g_rp.pipe_wait + (plat_ns() - t0) < pipe_budget_ns()))
            plat_sleep_ns(0);
        waited = plat_ns() - t0;
        g_rp.pipe_wait += waited;
        g_pw_n++;
        g_pw_ns += waited;
        if (waited > g_pw_max) g_pw_max = waited;
        if (waited > 33000000ull)
            LOG_LIMIT(200, "nvn-live: a draw of %u waited %.1f ms for its pipeline, %u ahead of it\n", call->count,
                      (double)waited / 1e6, ahead);
        if (__atomic_load_n(&p->pending, __ATOMIC_ACQUIRE)) {
            int c0 = g_rp.ncol ? g_rp.col[0] : -1;
            u64 area = c0 >= 0 ? (u64)g_rp.t[c0].w * g_rp.t[c0].h : 0;
            g_rp.deferred++;
            g_rp.fr_deferred++;
            if (area > g_rp.fr_deferred_area) g_rp.fr_deferred_area = area;
            return;
        }
    }
    if (!p || !p->p) RSKIP("no pipeline");
    if (call->primitive == 7) RSKIP("quads");
    if (call->kind >= 1 && call->index_type == 0) RSKIP("8-bit indices");
    if (call->kind == 3 && !g_vk.has_indirect_count) RSKIP("indirect count unavailable");
    g_rp.fs_src = -1;
    if (g_rp.live)
        for (s = 0; s < 5; s++)
            if (d->mod[s] >= 0) rp_tex_ready(d->mod[s], &bind[s]);
    if (rp_final_draw(y, call)) rp_fsr_begin();
    for (s = 0; s < 5; s++)
        if (d->mod[s] >= 0 && !rp_stage_set(d->mod[s], &bind[s], st_of[s], &set[s], &why)) RSKIP(why);
    if (g_pt.open && d->mod[4] >= 0) g_pt.mod = d->mod[4];
    for (b = 0; b < p->nbind; b++) {
        u32 st = p->bstream[b];
        u64 off;
        int n = st < 16 && y->vbuf[st].addr ? import_of(y->vbuf[st].addr, y->vbuf[st].size ? y->vbuf[st].size : 1, &off)
                                            : -1;
        if (n >= 0) {
            vb[b] = rp_read_buf(n, off, y->vbuf[st].size);
            vo[b] = off;
            vs[b] = y->stride[st];
        } else { /* the constant (0, 0, 0, 1) binding, and streams not bound: 0, 0, 0, 1 floats then integers */
            static const u32 c[8] = {0, 0, 0, 0x3f800000u, 0, 0, 0, 1};
            u8* q = rp_alloc(32, &off);
            if (st < 16 && y->vbuf[st].addr)
                LOG_LIMIT(32, "nvn-live: vertex stream %u at %llx, %llu bytes, outside the pools (draw %llu)\n", st,
                          (unsigned long long)y->vbuf[st].addr, (unsigned long long)y->vbuf[st].size,
                          (unsigned long long)g_rp.draws);
            else if (g_rp.debug_frame)
                LOG("nvn-live: draw %llu binding %u stream %u unbound (of %u, attributes %u)\n",
                    (unsigned long long)g_rp.draws, b, st, p->nbind, d->nattr);
            if (!q) RSKIP("staging used up");
            memcpy(q, c, 32);
            vb[b] = g_rp.sbuf;
            vo[b] = off;
            vs[b] = 0;
        }
    }
    if (call->kind >= 1) {
        int n = import_of(call->index, 2, &io);
        if (n < 0) RSKIP("index buffer outside the pools");
        ib =
            call->kind == 1 ? rp_read_buf(n, io, (u64)call->count << (call->index_type == 1 ? 1 : 2)) : g_import[n].buf;
    }
    if (call->kind >= 2) {
        /* arguments and count from the replay's copies when this frame's compute wrote them (GPU culling) */
        u64 span =
            call->kind == 3 ? (u64)(call->max_draws ? call->max_draws : 1) * (call->stride ? call->stride : 20) : 20;
        VkDescriptorBufferInfo cp;
        int n;
        if (rp_ssbo(call->indirect, span, 0, &cp))
            xb = cp.buffer, xo = cp.offset;
        else if ((n = import_of(call->indirect, 20, &xo)) >= 0) {
            u64 whole; /* c7: from the copy when a dispatch wrote the arguments there; span when it fits the pool */
            xb = rp_read_buf(n, xo, import_of(call->indirect, span, &whole) == n ? span : 20);
        } else
            RSKIP("indirect buffer outside the pools");
        if (call->kind == 3) {
            if (rp_ssbo(call->parameter, 4, 0, &cp))
                pb = cp.buffer, po = cp.offset;
            else if ((n = import_of(call->parameter, 4, &po)) >= 0)
                pb = rp_read_buf(n, po, 4);
            else
                RSKIP("draw count outside the pools");
        }
    }
    rp_begin();
    if (g_rpd.pipe != p->p) vkCmdBindPipeline(g_rp.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    for (s = 0; s < 5; s++)
        if (set[s])
            vkCmdBindDescriptorSets(g_rp.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk_playout,
                                    (u32)shader_vk_set(st_of[s]), 1, &set[s], 0, NULL);
    if (p->nbind) vkCmdBindVertexBuffers2(g_rp.cb, 0, p->nbind, vb, vo, NULL, vs);
    if (ib && (g_rpd.ib != ib || g_rpd.io != io || g_rpd.it != call->index_type)) {
        vkCmdBindIndexBuffer(g_rp.cb, ib, io, call->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        g_rpd.ib = ib;
        g_rpd.io = io;
        g_rpd.it = call->index_type;
    }
    rp_dynamic(y, d, call->primitive);
    /* the translator's push constants: GL depth (near is minus w) for the vertex stages, the alpha test */
    /* pc[0]: the base instance's bits, which the translator takes off InstanceIndex for Maxwell's instance id (as the
       renderer passes register 0x50E). An indirect draw's comes from its arguments on the GPU: 0 here */
    if (call->kind == 1) memcpy(&pc[0], &call->base_instance, 4);
    pc[1] = y->depth_zero ? 0.0f : 1.0f;
    pc[4] = y->alpha_func >= 1 && y->alpha_func < 8 && !(rp_debug() & 8) ? 1.0f : 0.0f; /* 8: no alpha test */
    pc[5] = y->alpha_func >= 1 && y->alpha_func <= 8 ? (float)(y->alpha_func - 1) : 7.0f;
    pc[6] = y->alpha_ref;
    if (g_rpd.pipe != p->p || memcmp(pc, g_rpd.pc, sizeof pc)) {
        vkCmdPushConstants(g_rp.cb, g_vk_playout, VK_PUSH_STAGES, 0, 32, pc);
        memcpy(g_rpd.pc, pc, sizeof pc);
    }
    g_rpd.pipe = p->p;
    dl = dl_row(d, bind, y, call);
    if (dl >= 0) vkCmdBeginQuery(g_rp.cb, g_dl.q, (u32)dl, VK_QUERY_CONTROL_PRECISE_BIT);
    if (call->kind == 0)
        vkCmdDraw(g_rp.cb, call->count, 1, call->first, 0);
    else if (call->kind == 1)
        vkCmdDrawIndexed(g_rp.cb, call->count, call->instances ? call->instances : 1, 0, call->base_vertex,
                         call->base_instance);
    else if (call->kind == 2)
        vkCmdDrawIndexedIndirect(g_rp.cb, xb, xo, 1, 20);
    else
        vkCmdDrawIndexedIndirectCount(g_rp.cb, xb, xo, pb, po, call->max_draws, call->stride ? call->stride : 20);
    if (dl >= 0) vkCmdEndQuery(g_rp.cb, g_dl.q, (u32)dl);
    g_rp.drawn++;
    g_rp.kinds[call->kind & 3]++;
    if (g_rp.fsr_win) rp_fsr_end();
}
void vk_nvn_replay_dispatch(int mod, const NvnStageBind* b, u32 x, u32 y, u32 z, u64 indirect) {
    VkDescriptorSet set;
    VkDescriptorBufferInfo ib;
    const char* why = NULL;
    u64 off = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    if (!g_rp.on) return;
    g_rp.dispatches++;
    rp_chunk();
    if (g_rp.debug_frame)
        LOG("nvn-live: dispatch at draw %llu: module %d, %u %u %u, indirect %llx, %.2f ms into the window\n",
            (unsigned long long)g_rp.draws, mod, x, y, z, (unsigned long long)indirect,
            (double)(plat_ns() - g_rp.win_start) / 1e6);
    if (mod < 0 || mod >= g_nmod || !g_cpipe[mod]) RSKIP("compute: no pipeline");
    if (rp_debug() & 64) RSKIP("compute (SNX_NVN_REPLAY_DEBUG 64)");
    if (g_rp.live) rp_tex_ready(mod, b);
    /* storage images go to the replay's images (rp_stage_set) */
    if (!rp_stage_set(mod, b, ST_CS, &set, &why)) RSKIP(why);
    if (indirect) { /* its arguments, from a copy when the replay wrote them */
        int n;
        if (rp_ssbo(indirect, 12, 0, &ib))
            buf = ib.buffer, off = ib.offset;
        else if ((n = import_of(indirect, 12, &off)) >= 0)
            buf = rp_read_buf(n, off, 12); /* the copy when a dispatch wrote them there (c7) */
        else
            RSKIP("indirect dispatch outside the pools");
    }
    rp_end_rendering();
    rp_barrier();
    vkCmdBindPipeline(g_rp.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_cpipe[mod]);
    vkCmdBindDescriptorSets(g_rp.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_playout, (u32)shader_vk_set(ST_CS), 1, &set,
                            0, NULL);
    if (buf)
        vkCmdDispatchIndirect(g_rp.cb, buf, off);
    else
        vkCmdDispatch(g_rp.cb, x, y, z);
    if (g_pt.open) {
        int k;
        g_pt.mod = mod;
        for (k = 0; k < 16; k++) g_pt.bound += b->ssbo[k].size;
    }
    pt_mark(PT_COMPUTE, x, y, 0);
    rp_barrier();
    g_rp.dispatched++;
}
/* CopyBufferToBuffer (src not 0) and ClearBuffer (src 0, value): into the replay's copy of the destination */
void vk_nvn_replay_buffer(u64 src, u64 dst, u64 size, u32 value) {
    VkDescriptorBufferInfo d, s;
    u64 off;
    int n;
    if (!g_rp.on) return;
    if (!size || (!src && ((dst | size) & 3))) RSKIP("buffer clear not 4-byte aligned");
    s.buffer = VK_NULL_HANDLE;
    if (g_rp.real) {
        u64 soff;
        int sn = src ? import_of(src, size, &soff) : -1;
        if ((n = import_of(dst, size, &off)) < 0) RSKIP("buffer copy or clear outside the pools");
        if (src && sn < 0) RSKIP("buffer copy source outside the pools");
        if (src) s = (VkDescriptorBufferInfo){rp_read_buf(sn, soff, size), soff, size}; /* the copy when it holds it */
        /* c7: into the video memory copy when the GPU owns part of the range there, so it stays the GPU's */
        if (own_touches(n, off, size) && mir_own(n, off, size)) {
            d = (VkDescriptorBufferInfo){g_mir[n].buf, off, size};
            dl_written(dst, size);
        } else {
            g_land_why = 7;
            own_land_range(n, off, size);
            d = (VkDescriptorBufferInfo){g_import[n].buf, off, size};
            rp_stamp(dst, size);
        }
    } else if (!rp_ssbo(dst, size, 1, &d) || !d.buffer)
        RSKIP("buffer copy or clear: no copy of the destination");
    rp_end_rendering();
    rp_barrier();
    if (!src)
        vkCmdFillBuffer(g_rp.cb, d.buffer, d.offset, size, value);
    else {
        if (!s.buffer && !rp_ssbo(src, size, 0, &s)) {
            if ((n = import_of(src, size, &off)) < 0) RSKIP("buffer copy source outside the pools");
            s = (VkDescriptorBufferInfo){g_import[n].buf, off, size};
        }
        VkBufferCopy r = {s.offset, d.offset, size};
        vkCmdCopyBuffer(g_rp.cb, s.buffer, d.buffer, 1, &r);
    }
    rp_barrier();
}
/* The hardware writes these in command order, so a draw recorded before sees the old bytes and one after sees the new:
   a copy from staging between them, into the pool the constant buffers are bound from */
void vk_nvn_replay_update(u64 dst, const void* data, u32 size) {
    VkBufferCopy r;
    u64 off, from;
    int n;
    u8* p;
    if (!g_rp.on || !g_rp.real) return;
    if ((n = import_of(dst, size, &off)) < 0) RSKIP("uniform buffer update outside the pools");
    g_land_why = 8;
    own_land_range(n, off, size); /* written in the pool: the copy's blocks of it land first */
    if (!(p = rp_alloc(size, &from))) RSKIP("staging used up");
    memcpy(p, data, size);
    rp_stamp(dst, size);
    dl_update(dst, data, size);
    rp_end_rendering();
    rp_barrier();
    r = (VkBufferCopy){from, off, size};
    vkCmdCopyBuffer(g_rp.cb, g_rp.sbuf, g_import[n].buf, 1, &r);
    rp_barrier();
}
void vk_nvn_replay_clear_color(u32 index, const float* rgba, u32 mask, const NvnDrawDyn* y) {
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = index};
    VkClearRect r;
    if (!g_rp.on) return;
    rp_fsr_restore();
    g_rp.clears++;
    if (index >= g_rp.ncol || g_rp.col[index] < 0) RSKIP("colour clear of no target");
    if ((mask & 15) != 15) RSKIP("colour clear of some channels");
    rp_begin();
    memcpy(a.clearValue.color.float32, rgba, 16);
    r = (VkClearRect){rp_scissor(y), 0, 1};
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(g_rp.cb, 1, &a, 1, &r);
    g_rp.cleared++;
}
void vk_nvn_replay_clear_depth(float depth, int depth_mask, u32 stencil, u32 stencil_mask, const NvnDrawDyn* y) {
    VkImageAspectFlags za = g_rp.dep >= 0 ? g_rp.t[g_rp.dep].aspect : 0;
    VkClearAttachment a = {.aspectMask = (depth_mask ? za & VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                         (stencil_mask ? za & VK_IMAGE_ASPECT_STENCIL_BIT : 0)};
    VkClearRect r;
    if (!g_rp.on) return;
    g_rp.clears++;
    if (!a.aspectMask) RSKIP("depth clear of no target");
    rp_begin();
    a.clearValue.depthStencil = (VkClearDepthStencilValue){clamp01(depth), stencil};
    r = (VkClearRect){rp_scissor(y), 0, 1};
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(g_rp.cb, 1, &a, 1, &r);
    g_rp.cleared++;
}
/* the colour targets blitted to RGBA8 (sRGB when the target is, so the bytes stay as the display shows them; float
   targets clamp to 0..1) and copied to host memory, to be saved once the replay has run */
static struct {
    VkImage img;
    VmaAllocation img_mem, buf_mem;
    VkBuffer buf;
    VmaAllocationInfo info;
    int srgb;
} g_rb[RP_TARGETS];
static void rp_readback(int i) {
    RpTarget* t = &g_rp.t[i];
    int hdr = t->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || t->f == VK_FORMAT_R16G16B16A16_SFLOAT ||
              t->f == VK_FORMAT_R16G16_SFLOAT || t->f == VK_FORMAT_R16_SFLOAT || t->f == VK_FORMAT_R32_SFLOAT;
    VkFormat rf = hdr                                                          ? VK_FORMAT_R16G16B16A16_SFLOAT
                  : vk_srgb_as(t->f, 1) == t->f && vk_srgb_as(t->f, 0) != t->f ? VK_FORMAT_R8G8B8A8_SRGB
                                                                               : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = rf,
                            .extent = {t->sw, t->sh, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = (u64)t->sw * t->sh * 8,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo bai = {.flags =
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .srcOffsets = {{0, 0, 0}, {(i32)t->sw, (i32)t->sh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstOffsets = {{0, 0, 0}, {(i32)t->sw, (i32)t->sh, 1}}};
    VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {t->sw, t->sh, 1}};
    memset(&g_rb[i], 0, sizeof g_rb[i]);
    if (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { /* depth: its 4-byte texels copied as they are (rp_save greys them) */
        VkBufferImageCopy dc = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                .imageExtent = {t->sw, t->sh, 1}};
        if (t->f == VK_FORMAT_D16_UNORM ||
            vmaCreateBuffer(g_vk.vma, &bci, &bai, &g_rb[i].buf, &g_rb[i].buf_mem, &g_rb[i].info) != VK_SUCCESS)
            return;
        g_rb[i].srgb = -1;
        vkCmdCopyImageToBuffer(g_rp.cb, t->img, VK_IMAGE_LAYOUT_GENERAL, g_rb[i].buf, 1, &dc);
        return;
    }
    if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !fmt_has(t->f, 0, VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return;
    if (vmaCreateImage(g_vk.vma, &ii, &iai, &g_rb[i].img, &g_rb[i].img_mem, NULL) != VK_SUCCESS) return;
    if (vmaCreateBuffer(g_vk.vma, &bci, &bai, &g_rb[i].buf, &g_rb[i].buf_mem, &g_rb[i].info) != VK_SUCCESS) {
        vmaDestroyImage(g_vk.vma, g_rb[i].img, g_rb[i].img_mem);
        g_rb[i].img = VK_NULL_HANDLE;
        return;
    }
    g_rb[i].srgb = hdr ? 2 : rf == VK_FORMAT_R8G8B8A8_SRGB;
    vk_barrier(g_rp.cb, g_rb[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdBlitImage(g_rp.cb, t->img, VK_IMAGE_LAYOUT_GENERAL, g_rb[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_NEAREST);
    vk_barrier(g_rp.cb, g_rb[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(g_rp.cb, g_rb[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rb[i].buf, 1, &cp);
}
static u64 g_hdr[3]; /* the last float target saved: NaN, infinite and non-zero components */
static float g_hdr_max;
static int rp_save(int i, const char* path) {
    const RpTarget* t = &g_rp.t[i];
    const u8* px = g_rb[i].info.pMappedData;
    u64 n = (u64)t->sw * t->sh, j;
    FILE* f;
    if (!g_rb[i].buf) return 0;
    g_hdr[0] = g_hdr[1] = g_hdr[2] = 0;
    g_hdr_max = 0;
    vmaInvalidateAllocation(g_vk.vma, g_rb[i].buf_mem, 0, VK_WHOLE_SIZE);
    f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%u %u\n255\n", t->sw, t->sh);
    for (j = 0; j < n; j++) {
        if (g_rb[i].srgb == -1) { /* depth, 0..1 as grey: 24 bits of X8_D24 / D24S8, or a 32-bit float */
            u32 v;
            float d;
            u8 g[3];
            memcpy(&v, px + j * 4, 4);
            if (t->f == VK_FORMAT_D32_SFLOAT || t->f == VK_FORMAT_D32_SFLOAT_S8_UINT)
                memcpy(&d, &v, 4);
            else
                d = (float)(v & 0xFFFFFF) / 16777215.0f;
            g[0] = g[1] = g[2] = (u8)(clamp01(d) * 255.0f);
            fwrite(g, 1, 3, f);
        } else if (g_rb[i].srgb == 2) { /* half floats: counted, and tone mapped x / (1 + x) for the picture */
            u16 hv[3];
            u8 g[3];
            int ch;
            memcpy(hv, px + j * 8, 6);
            for (ch = 0; ch < 3; ch++) {
                u32 e = (hv[ch] >> 10) & 31, m = hv[ch] & 1023;
                float v = e == 0    ? (float)m / 16777216.0f
                          : e == 31 ? 0.0f
                                    : (float)(1024 + m) * (float)(1u << e) / 33554432.0f;
                if (e == 31) g_hdr[m ? 0 : 1]++;
                if (hv[ch] & 0x8000) v = -v;
                if (v != 0.0f) g_hdr[2]++;
                if (v > g_hdr_max) g_hdr_max = v;
                g[ch] = (u8)(v <= 0 ? 0 : 255.0f * v / (1.0f + v));
            }
            fwrite(g, 1, 3, f);
        } else
            fwrite(px + j * 4, 1, 3, f);
    }
    fclose(f);
    return 1;
}
/* ------------------------------------------------------------ live frames (phase 4 step 4.1) */
int vk_nvn_live_start(void) {
    int i;
    if (g_rp.on) return 0;
    memset(&g_rp, 0, sizeof g_rp);
    g_nrpw = g_rpw_full = 0;
    g_rp.dep = -1;
    for (i = 0; i < 8; i++) g_rp.col[i] = -1;
    g_rp.live = 1;
    g_rp.on = 1;
    g_rp.psx = g_rp.psy = 1.0f;
    LOG("nvn-live: host frames in the renderer's command buffer\n");
    if (rp_scaling())
        LOG("nvn-live: window %.2fx, scene %.2fx%s\n", (double)g_vk_scale, (double)rp_scene_scale(),
            rp_fsr_on() ? ", FSR 1" : "");
    if (g_opt_fsr > 1.0f && !vk_fsr1_ready()) WARN("nvn-live: this build has no FSR 1 shaders: fsr stays off\n");
    return 1;
}
/* The window buffers a present named: their host images as Surfs, so the renderer's show takes them as it takes a
   render target. show leaves one in its own layout: the next window brings it back to GENERAL */
#define RP_SHOW 8
static struct {
    u64 cpu, gpu;
    Surf s;
} g_rps[RP_SHOW];
static int g_rps_on;
void vk_nvn_live_show(void) { g_rps_on = 1; }
Surf* vk_nvn_live_surf(u64 cpu_addr) {
    int k, i;
    if (!g_rps_on || !cpu_addr) return NULL;
    for (k = 0; k < RP_SHOW && g_rps[k].cpu != cpu_addr; k++) {}
    LOG_LIMIT(6, "nvn-live: a present of buffer %llx: %s\n", (unsigned long long)cpu_addr,
              k == RP_SHOW ? "the host names no such window texture" : "the host's image");
    if (k == RP_SHOW) return NULL;
    for (i = 0; i < g_rp.nt; i++) {
        const RpTarget* t = &g_rp.t[i];
        Surf* s = &g_rps[k].s;
        if (t->gpu != g_rps[k].gpu || !t->img) continue;
        if (s->img != t->img) {
            memset(s, 0, sizeof *s);
            s->img = t->img;
            s->layout = VK_IMAGE_LAYOUT_GENERAL;
            s->stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            s->access = VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
        s->w = t->w;
        s->sw = t->sw;
        s->h = t->h;
        s->sh = t->sh;
        s->layers = s->levels = 1;
        s->f.vk = t->f;
        s->gva = t->gpu;
        return s;
    }
    return NULL;
}
/* GPU time of the host's windows, by timestamps: a pair for each, read back when done */
#define RP_TS 4096u
static VkQueryPool g_ts_pool;
static u32 g_ts_at, g_ts_read; /* pairs written and read, counters */
static u64 g_ts_ns, g_ts_windows;
static void rp_ts_write(int end) {
    if (!g_ts_pool) {
        VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = RP_TS * 2};
        if (!VK_OK(vkCreateQueryPool(g_vk.dev, &ci, NULL, &g_ts_pool))) return;
    }
    if (!end) {
        if (g_ts_at - g_ts_read >= RP_TS - 1) return; /* behind: this window isn't counted */
        vkCmdResetQueryPool(g_rp.cb, g_ts_pool, (g_ts_at % RP_TS) * 2, 2);
        vkCmdWriteTimestamp2(g_rp.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_ts_pool, (g_ts_at % RP_TS) * 2);
    } else {
        vkCmdWriteTimestamp2(g_rp.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_ts_pool, (g_ts_at % RP_TS) * 2 + 1);
        g_ts_at++;
    }
}
static void rp_ts_read(void) {
    while (g_ts_read != g_ts_at) {
        u64 v[2];
        if (vkGetQueryPoolResults(g_vk.dev, g_ts_pool, (g_ts_read % RP_TS) * 2, 2, sizeof v, v, 8,
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        g_ts_ns += (u64)((double)(v[1] - v[0]) * (double)g_vk.props.limits.timestampPeriod);
        g_ts_windows++;
        g_ts_read++;
    }
}
void vk_nvn_live_window(int open) {
    if (open) {
        int k;
        u64 t0 = plat_ns();
        vk_lock();
        g_rp.win_start = plat_ns();
        g_rp.fr_lock += g_rp.win_start - t0;
        if (!g_rp.fr_first) {
            g_rp.fr_first = g_rp.win_start;
            g_rp.fr_gpu_busy = !vk_submit_done(vk_last_submit());
        }
        vk_render_end();
        rp_stamps_done();
        vk_nvn_frame(); /* the renderer's textures may have changed since the last list: looked up again */
        g_rp.cb = vk_cmd();
        g_rp_win = 1;
        for (k = 0; k < RP_SHOW; k++)
            if (g_rps[k].s.img && g_rps[k].s.layout != VK_IMAGE_LAYOUT_GENERAL)
                vk_surf_use(g_rp.cb, &g_rps[k].s, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        rp_barrier();
        pt_window();
        if (g_rp.real) rp_mirror_window();
        pt_mark(PT_COPIES, 0, 0, 0);
        dl_window();
        rp_ts_write(0);
    } else {
        g_rp_win = 0;
        rp_end_rendering();
        rp_barrier();
        pt_mark(PT_BETWEEN, 0, 0, 0);
        rp_ts_write(1);
        if (g_rp.real) { /* the guest may read what it wrote once its release is performed */
            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                   .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT};
            VkDependencyInfo di = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
            vkCmdPipelineBarrier2(g_rp.cb, &di);
            vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
            vk_release_host_gate(vk_last_submit());
            if (!g_nstamp_sent) {
                g_nstamp_sent = g_nstamp;
                g_stamp_gate = vk_last_submit();
            }
        }
        g_rp.win_ns += plat_ns() - g_rp.win_start;
        g_rp.fr_win += plat_ns() - g_rp.win_start;
        g_rp.fr_last = plat_ns();
        vk_unlock();
    }
}
void vk_nvn_live_real(void) {
    const char* e = getenv("SNX_NVN_TEX_BUDGET");
    g_rp.real = g_rp_real = 1;
    /* off by default: a texture put off draws black, and with the pools in video memory decoding all of a load is
       about 90 ms a 350 Mpx frame, where the budget spread it over a spell of slow frames of null textures */
    g_vk_tex_budget = (u64)(e ? atoi(e) : 0) * 1000000;
}
void vk_nvn_live_fence(void) {
    if (g_rp.debug_frame)
        LOG("nvn-live: command buffer fence at draw %llu, %.2f ms into the window\n", (unsigned long long)g_rp.draws,
            (double)(plat_ns() - g_rp.win_start) / 1e6);
}
void vk_nvn_live_debug(int on) { g_rp.debug_frame = on; }
/* SNX_NVN_LIVE_DUMP in dual mode: what the host's compute left in its copies against what the renderer's left in guest
   memory, two presents later */
static struct {
    u64 gpu, size;
    u8* p;
    u8* host;
} g_chk[RP_WRITTEN];
static u32 g_nchk, g_chk_wait;
static void rp_chk(void) {
    u32 k;
    u64 j;
    for (k = 0; k < g_nchk; k++) {
        u64 at = vk_nvn_cpu_of(g_chk[k].gpu), differ = 0, hz = 0, gz = 0, first = ~0ull;
        const u8* g = at ? GP(at) : NULL;
        if (!g || !g_chk[k].host) continue;
        for (j = 0; j < g_chk[k].size; j++) {
            hz += g_chk[k].host[j] != 0;
            gz += g[j] != 0;
            if (g_chk[k].host[j] != g[j]) {
                differ++;
                if (first == ~0ull) first = j;
            }
        }
        LOG("nvn-live: written range %llx, %llu bytes: host non-zero %llu, guest non-zero %llu, differing %llu, first "
            "at %llx\n",
            (unsigned long long)g_chk[k].gpu, (unsigned long long)g_chk[k].size, (unsigned long long)hz,
            (unsigned long long)gz, (unsigned long long)differ, (unsigned long long)first);
        free(g_chk[k].host);
    }
    g_nchk = 0;
}
void vk_nvn_live_present(u64 gpu, u64 cpu, const char* out, int all) {
    int i, shown = -1;
    u64 gate = 0;
    u32 nkeep = 0;
    if (!g_rp.live) return;
    dl_flush();
    g_rp.frame++;
    {
        u64 now = plat_ns(), d = now - g_rp.last_present;
        static u64 slow_ns; /* SNX_NVN_SLOW_MS: frames past it get their breakdown line, 50 ms when unset */
        int at60;
        if (!slow_ns)
            slow_ns = (getenv("SNX_NVN_SLOW_MS") ? (u64)atoi(getenv("SNX_NVN_SLOW_MS")) : 50) * 1000000ull + 1;
        if (g_rp.last_present && d > g_rp.worst_ns) g_rp.worst_ns = d;
        g_rp.slow_frames += g_rp.last_present && d > 50000000;
        /* the 1% low is play's: frames the game itself asked to come at 30 FPS or less (swap interval 2 and more:
           loads, in-engine scenes) are 33 ms by design, and counting them made a session with more of those read as
           stutter */
        at60 = __atomic_load_n(&g_pace_interval, __ATOMIC_RELAXED) <= 1;
        if (g_rp.last_present && at60) g_ft[d / 1000000 < 255 ? d / 1000000 : 255]++;
        {
            /* the pacer's hold this frame (present.c, on the game's thread in DequeueBuffer): part of the time before
               the first window, which is otherwise the game's own work */
            static u64 slept_before;
            u64 slept = __atomic_load_n(&g_pace_slept_ns, __ATOMIC_RELAXED), held = slept - slept_before;
            slept_before = slept;
            if (g_rp.last_present && d >= slow_ns)
                LOG_LIMIT(20000,
                          "nvn-live: frame %llu took %.1f ms: windows %.1f, of which texture lookups %.1f (%u) and "
                          "pipeline waits %.1f; vk_lock waits %.1f; draws %u; ASTC %.1f Mpx; before the first "
                          "window %.1f, after the last %.1f, GPU busy at its start %d; held by the pacer %.1f\n",
                          (unsigned long long)g_rp.frame, (double)d / 1e6, (double)g_rp.fr_win / 1e6,
                          (double)g_fr_tex / 1e6, g_fr_miss, (double)g_rp.pipe_wait / 1e6, (double)g_rp.fr_lock / 1e6,
                          g_rp.fr_draws, (double)(g_vk_astc_px - g_fr_astc) / 1e6,
                          g_rp.fr_first ? (double)(g_rp.fr_first - g_rp.last_present) / 1e6 : 0.0,
                          g_rp.fr_last ? (double)(now - g_rp.fr_last) / 1e6 : 0.0, g_rp.fr_gpu_busy,
                          (double)held / 1e6);
        }
        {
            static u64 spike_ns; /* SNX_NVN_SPIKE_MS: the time before the first window that gets profiled, 150 ms */
            if (!spike_ns)
                spike_ns = (getenv("SNX_NVN_SPIKE_MS") ? (u64)atoi(getenv("SNX_NVN_SPIKE_MS")) : 150) * 1000000ull + 1;
            if (g_rp.last_present && g_rp.fr_first && g_rp.fr_first - g_rp.last_present >= spike_ns)
                plat_prof_spike(g_rp.last_present, g_rp.fr_first, g_rp.frame); /* what ran before the first list */
        }
        /* what a frame's picture lacked: a dark frame in play is found by its time here */
        if (g_rp.fr_deferred || g_fr_null)
            LOG_LIMIT(20000,
                      "nvn-live: frame %llu lacks: %u draws for a pipeline (largest target %llu px), %llu textures "
                      "put off (largest %llu px)\n",
                      (unsigned long long)g_rp.frame, g_rp.fr_deferred, (unsigned long long)g_rp.fr_deferred_area,
                      (unsigned long long)g_fr_null, (unsigned long long)g_fr_null_area);
        g_rp.fr_deferred = 0;
        g_rp.fr_deferred_area = g_fr_null = g_fr_null_area = 0;
        if (g_rp.last_present) {
            g_rp.p_frames++;
            g_rp.p_over += at60 && d > 33400000;
            g_rp.p_at60 += at60;
            g_rp.p_at30 += !at60;
            g_rp.p_win += g_rp.fr_win;
            g_rp.p_lock += g_rp.fr_lock;
            g_rp.p_before += g_rp.fr_first ? g_rp.fr_first - g_rp.last_present : 0;
            g_rp.p_after += g_rp.fr_last ? now - g_rp.fr_last : 0;
            g_rp.p_busy += (u64)g_rp.fr_gpu_busy;
        }
        g_fr_astc = g_vk_astc_px;
        g_rp.fr_first = g_rp.fr_last = 0;
        g_rp.fr_win = g_fr_tex = g_rp.fr_lock = 0;
        prep_frame(g_rp.fr_draws);
        g_rp.fr_draws = g_fr_miss = 0;
        g_rp.last_present = now;
    }
    g_rp.pipe_wait = 0;
    if (g_nchk && ++g_chk_wait == 2) rp_chk();
    vk_lock();
    if (g_ts_pool) rp_ts_read();
    pt_read();
    if (g_rp.real && g_rp.frame % 300 == 0) rp_mirror_check();
    if (all)
        for (nkeep = 0; nkeep < g_nrpw; nkeep++) {
            g_chk[nkeep].gpu = g_rpw[nkeep].gpu;
            g_chk[nkeep].size = g_rpw[nkeep].size;
            g_chk[nkeep].p = g_rpw[nkeep].p;
        }
    g_nrpw = g_rpw_full = 0; /* the next frame's writes start from guest memory */
    LOG_LIMIT(6, "nvn-live: present of GPU %llx at CPU %llx\n", (unsigned long long)gpu, (unsigned long long)cpu);
    for (i = 0; i < RP_SHOW && g_rps_on && cpu; i++)
        if (g_rps[i].cpu == cpu || !g_rps[i].cpu) {
            g_rps[i].cpu = cpu;
            g_rps[i].gpu = gpu;
            break;
        }
    if (out)
        for (i = 0; i < g_rp.nt; i++)
            if (g_rp.t[i].gpu == gpu && g_rp.t[i].img) shown = i;
    if (shown >= 0) {
        vk_render_end();
        g_rp.cb = vk_cmd();
        rp_barrier();
        rp_readback(shown);
        for (i = 0; all && i < g_rp.nt; i++)
            if (i != shown && g_rp.t[i].img) rp_readback(i);
        gate = vk_submit_number();
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    }
    vk_unlock();
    if (out && shown < 0) LOG("nvn-live: the presented texture %llx has no host image\n", (unsigned long long)gpu);
    if (shown >= 0) {
        char path[700];
        vk_wait_submit(gate);
        for (g_nchk = 0; g_nchk < nkeep; g_nchk++) {
            g_chk[g_nchk].host = malloc(g_chk[g_nchk].size);
            if (g_chk[g_nchk].host) memcpy(g_chk[g_nchk].host, g_chk[g_nchk].p, g_chk[g_nchk].size);
        }
        g_chk_wait = 0;
        snprintf(path, sizeof path, "%s.ppm", out);
        if (g_rb[shown].buf && rp_save(shown, path)) LOG("nvn-live: the presented image saved as %s\n", path);
        if (g_rb[shown].img) vmaDestroyImage(g_vk.vma, g_rb[shown].img, g_rb[shown].img_mem);
        if (g_rb[shown].buf) vmaDestroyBuffer(g_vk.vma, g_rb[shown].buf, g_rb[shown].buf_mem);
        memset(&g_rb[shown], 0, sizeof g_rb[shown]);
        for (i = 0; all && i < g_rp.nt; i++) {
            if (!g_rp.t[i].img || i == shown) continue;
            snprintf(path, sizeof path, "%s-t%02d.ppm", out, i);
            if (g_rb[i].buf && rp_save(i, path))
                LOG("nvn-live:   image %d: GPU %llx, %ux%u, layers %u, format %d\n", i,
                    (unsigned long long)g_rp.t[i].gpu, g_rp.t[i].w, g_rp.t[i].h, g_rp.t[i].layers, (int)g_rp.t[i].f);
            if (g_rb[i].img) vmaDestroyImage(g_vk.vma, g_rb[i].img, g_rb[i].img_mem);
            if (g_rb[i].buf) vmaDestroyBuffer(g_vk.vma, g_rb[i].buf, g_rb[i].buf_mem);
            memset(&g_rb[i], 0, sizeof g_rb[i]);
        }
        snprintf(path, sizeof path, "%s-renderer", out);
        vk_capture_next_present(cpu ? cpu : ~0ull, path);
    }
}
void vk_nvn_live_tex_free(u64 gpu) {
    int i;
    if (!g_rp.live || !gpu) return;
    vk_lock();
    for (i = 0; i < g_rp.nt; i++)
        if (g_rp.t[i].gpu == gpu) rp_retire(i);
    vk_unlock();
}
void vk_nvn_live_stats(u64 frames) {
    int i, images = 0, k;
    for (i = 0; i < g_rp.nt; i++) images += g_rp.t[i].img != VK_NULL_HANDLE;
    if (g_ts_pool) {
        vk_lock();
        rp_ts_read();
        vk_unlock();
        if (g_rp.p_frames)
            LOG("nvn-live: GPU time of the host's windows, a frame on average: %.2f ms in %.1f windows\n",
                (double)g_ts_ns / (double)g_rp.p_frames / 1e6, (double)g_ts_windows / (double)g_rp.p_frames);
        g_ts_ns = g_ts_windows = 0;
    }
    if (g_mir_hits || g_mir_miss) {
        u64 blocks = 0, valid = 0, hot = 0, j;
        for (k = 0; k < g_nimport; k++) {
            u64 n = (g_import[k].size + (1u << MIR_SHIFT) - 1) >> MIR_SHIFT;
            if (!g_mir[k].st) continue;
            blocks += n;
            for (j = 0; j < n; j++) {
                valid += g_mir[k].st[j] == MIR_VALID;
                hot += g_mir[k].hot[j] >= MIR_HOT;
                g_mir[k].hot[j] >>= 1; /* what stopped being written comes back */
            }
        }
        LOG("nvn-live: video memory copies: reads %.1f%% from them (%llu, %llu from the pools), %llu of %llu blocks there, "
            "%llu kept in the pools, %llu MiB copied, %llu blocks taken back, tracking %.2f ms a frame; bound %llu MiB, "
            "binding %.1f ms\n",
            100.0 * (double)g_mir_hits / (double)(g_mir_hits + g_mir_miss), (unsigned long long)g_mir_hits,
            (unsigned long long)g_mir_miss, (unsigned long long)valid, (unsigned long long)blocks,
            (unsigned long long)hot, (unsigned long long)(g_mir_bytes >> 20), (unsigned long long)g_mir_taken,
            (double)g_mir_scan_ns / 6e8, (unsigned long long)(g_mir_bound >> 4), (double)g_mir_bind_ns / 1e6);
        g_mir_hits = g_mir_miss = g_mir_bytes = g_mir_taken = g_mir_scan_ns = g_mir_bind_ns = 0;
        if (g_own_made || g_own_n)
            LOG("nvn-live: GPU-written storage buffers: %u blocks in video memory now (%llu MiB), %llu taken (%llu MiB "
                "filled), %llu not (no memory), landed %llu at once and %llu idle (%llu MiB, %.1f ms waiting), %llu "
                "pool writes over them; the game's writes patched in %llu times (%llu KiB)\n",
                g_own_n, (unsigned long long)g_own_n >> 4, (unsigned long long)g_own_made,
                (unsigned long long)(g_own_fill >> 20), (unsigned long long)g_own_fail,
                (unsigned long long)g_land_forced, (unsigned long long)g_land_idle,
                (unsigned long long)(g_land_bytes >> 20), (double)g_land_ns / 1e6, (unsigned long long)g_own_lost,
                (unsigned long long)g_patch_blocks, (unsigned long long)(g_patch_bytes >> 10));
        g_patch_blocks = g_patch_bytes = 0;
        if (g_land_forced)
            LOG("nvn-live: landed at once because: game wrote %llu, texture %llu, CPU constant copy %llu, no memory "
                "%llu, no tracking %llu, partial read %llu, pool write %llu, buffer copy %llu, constant update %llu\n",
                (unsigned long long)g_land_by[0], (unsigned long long)g_land_by[1], (unsigned long long)g_land_by[2],
                (unsigned long long)g_land_by[3], (unsigned long long)g_land_by[4], (unsigned long long)g_land_by[5],
                (unsigned long long)g_land_by[6], (unsigned long long)g_land_by[7], (unsigned long long)g_land_by[8]);
        memset(g_land_by, 0, sizeof g_land_by);
        g_own_made = g_own_fill = g_own_fail = g_land_forced = g_land_idle = g_land_bytes = g_land_ns = 0;
    }
    pm_lock(&g_prq_lk);
    if (g_pr_set)
        LOG("nvn-live: shader preparation: game gave %llu stages, done %llu, %.1f ms each, gone before their turn %llu, "
            "left %u, paused %llu times\n",
            (unsigned long long)g_pr_set, (unsigned long long)g_pr_done,
            g_pr_done ? (double)g_pr_ns / (double)g_pr_done / 1e6 : 0.0, (unsigned long long)g_pr_gone,
            g_prq_in - g_prq_out, (unsigned long long)g_pr_paused);
    pm_unlock(&g_prq_lk);
    pm_lock(&g_pq_lk);
    if (g_ps.made || g_pw_n)
        LOG("nvn-live: pipelines: made %llu, a pipeline %.1f ms in the queue (longest %.1f) and %.1f compiling (longest "
            "%.1f), most queued %u, moved to the front %llu; draws waited %llu times, %.1f ms, longest %.1f\n",
            (unsigned long long)g_ps.made, g_ps.made ? (double)g_ps.queue_ns / (double)g_ps.made / 1e6 : 0.0,
            (double)g_ps.queue_max / 1e6, g_ps.made ? (double)g_ps.make_ns / (double)g_ps.made / 1e6 : 0.0,
            (double)g_ps.make_max / 1e6, g_ps.depth, (unsigned long long)g_ps.front, (unsigned long long)g_pw_n,
            (double)g_pw_ns / 1e6, (double)g_pw_max / 1e6);
    memset(&g_ps, 0, sizeof g_ps);
    g_pw_n = g_pw_ns = g_pw_max = 0;
    pm_unlock(&g_pq_lk);
    if (g_vk.has_gpl) {
        VkLibStats s;
        vk_pipeline_lib_stats(&s);
        if (s.links)
            LOG("nvn-live: pipeline libraries: vertex stages %llu, %.1f ms (longest %.1f); fragment %llu, %.1f ms (longest "
                "%.1f); input %llu, output %llu, %.1f ms; links %llu, %.2f ms each (longest %.1f)\n",
                (unsigned long long)s.n[1], (double)s.ns[1] / 1e6, (double)s.max[1] / 1e6, (unsigned long long)s.n[2],
                (double)s.ns[2] / 1e6, (double)s.max[2] / 1e6, (unsigned long long)s.n[0], (unsigned long long)s.n[3],
                (double)(s.ns[0] + s.ns[3]) / 1e6, (unsigned long long)s.links,
                (double)s.link_ns / (double)s.links / 1e6, (double)s.link_max / 1e6);
    }
    {
        static u64 astc_seen;
        u32 i, n = 0, sum = 0, median = 0, p99 = 0;
        for (i = 0; i < 256; i++) n += g_ft[i];
        for (i = 0; i < 256 && n; i++) {
            sum += g_ft[i];
            if (!median && sum * 2 >= n) median = i + 1;
            if (!p99 && (u64)sum * 100 >= (u64)n * 99) p99 = i + 1;
        }
        if (n)
            LOG("nvn-live: frame times: median %u ms, p99 %u ms, 1%% low %.1f FPS; ASTC %.1f Mpx since the last report\n",
                median, p99, 1000.0 / (double)p99, (double)(g_vk_astc_px - astc_seen) / 1e6);
        astc_seen = g_vk_astc_px;
        memset(g_ft, 0, sizeof g_ft);
    }
    if (g_rp.p_frames) { /* where a frame goes: on the game's thread until the present, whatever is left is the game's */
        double n = (double)g_rp.p_frames;
        u64 slept = __atomic_load_n(&g_pace_slept_ns, __ATOMIC_RELAXED);
        u64 restarts = __atomic_load_n(&g_pace_restarts, __ATOMIC_RELAXED);
        LOG("nvn-live: pace, a frame on average: before the first window %.1f ms, windows %.1f (vk_lock %.1f), after the "
            "last %.1f; held by the pacer %.1f; frames over 33.4 ms %llu of %llu; schedule restarts %llu; GPU busy at the "
            "first window %llu; at the game's 30 FPS or less %llu\n",
            (double)g_rp.p_before / n / 1e6, (double)g_rp.p_win / n / 1e6, (double)g_rp.p_lock / n / 1e6,
            (double)g_rp.p_after / n / 1e6, (double)(slept - g_rp.p_slept) / n / 1e6, (unsigned long long)g_rp.p_over,
            (unsigned long long)g_rp.p_at60, (unsigned long long)(restarts - g_rp.p_restarts),
            (unsigned long long)g_rp.p_busy, (unsigned long long)g_rp.p_at30);
        g_rp.p_slept = slept;
        g_rp.p_restarts = restarts;
        g_rp.p_frames = g_rp.p_over = g_rp.p_win = g_rp.p_lock = g_rp.p_before = g_rp.p_after = g_rp.p_busy = 0;
        g_rp.p_at60 = g_rp.p_at30 = 0;
    }
    vk_mem_log();
    LOG("nvn-live: %llu frames: draws %llu, drawn %llu; clears %llu, done %llu; dispatches %llu, done %llu; left out "
        "%llu; images %d; staging %llu MiB\n",
        (unsigned long long)frames, (unsigned long long)g_rp.draws, (unsigned long long)g_rp.drawn,
        (unsigned long long)g_rp.clears, (unsigned long long)g_rp.cleared, (unsigned long long)g_rp.dispatches,
        (unsigned long long)g_rp.dispatched, (unsigned long long)g_rp.skipped, images,
        (unsigned long long)(g_rp.sat >> 20));
    if (frames > g_rp.frame_seen)
        LOG("nvn-live: recording %.1f ms a frame; worst frame %.1f ms, over 50 ms %llu; draws waiting for a pipeline %llu; "
            "texture uploads put off %llu\n",
            (double)(g_rp.win_ns - g_rp.win_ns_seen) / 1e6 / (double)(frames - g_rp.frame_seen),
            (double)g_rp.worst_ns / 1e6, (unsigned long long)g_rp.slow_frames,
            (unsigned long long)(g_rp.deferred - g_rp.deferred_seen), (unsigned long long)g_vk_tex_deferred);
    g_rp.worst_ns = g_rp.slow_frames = 0;
    g_rp.deferred_seen = g_rp.deferred;
    g_rp.win_ns_seen = g_rp.win_ns;
    g_rp.frame_seen = frames;
    for (k = 0; k < 16 && g_rp.why_n[k]; k++)
        LOG("nvn-live:   left out, %llu: %s\n", (unsigned long long)g_rp.why_n[k], g_rp.why[k]);
}
void vk_nvn_replay_end(u64 present_gpu, u64 present_cpu, const char* out) {
    int i, k, shown = -1;
    u32 u;
    char path[700];
    if (!g_rp.on || g_rp.live) return;
    rp_end_rendering();
    rp_barrier();
    for (i = 0; i < g_rp.nt; i++) {
        rp_readback(i);
        if (g_rp.t[i].gpu == present_gpu && g_rb[i].buf) shown = i;
    }
    rp_barrier();
    vkEndCommandBuffer(g_rp.cb);
    vk_lock(); /* after the renderer's commands so far: the textures the replay samples are uploaded by them */
    vk_render_end();
    for (u = 0; u < g_rp.nused; u++)
        vk_surf_use(vk_cmd(), g_rp.used[u], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_rp.cb};
    vk_queue_lock();
    VkResult r = vkQueueSubmit(g_vk.gfx, 1, &si, g_rp.fence);
    vk_queue_unlock();
    vk_unlock();
    if (r == VK_SUCCESS) r = vkWaitForFences(g_vk.dev, 1, &g_rp.fence, VK_TRUE, 20000000000ull);
    LOG("nvn-replay: frame replayed: %s; draws %llu, drawn %llu; clears %llu, done %llu; left out %llu; targets "
        "%d, renderer textures %u, staging %llu KiB, descriptor pools %d; dispatches %llu, done %llu; written ranges copied %u (%u not); presented %llx at CPU %llx\n",
        r == VK_SUCCESS ? "done" : "the GPU didn't finish it", (unsigned long long)g_rp.draws,
        (unsigned long long)g_rp.drawn, (unsigned long long)g_rp.clears, (unsigned long long)g_rp.cleared,
        (unsigned long long)g_rp.skipped, g_rp.nt, g_rp.nused, (unsigned long long)(g_rp.sat >> 10), g_rp.npool,
        (unsigned long long)g_rp.dispatches, (unsigned long long)g_rp.dispatched, g_nrpw, g_rpw_full,
        (unsigned long long)present_gpu, (unsigned long long)present_cpu);
    for (k = 0; k < 16 && g_rp.why_n[k]; k++)
        LOG("nvn-replay:   left out, %llu: %s\n", (unsigned long long)g_rp.why_n[k], g_rp.why[k]);
    for (i = 0; i < g_rp.nt; i++)
        LOG("nvn-replay:   target %d: GPU %llx, %ux%u, format %d%s\n", i, (unsigned long long)g_rp.t[i].gpu,
            g_rp.t[i].w, g_rp.t[i].h, (int)g_rp.t[i].f, g_rp.t[i].gpu == present_gpu ? " (presented)" : "");
    for (i = 0; i < g_rp.nt && r == VK_SUCCESS; i++) {
        snprintf(path, sizeof path, "%s-t%02d.ppm", out, i);
        if (rp_save(i, path))
            LOG("nvn-replay:   target %d saved as %s (%ux%u, %s; float components NaN %llu, infinite %llu, non-zero "
                "%llu, largest %g)\n",
                i, path, g_rp.t[i].w, g_rp.t[i].h,
                g_rb[i].srgb < 0    ? "depth as grey"
                : g_rb[i].srgb == 2 ? "float, tone mapped"
                : g_rb[i].srgb      ? "sRGB"
                                    : "linear",
                (unsigned long long)g_hdr[0], (unsigned long long)g_hdr[1], (unsigned long long)g_hdr[2],
                (double)g_hdr_max);
    }
    snprintf(path, sizeof path, "%s.ppm", out);
    if (shown >= 0 && r == VK_SUCCESS && rp_save(shown, path))
        LOG("nvn-replay: the presented target (%d) saved as %s\n", shown, path);
    else
        LOG("nvn-replay: the presented texture %llx is not one of the replay's targets\n",
            (unsigned long long)present_gpu);
    snprintf(path, sizeof path, "%s-renderer", out); /* its buffer, or the next present when its address is not known */
    vk_capture_next_present(present_cpu ? present_cpu : ~0ull, path);
    g_rp.on = 0;
    if (r != VK_SUCCESS) { /* the GPU may still use them: kept (a one-off diagnostic) */
        LOG("nvn-replay: its objects are kept, not destroyed\n");
        return;
    }
    for (i = 0; i < g_rp.nt; i++) {
        if (g_rb[i].img) vmaDestroyImage(g_vk.vma, g_rb[i].img, g_rb[i].img_mem);
        if (g_rb[i].buf) vmaDestroyBuffer(g_vk.vma, g_rb[i].buf, g_rb[i].buf_mem);
        for (k = 0; k < g_rp.t[i].nview; k++) vkDestroyImageView(g_vk.dev, g_rp.t[i].view[k].v, NULL);
        vk_mem_untag(g_rp.t[i].mem);
        vmaDestroyImage(g_vk.vma, g_rp.t[i].img, g_rp.t[i].mem);
    }
    vk_mem_untag(g_rp.smem);
    vmaDestroyBuffer(g_vk.vma, g_rp.sbuf, g_rp.smem);
    for (i = 0; i < g_rp.npool; i++) vkDestroyDescriptorPool(g_vk.dev, g_rp.dpool[i], NULL);
    vkDestroyFence(g_vk.dev, g_rp.fence, NULL);
    vkDestroyCommandPool(g_vk.dev, g_rp.pool, NULL);
    g_rp.on = 0;
}

/* ---- when each texture address was first registered (vk_nvn_tex_registered), for the ahead-of-draw measure */
#define KNOWN 65536
static struct {
    u64 gpu;
    u32 present;
} g_known[KNOWN];
static PMutex g_known_lk = PMUTEX_INIT;
static u64 g_known_count;
void vk_nvn_tex_registered(u64 gpu) {
    u32 i = (u32)(gpu >> 8) & (KNOWN - 1), n;
    if (!gpu) return;
    pm_lock(&g_known_lk);
    for (n = 0; n < 64; n++, i = (i + 1) & (KNOWN - 1)) {
        if (g_known[i].gpu == gpu) break; /* known already: the first registration counts */
        if (!g_known[i].gpu) {
            g_known[i].gpu = gpu;
            g_known[i].present = g_vk_present;
            g_known_count++;
            break;
        }
    }
    pm_unlock(&g_known_lk);
}
long long vk_nvn_tex_known(u64 gpu) {
    u32 i = (u32)(gpu >> 8) & (KNOWN - 1), n;
    long long at = -1;
    pm_lock(&g_known_lk);
    for (n = 0; n < 64 && g_known[i].gpu; n++, i = (i + 1) & (KNOWN - 1))
        if (g_known[i].gpu == gpu) {
            at = g_known[i].present;
            break;
        }
    pm_unlock(&g_known_lk);
    return at;
}
