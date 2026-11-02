/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Phase 2 of diagnostics/nvn-route-plan-20261029/PLAN.md (SNX_NVN_SHADOW=1): handlers that run the game's NVN driver
   and then note what the call asked for, so B3's use of NVN (SDK 13.3.2) can be checked against what B2recomp's NVN
   layer (SDK 3.6.0) handles: texture and vertex formats outside B2's tables, the layout of shader control sections
   B2's translator reads, and the sizes and patterns of pools, buffers, command memory, windows and samplers. Nothing
   the game sees changes. The tables print to the log every 30 seconds */
#include <stdio.h>
#include <string.h>
#include "nx/hle/nvn/shadow.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/shader/shader.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

static PMutex g_lk = PMUTEX_INIT;
static u64 g_last_dump;

/* a small set of distinct values with a count each */
#define VALS 48
typedef struct {
    u64 v[VALS], n[VALS];
    u32 used;
    u64 other; /* calls whose value didn't fit */
} Vals;
static void vals_add(Vals* s, u64 v) {
    u32 i;
    for (i = 0; i < s->used; i++)
        if (s->v[i] == v) {
            s->n[i]++;
            return;
        }
    if (s->used < VALS) {
        s->v[s->used] = v;
        s->n[s->used++] = 1;
    } else
        s->other++;
}
static void vals_log(const char* what, const Vals* s, int (*b2_ok)(u64)) {
    char line[1024];
    size_t at = 0;
    u32 i;
    if (!s->used) return;
    for (i = 0; i < s->used && at < sizeof line - 64; i++)
        at += (size_t)snprintf(line + at, sizeof line - at, " %llx:%llu%s", (unsigned long long)s->v[i],
                               (unsigned long long)s->n[i], b2_ok && !b2_ok(s->v[i]) ? "(!B2)" : "");
    LOG("nvn-shadow: %s%s%s\n", what, line, s->other ? " (more not listed)" : "");
}

/* B2recomp's fmt_get and vfmt_get (runtime/nvn_gl.c): the NVN formats they translate */
static int b2_texture_format(u64 f) {
    return (f >= 0x01 && f <= 0x50) || (f >= 0x6D && f <= 0x95) || (f >= 0x99 && f <= 0x9C);
}
static int b2_vertex_format(u64 f) {
    return (f >= 0x01 && f <= 0x30) || (f >= 0x51 && f <= 0x78) || f == 0x3D || f == 0x3E;
}

static Vals g_tex_format, g_view_format, g_tex_target, g_view_target, g_samples, g_tex_flags, g_dsmode, g_vertex_format;
static Vals g_pool_flags, g_present_interval, g_window_textures, g_submit_count, g_min_mag, g_wrap, g_aniso;
static Vals g_stage_kind; /* per shader stage: control magic ok, assembly text, machine code header, stage type */
static u64 g_size3d, g_packaged, g_pool_bytes, g_pools, g_pool_max, g_buffers, g_buffer_bytes;
static u64 g_cmd_mem, g_cmd_mem_bytes, g_ctl_mem, g_ctl_mem_bytes, g_programs, g_stages;
static u32 g_headers_logged, g_headerless_logged;
static Vals g_bind_stages; /* nvnCommandBufferBindProgram's stage bits (B2's cmd_program ignores them) */

/* pass 2: pools and handles, bindings, recording, fences and present */
static Vals g_tpool_size, g_spool_size, g_barrier, g_fence_queue, g_fence_cmd, g_acquire, g_present_index;
static Vals g_bind_tex, g_bind_img, g_bind_ubo, g_bind_ssbo; /* stage << 16 | first << 8 | count */
static u64 g_max_tex_id, g_max_img_id, g_max_smp_id, g_handle_calls, g_handle_b2, g_image_calls, g_image_b2;
static u64 g_wait_sync, g_flush, g_set_tpool, g_set_spool, g_recorded, g_submitted, g_resubmitted, g_unknown;
static u32 g_handle_logged;
#define RECENT 16
static u64 g_recent[RECENT], g_recent_submits[RECENT]; /* command handles EndRecording returned, newest last */
static u32 g_nrecent;

/* guest bytes at a CPU address when [a, a + n) is mapped */
static const u8* cpu_bytes(u64 a, u64 n) {
    MemBlock b;
    if (!g_mem || !a || a < AS_BASE || a >= AS_END || n > AS_END - a) return NULL;
    mem_query(a, &b);
    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !b.size || a + n > b.addr + b.size) return NULL;
    return GP(a);
}
static u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static void tex_dump(void);
static void corpus_dump(void);
static void dump_all(void) {
    vals_log("texture builder formats", &g_tex_format, b2_texture_format);
    vals_log("texture view formats", &g_view_format, b2_texture_format);
    vals_log("texture builder targets", &g_tex_target, NULL);
    vals_log("texture view targets", &g_view_target, NULL);
    vals_log("texture samples", &g_samples, NULL);
    vals_log("texture flags", &g_tex_flags, NULL);
    vals_log("depth-stencil modes", &g_dsmode, NULL);
    vals_log("vertex formats", &g_vertex_format, b2_vertex_format);
    vals_log("pool flags", &g_pool_flags, NULL);
    vals_log("sampler min/mag (min << 16 | mag)", &g_min_mag, NULL);
    vals_log("sampler wrap (s << 16 | t << 8 | r)", &g_wrap, NULL);
    vals_log("sampler max anisotropy (x10)", &g_aniso, NULL);
    vals_log("window textures", &g_window_textures, NULL);
    vals_log("window present interval", &g_present_interval, NULL);
    vals_log("queue submit counts", &g_submit_count, NULL);
    vals_log("bind program stage bits", &g_bind_stages, NULL);
    tex_dump();
    corpus_dump();
    vals_log("texture pool descriptors", &g_tpool_size, NULL);
    vals_log("sampler pool descriptors", &g_spool_size, NULL);
    vals_log("bind textures (stage << 16 | first << 8 | count)", &g_bind_tex, NULL);
    vals_log("bind images (stage << 16 | first << 8 | count)", &g_bind_img, NULL);
    vals_log("bind uniform buffers (stage << 16 | first << 8 | count)", &g_bind_ubo, NULL);
    vals_log("bind storage buffers (stage << 16 | first << 8 | count)", &g_bind_ssbo, NULL);
    vals_log("barrier flags", &g_barrier, NULL);
    vals_log("queue fence (condition << 8 | flags)", &g_fence_queue, NULL);
    vals_log("command fence (condition << 8 | flags)", &g_fence_cmd, NULL);
    vals_log("acquire texture (result << 8 | index)", &g_acquire, NULL);
    vals_log("present index", &g_present_index, NULL);
    LOG("nvn-shadow: texture handles %llu, %llu equal to B2's (texture | sampler << 20); image handles %llu, %llu equal to "
        "B2's (id); ids up to texture %llu, image %llu, sampler %llu\n",
        (unsigned long long)g_handle_calls, (unsigned long long)g_handle_b2, (unsigned long long)g_image_calls,
        (unsigned long long)g_image_b2, (unsigned long long)g_max_tex_id, (unsigned long long)g_max_img_id,
        (unsigned long long)g_max_smp_id);
    LOG("nvn-shadow: command buffers recorded %llu, handles submitted %llu, submitted more than once %llu, not from a "
        "recent EndRecording %llu; queue waits %llu, flushes %llu, texture/sampler pool sets %llu/%llu\n",
        (unsigned long long)g_recorded, (unsigned long long)g_submitted, (unsigned long long)g_resubmitted,
        (unsigned long long)g_unknown, (unsigned long long)g_wait_sync, (unsigned long long)g_flush,
        (unsigned long long)g_set_tpool, (unsigned long long)g_set_spool);
    vals_log("shader stages (magic ok << 28 | text << 24 | code header << 20 | type << 16 | code KiB)", &g_stage_kind,
             NULL);
    LOG("nvn-shadow: pools %llu (%llu MiB, largest %llu MiB), buffers %llu (%llu MiB), command memory %llu (%llu KiB), "
        "control memory %llu (%llu KiB), 3D sizes %llu, packaged layouts %llu, programs %llu with %llu stages\n",
        (unsigned long long)g_pools, (unsigned long long)(g_pool_bytes >> 20), (unsigned long long)(g_pool_max >> 20),
        (unsigned long long)g_buffers, (unsigned long long)(g_buffer_bytes >> 20), (unsigned long long)g_cmd_mem,
        (unsigned long long)(g_cmd_mem_bytes >> 10), (unsigned long long)g_ctl_mem,
        (unsigned long long)(g_ctl_mem_bytes >> 10), (unsigned long long)g_size3d, (unsigned long long)g_packaged,
        (unsigned long long)g_programs, (unsigned long long)g_stages);
}
/* the lock held: the tables every 30 s */
static void maybe_dump(void) {
    u64 now = plat_ns();
    if (!g_last_dump) g_last_dump = now;
    if (now - g_last_dump < 30000000000ull) return;
    g_last_dump = now;
    dump_all();
}
#define NOTE(set, value)                                                                                               \
    do {                                                                                                               \
        pm_lock(&g_lk);                                                                                                \
        vals_add(&(set), (value));                                                                                     \
        maybe_dump();                                                                                                  \
        pm_unlock(&g_lk);                                                                                              \
    } while (0)

/* each handler: the arguments it notes, the driver, then the note (the driver may change x0..x18) */
#define ARG1_HANDLER(fn, set, expr)                                                                                    \
    static void fn(CPU* RESTRICT c, const NvnFn* f) {                                                                  \
        u64 a1 = c->x[1], a2 = c->x[2], a3 = c->x[3];                                                                  \
        (void)a1;                                                                                                      \
        (void)a2;                                                                                                      \
        (void)a3;                                                                                                      \
        nvn_guest(c, f);                                                                                               \
        NOTE(set, (expr));                                                                                             \
    }
ARG1_HANDLER(view_format, g_view_format, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(view_target, g_view_target, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(texb_dsmode, g_dsmode, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(view_dsmode, g_dsmode, (a1 & 0xFFFFFFFFu) | 0x100)
ARG1_HANDLER(vattr_format, g_vertex_format, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(pool_flags, g_pool_flags, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(win_interval, g_present_interval, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(win_textures, g_window_textures, a1 & 0xFFFFFFFFu)
ARG1_HANDLER(smp_minmag, g_min_mag, ((a1 & 0xFFFF) << 16) | (a2 & 0xFFFF))
ARG1_HANDLER(smp_wrap, g_wrap, ((a1 & 0xFF) << 16) | ((a2 & 0xFF) << 8) | (a3 & 0xFF))
ARG1_HANDLER(bind_program, g_bind_stages, a2 & 0xFFFFFFFFu) /* (cmdbuf, program, stage bits) */
/* (pool, memory pool, offset, descriptors) */
#define ARG3_HANDLER(fn, set) ARG1_HANDLER(fn, set, a3 & 0xFFFFFFFFu)
ARG3_HANDLER(tpool_init, g_tpool_size)
ARG3_HANDLER(spool_init, g_spool_size)
/* (cmdbuf, stage, first, count, list) */
#define BIND_HANDLER(fn, set) ARG1_HANDLER(fn, set, ((a1 & 0xFF) << 16) | ((a2 & 0xFF) << 8) | (a3 & 0xFF))
BIND_HANDLER(bind_textures, g_bind_tex)
BIND_HANDLER(bind_images, g_bind_img)
BIND_HANDLER(bind_ubos, g_bind_ubo)
BIND_HANDLER(bind_ssbos, g_bind_ssbo)
ARG1_HANDLER(barrier, g_barrier, a1 & 0xFFFFFFFFu)                                         /* (cmdbuf, flags) */
ARG1_HANDLER(fence_queue, g_fence_queue, ((a2 & 0xFF) << 8) | (a3 & 0xFF))                /* (queue, sync, condition, flags) */
ARG1_HANDLER(fence_cmd, g_fence_cmd, ((a2 & 0xFF) << 8) | (a3 & 0xFF))                    /* (cmdbuf, sync, condition, flags) */
ARG1_HANDLER(present, g_present_index, a2 & 0xFFFFFFFFu)                                  /* (queue, window, index) */

#define COUNT_HANDLER(fn, counter)                                                                                     \
    static void fn(CPU* RESTRICT c, const NvnFn* f) {                                                                  \
        nvn_guest(c, f);                                                                                               \
        pm_lock(&g_lk);                                                                                                \
        (counter)++;                                                                                                   \
        pm_unlock(&g_lk);                                                                                              \
    }
COUNT_HANDLER(wait_sync, g_wait_sync)
COUNT_HANDLER(queue_flush, g_flush)
COUNT_HANDLER(set_tpool, g_set_tpool)
COUNT_HANDLER(set_spool, g_set_spool)

/* (pool, id, texture or sampler, view): the largest id registered */
#define REGISTER_HANDLER(fn, max)                                                                                      \
    static void fn(CPU* RESTRICT c, const NvnFn* f) {                                                                  \
        u64 id = c->x[1] & 0xFFFFFFFFu;                                                                                \
        nvn_guest(c, f);                                                                                               \
        pm_lock(&g_lk);                                                                                                \
        if (id > (max)) (max) = id;                                                                                    \
        pm_unlock(&g_lk);                                                                                              \
    }
REGISTER_HANDLER(reg_texture, g_max_tex_id)
REGISTER_HANDLER(reg_image, g_max_img_id)
REGISTER_HANDLER(reg_sampler, g_max_smp_id)

/* nvnDeviceGetTextureHandle(device, textureID, samplerID): B2 answers textureID | samplerID << 20 (and bit 32) itself,
   which the game's shaders then read; the driver's answer is checked against it (low 32 bits) */
static void tex_handle(CPU* RESTRICT c, const NvnFn* f) {
    u64 tex = c->x[1] & 0xFFFFFFFFu, smp = c->x[2] & 0xFFFFFFFFu, h, b2 = (tex | smp << 20) & 0xFFFFFFFFu;
    nvn_guest(c, f);
    h = c->x[0];
    pm_lock(&g_lk);
    g_handle_calls++;
    if ((h & 0xFFFFFFFFu) == b2)
        g_handle_b2++;
    else if (g_handle_logged < 6) {
        g_handle_logged++;
        LOG("nvn-shadow: texture handle for texture %llu sampler %llu is %llx, B2 would answer %llx\n",
            (unsigned long long)tex, (unsigned long long)smp, (unsigned long long)h, (unsigned long long)b2);
    }
    pm_unlock(&g_lk);
}
static void img_handle(CPU* RESTRICT c, const NvnFn* f) { /* (device, imageID): B2 answers the id */
    u64 id = c->x[1] & 0xFFFFFFFFu, h;
    nvn_guest(c, f);
    h = c->x[0];
    pm_lock(&g_lk);
    g_image_calls++;
    if ((h & 0xFFFFFFFFu) == id)
        g_image_b2++;
    else if (g_handle_logged < 12) {
        g_handle_logged++;
        LOG("nvn-shadow: image handle for image %llu is %llx, B2 would answer %llx\n", (unsigned long long)id,
            (unsigned long long)h, (unsigned long long)id);
    }
    pm_unlock(&g_lk);
}
static void end_recording(CPU* RESTRICT c, const NvnFn* f) { /* (cmdbuf) -> handle */
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_recorded++;
    if (g_nrecent == RECENT) {
        memmove(g_recent, g_recent + 1, sizeof g_recent - sizeof *g_recent);
        memmove(g_recent_submits, g_recent_submits + 1, sizeof g_recent_submits - sizeof *g_recent_submits);
        g_nrecent--;
    }
    g_recent[g_nrecent] = c->x[0];
    g_recent_submits[g_nrecent++] = 0;
    pm_unlock(&g_lk);
}
static void queue_submit(CPU* RESTRICT c, const NvnFn* f) { /* (queue, count, handles) */
    u64 count = c->x[1] & 0xFFFFFFFFu, list = c->x[2], i;
    const u8* h = count && count < 64 ? cpu_bytes(list, count * 8) : NULL;
    u64 handles[64];
    if (h) memcpy(handles, h, count * 8);
    nvn_guest(c, f);
    pm_lock(&g_lk);
    vals_add(&g_submit_count, count);
    for (i = 0; h && i < count; i++) {
        u32 k;
        g_submitted++;
        for (k = 0; k < g_nrecent && g_recent[k] != handles[i]; k++) {}
        if (k == g_nrecent)
            g_unknown++;
        else if (g_recent_submits[k]++)
            g_resubmitted++;
    }
    maybe_dump();
    pm_unlock(&g_lk);
}
static void acquire(CPU* RESTRICT c, const NvnFn* f) { /* (window, sync, int* index) -> result */
    u64 at = c->x[2];
    const u8* p;
    u32 index = 0xFF;
    nvn_guest(c, f);
    p = cpu_bytes(at, 4);
    if (p) index = rd32(p) & 0xFF;
    NOTE(g_acquire, ((c->x[0] & 0xFF) << 8) | index);
}

static void smp_aniso(CPU* RESTRICT c, const NvnFn* f) { /* (builder, float) */
    float v;
    memcpy(&v, &c->v[0], 4);
    nvn_guest(c, f);
    NOTE(g_aniso, (u64)(v * 10.0f));
}
/* ------------------------------------------------------------ phase 3 dual check: textures read back by the host */
/* A host copy of each texture builder, from its setters. After nvnTextureInitialize the texture is read back through
   the driver's own getters (nvn_call) and compared with the copy: how a host renderer will learn a texture */
enum { TB_FLAGS, TB_TARGET, TB_FORMAT, TB_WIDTH, TB_HEIGHT, TB_DEPTH, TB_LEVELS, TB_SAMPLES, TB_FIELDS };
static const char* const g_tb_getter[TB_FIELDS] = {
    "nvnTextureGetFlags", "nvnTextureGetTarget", "nvnTextureGetFormat", "nvnTextureGetWidth",
    "nvnTextureGetHeight", "nvnTextureGetDepth", "nvnTextureGetLevels", "nvnTextureGetSamples"};
#define BUILDERS 512u
typedef struct {
    u64 addr;
    u32 v[TB_FIELDS];
} TexCopy;
static TexCopy g_tb[BUILDERS];
static u64 g_tex_checked, g_tex_nocopy, g_tex_nocall, g_tex_field_ok[TB_FIELDS], g_tex_field_bad[TB_FIELDS];
static u64 g_tex_addr_zero, g_tex_size_zero;
static u32 g_tex_logged;

/* the copy of the builder at addr, made (with the driver's defaults: target 2D, one level) when new. Lock held */
static TexCopy* tb_copy(u64 addr, int make) {
    u32 i = (u32)(addr >> 3) & (BUILDERS - 1), n;
    for (n = 0; n < BUILDERS; n++, i = (i + 1) & (BUILDERS - 1)) {
        if (g_tb[i].addr == addr) return &g_tb[i];
        if (!g_tb[i].addr) break;
    }
    if (!make) return NULL;
    if (n == BUILDERS) i = (u32)(addr >> 3) & (BUILDERS - 1); /* full: the slot is reused */
    memset(&g_tb[i], 0, sizeof g_tb[i]);
    g_tb[i].addr = addr;
    g_tb[i].v[TB_TARGET] = 1;
    g_tb[i].v[TB_LEVELS] = 1;
    return &g_tb[i];
}
static void texb_defaults(CPU* RESTRICT c, const NvnFn* f) { /* (builder) */
    u64 b = c->x[0];
    TexCopy* t;
    nvn_guest(c, f);
    pm_lock(&g_lk);
    t = tb_copy(b, 1);
    memset(t->v, 0, sizeof t->v);
    t->v[TB_TARGET] = 1;
    t->v[TB_LEVELS] = 1;
    pm_unlock(&g_lk);
}
/* (builder, value): a field of the copy, and the value noted for the inventory */
static void tb_set(CPU* RESTRICT c, const NvnFn* f, int field, Vals* note) {
    u64 b = c->x[0];
    u32 v = (u32)c->x[1];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    tb_copy(b, 1)->v[field] = v;
    if (note) vals_add(note, v);
    maybe_dump();
    pm_unlock(&g_lk);
}
static void texb_format(CPU* RESTRICT c, const NvnFn* f) { tb_set(c, f, TB_FORMAT, &g_tex_format); }
static void texb_target(CPU* RESTRICT c, const NvnFn* f) { tb_set(c, f, TB_TARGET, &g_tex_target); }
static void texb_samples(CPU* RESTRICT c, const NvnFn* f) { tb_set(c, f, TB_SAMPLES, &g_samples); }
static void texb_flags(CPU* RESTRICT c, const NvnFn* f) { tb_set(c, f, TB_FLAGS, &g_tex_flags); }
static void texb_levels(CPU* RESTRICT c, const NvnFn* f) { tb_set(c, f, TB_LEVELS, NULL); }
static void texb_size(CPU* RESTRICT c, const NvnFn* f, int dims) { /* (builder, width, height[, depth]) */
    u64 b = c->x[0];
    u32 w = (u32)c->x[1], h = (u32)c->x[2], d = dims == 3 ? (u32)c->x[3] : 1;
    TexCopy* t;
    nvn_guest(c, f);
    pm_lock(&g_lk);
    t = tb_copy(b, 1);
    t->v[TB_WIDTH] = w;
    t->v[TB_HEIGHT] = h;
    t->v[TB_DEPTH] = d;
    if (dims == 3) g_size3d++;
    pm_unlock(&g_lk);
}
static void texb_size2d(CPU* RESTRICT c, const NvnFn* f) { texb_size(c, f, 2); }
static void texb_size3d(CPU* RESTRICT c, const NvnFn* f) { texb_size(c, f, 3); }
/* nvnTextureInitialize(texture, builder) -> bool: the texture read back through the driver's getters */
static void tex_init(CPU* RESTRICT c, const NvnFn* f) {
    u64 tex = c->x[0], b = c->x[1], got[TB_FIELDS], addr = 0, size = 0;
    TexCopy copy;
    TexCopy* t;
    int ok = 1, i;
    nvn_guest(c, f);
    if (!(c->x[0] & 0xFF)) return; /* NVNboolean false: not initialized */
    pm_lock(&g_lk);
    t = tb_copy(b, 0);
    if (t) copy = *t;
    pm_unlock(&g_lk);
    if (!t) {
        pm_lock(&g_lk);
        g_tex_nocopy++;
        pm_unlock(&g_lk);
        return;
    }
    for (i = 0; i < TB_FIELDS && ok; i++) ok = nvn_call(c, g_tb_getter[i], &tex, 1, &got[i]);
    if (ok) ok = nvn_call(c, "nvnTextureGetTextureAddress", &tex, 1, &addr) &&
                 nvn_call(c, "nvnTextureGetStorageSize", &tex, 1, &size);
    pm_lock(&g_lk);
    if (!ok)
        g_tex_nocall++;
    else {
        int bad = 0;
        g_tex_checked++;
        if (!addr) g_tex_addr_zero++;
        if (!size) g_tex_size_zero++;
        for (i = 0; i < TB_FIELDS; i++)
            if ((u32)got[i] == copy.v[i])
                g_tex_field_ok[i]++;
            else {
                g_tex_field_bad[i]++;
                bad = 1;
            }
        if (bad && g_tex_logged < 8) {
            g_tex_logged++;
            LOG("nvn-shadow: texture %llx read back as flags %x target %u format %x %ux%ux%u levels %u samples %u; "
                "builder copy flags %x target %u format %x %ux%ux%u levels %u samples %u\n",
                (unsigned long long)tex, (u32)got[0], (u32)got[1], (u32)got[2], (u32)got[3], (u32)got[4], (u32)got[5],
                (u32)got[6], (u32)got[7], copy.v[0], copy.v[1], copy.v[2], copy.v[3], copy.v[4], copy.v[5], copy.v[6],
                copy.v[7]);
        }
    }
    maybe_dump();
    pm_unlock(&g_lk);
}
static void tex_dump(void) { /* the lock held */
    char line[512];
    size_t at = 0;
    int i;
    for (i = 0; i < TB_FIELDS; i++)
        at += (size_t)snprintf(line + at, sizeof line - at, " %s %llu/%llu", g_tb_getter[i] + 10,
                               (unsigned long long)g_tex_field_ok[i],
                               (unsigned long long)(g_tex_field_ok[i] + g_tex_field_bad[i]));
    LOG("nvn-shadow: textures read back %llu (no builder copy %llu, getter not found %llu, address 0: %llu, size 0: "
        "%llu); fields equal to the builder copy:%s\n",
        (unsigned long long)g_tex_checked, (unsigned long long)g_tex_nocopy, (unsigned long long)g_tex_nocall,
        (unsigned long long)g_tex_addr_zero, (unsigned long long)g_tex_size_zero, line);
}
static void texb_packaged(CPU* RESTRICT c, const NvnFn* f) {
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_packaged++;
    pm_unlock(&g_lk);
}
static void pool_storage(CPU* RESTRICT c, const NvnFn* f) { /* (builder, memory, size) */
    u64 size = c->x[2];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_pools++;
    g_pool_bytes += size;
    if (size > g_pool_max) g_pool_max = size;
    pm_unlock(&g_lk);
}
static void buffer_storage(CPU* RESTRICT c, const NvnFn* f) { /* (builder, pool, offset, size) */
    u64 size = c->x[3];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_buffers++;
    g_buffer_bytes += size;
    pm_unlock(&g_lk);
}
static void cmd_memory(CPU* RESTRICT c, const NvnFn* f) { /* (cmdbuf, pool, offset, size) */
    u64 size = c->x[3];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_cmd_mem++;
    g_cmd_mem_bytes += size;
    pm_unlock(&g_lk);
}
static void ctl_memory(CPU* RESTRICT c, const NvnFn* f) { /* (cmdbuf, memory, size) */
    u64 size = c->x[2];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_ctl_mem++;
    g_ctl_mem_bytes += size;
    pm_unlock(&g_lk);
}
/* SNX_NVN_CORPUS=dir: each stage's code goes into a shader corpus (gpu/shader/corpus.c) for snx-shaderc to translate
   offline, which keeps the translator off the game's threads. As B2recomp's prog_set_shaders reads them: a stage with
   the machine-code header has its 0x50-byte program header at data+0x30 and control+0x6f8 bytes of instructions after
   it (its stage from the header's type); a compute stage (control+0x714 = 5, or no header) is control+0x6f8 bytes of
   code at data, its work group at control+0x748 and shared memory at control+0x754. The saved length is the one the
   program's own end gives (shader_hash), as the renderer saves it: bytes past the end would decode as instructions */
static u64 g_corpus_saved, g_corpus_failed, g_corpus_cb1;
static u32 g_compute_logged;
static const char* corpus_dir(void) {
    static int done;
    static const char* dir;
    if (!done) {
        dir = getenv("SNX_NVN_CORPUS");
        if (dir && !*dir) dir = NULL;
        done = 1;
    }
    return dir;
}
static void corpus_dump(void) { /* the lock held */
    if (corpus_dir())
        LOG("nvn-shadow: shader corpus %s: %llu stages saved, %llu not, %llu with constant buffer 1\n", corpus_dir(),
            (unsigned long long)g_corpus_saved, (unsigned long long)g_corpus_failed, (unsigned long long)g_corpus_cb1);
}
static void corpus_stage(u64 data, int header, u32 type, const u8* ctl) { /* the lock held */
    static const int stage_of[6] = {-1, ST_VS, ST_TCS, ST_TES, ST_GS, ST_FS};
    const char* dir = corpus_dir();
    int compute = !header || rd32(ctl + 0x714) == 5;
    int stage = compute ? ST_CS : type < 6 ? stage_of[type] : -1;
    u32 len = rd32(ctl + 0x6f8), n = compute ? len : 0x50 + len, own = 0, i;
    /* SDK 13.3.2 compute, measured against the programs the renderer runs (diagnostics/nvn-port-20261029): data that
       starts with 0x12345679 is a 0x100-byte header, the code after it, its length counted from there and its constant
       data at data + word 5 of control+0x6f0 (word 4 + 0x100). Data without it is code from data on, its constant data
       at data + word 4, as B2recomp's SDK 3.6 reading has both */
    const u8* first = compute ? gmem_ptr_untracked(data, 4) : NULL;
    int cs_header = first && rd32(first) == 0x12345679u;
    u32 w4 = rd32(ctl + 0x700), w5 = rd32(ctl + 0x704), entry = cs_header ? 0x100 : 0;
    u64 at = compute ? data + entry : data + 0x30, key;
    const u8* code;
    ShaderInfo si;
    if (!dir) return;
    if (stage < 0 || !len || n > 0x10000 || n < 96 || !(code = gmem_ptr_untracked(at, n))) {
        g_corpus_failed++;
        return;
    }
    if (compute) { /* no program header: 80 zero bytes stand in for one, as the renderer frames compute code */
        static u8 framed[0x10000 + 80];
        memset(framed, 0, 80);
        memcpy(framed + 80, code, n);
        code = framed;
        n += 80;
    }
    key = shader_hash(code, n, &own) ^ (u64)stage;
    if (own && own < n) n = own;
    memset(&si, 0, sizeof si);
    si.stage = stage;
    if (compute) {
        for (i = 0; i < 3; i++) si.cs_local[i] = rd32(ctl + 0x748 + i * 4);
        si.shared_size = (int)rd32(ctl + 0x754);
    }
    if (shader_corpus_save(dir, code, n, compute ? len + 80 : 0x50 + len, stage, &si, key))
        g_corpus_saved++;
    else
        g_corpus_failed++;
    /* the program's own constant data, constant buffer 1 (its BRX jump tables): the 8 words at control+0x6f0 hold its
       size (word 3) and offset from data (word 4), as B2recomp binds it. Saved beside the entry for snx-shaderc */
    {
        u32 cb_size = rd32(ctl + 0x6fc), cb_off = cs_header ? w5 : w4;
        const u8* cb = cb_size && cb_off && cb_size <= 0x10000 ? gmem_ptr_untracked(data + cb_off, cb_size) : NULL;
        char path[1100];
        FILE* fp;
        if (cb && snprintf(path, sizeof path, "%s/%016llx_%d.cb1", dir, (unsigned long long)key, stage) <
                      (int)sizeof path &&
            (fp = fopen(path, "wb"))) {
            if (fwrite(cb, 1, cb_size, fp) == cb_size) g_corpus_cb1++;
            fclose(fp);
        }
    }
    if (compute && g_compute_logged < 400) {
        const u8* d = gmem_ptr_untracked(data, 0x110);
        g_compute_logged++;
        LOG("nvn-shadow: compute stage %016llx: control+0x6f0: %08x %08x %08x %08x %08x %08x %08x %08x, +0x714 %x, +0x748 %u %u %u "
            "| data+0: %08x %08x | data+0x100: %08x %08x\n",
            (unsigned long long)key, rd32(ctl + 0x6f0), rd32(ctl + 0x6f4), rd32(ctl + 0x6f8), rd32(ctl + 0x6fc), rd32(ctl + 0x700),
            rd32(ctl + 0x704), rd32(ctl + 0x708), rd32(ctl + 0x70c), rd32(ctl + 0x714), rd32(ctl + 0x748),
            rd32(ctl + 0x74c), rd32(ctl + 0x750), d ? rd32(d) : 0, d ? rd32(d + 4) : 0, d ? rd32(d + 0x100) : 0,
            d ? rd32(d + 0x104) : 0);
    }
}
/* nvnProgramSetShaders(program, count, NVNshaderData*): {u64 data (GPU address), u64 control (CPU address)} each.
   B2's prog_set_shaders reads control+0 (0x98761234), the assembly text at control+0x14/+0x18, a machine-code header
   at data (0x12345678) with the code at data+0x30 (stage type in bits 10..13 of its first word) and its length at
   control+0x6f8. Each stage is noted by which of these hold, and the first few headers are logged */
static void prog_shaders(CPU* RESTRICT c, const NvnFn* f) {
    u64 count = c->x[1], list = c->x[2], i;
    nvn_guest(c, f);
    pm_lock(&g_lk);
    g_programs++;
    for (i = 0; i < count && i < 6; i++) {
        const u8* e = cpu_bytes(list + i * 16, 16);
        u64 data = 0, control = 0, kind;
        const u8 *ctl, *dat;
        int magic, text, header;
        u32 type = 15, code_len = 0;
        if (!e) break;
        memcpy(&data, e, 8);
        memcpy(&control, e + 8, 8);
        ctl = cpu_bytes(control, 0x700);
        dat = data ? gmem_ptr_untracked(data, 0x40) : NULL;
        magic = ctl && rd32(ctl) == 0x98761234u;
        text = ctl && rd32(ctl + 0x18) != 0;
        header = dat && rd32(dat) == 0x12345678u;
        if (header)
            type = (rd32(dat + 0x30) >> 10) & 15;
        else if (dat)
            type = 14; /* readable, another first word */
        if (!header && g_headerless_logged < 6) {
            g_headerless_logged++;
            LOG("nvn-shadow: a stage without the machine-code header: data %llx %s%08x %08x %08x %08x\n",
                (unsigned long long)data, dat ? "" : "(not mapped contiguously) ", dat ? rd32(dat) : 0,
                dat ? rd32(dat + 4) : 0, dat ? rd32(dat + 8) : 0, dat ? rd32(dat + 12) : 0);
        }
        if (ctl) code_len = rd32(ctl + 0x6f8);
        kind = ((u64)magic << 28) | ((u64)text << 24) | ((u64)header << 20) | ((u64)type << 16) |
               (code_len >> 10 < 0xFFFF ? code_len >> 10 : 0xFFFF);
        vals_add(&g_stage_kind, kind);
        g_stages++;
        if (magic) corpus_stage(data, header, type, ctl);
        if (g_headers_logged < 6 && ctl) {
            g_headers_logged++;
            LOG("nvn-shadow: stage %llu of a program: control %llx: %08x %08x %08x %08x %08x %08x %08x %08x | +0x6f0: %08x "
                "%08x %08x | data %llx: %08x %08x %08x %08x\n",
                (unsigned long long)i, (unsigned long long)control, rd32(ctl), rd32(ctl + 4), rd32(ctl + 8),
                rd32(ctl + 12), rd32(ctl + 16), rd32(ctl + 20), rd32(ctl + 24), rd32(ctl + 28), rd32(ctl + 0x6f0),
                rd32(ctl + 0x6f4), rd32(ctl + 0x6f8), (unsigned long long)data, dat ? rd32(dat) : 0,
                dat ? rd32(dat + 4) : 0, dat ? rd32(dat + 8) : 0, dat ? rd32(dat + 12) : 0);
        }
    }
    maybe_dump();
    pm_unlock(&g_lk);
}

static const struct {
    const char* name;
    NvnHandler handler;
} g_shadow[] = {
    {"nvnTextureBuilderSetDefaults", texb_defaults},
    {"nvnTextureBuilderSetSize2D", texb_size2d},
    {"nvnTextureBuilderSetLevels", texb_levels},
    {"nvnTextureInitialize", tex_init},
    {"nvnTextureBuilderSetFormat", texb_format},
    {"nvnTextureViewSetFormat", view_format},
    {"nvnTextureBuilderSetTarget", texb_target},
    {"nvnTextureViewSetTarget", view_target},
    {"nvnTextureBuilderSetSamples", texb_samples},
    {"nvnTextureBuilderSetFlags", texb_flags},
    {"nvnTextureBuilderSetDepthStencilMode", texb_dsmode},
    {"nvnTextureViewSetDepthStencilMode", view_dsmode},
    {"nvnTextureBuilderSetSize3D", texb_size3d},
    {"nvnTextureBuilderSetPackagedTextureLayout", texb_packaged},
    {"nvnVertexAttribStateSetFormat", vattr_format},
    {"nvnMemoryPoolBuilderSetFlags", pool_flags},
    {"nvnMemoryPoolBuilderSetStorage", pool_storage},
    {"nvnBufferBuilderSetStorage", buffer_storage},
    {"nvnCommandBufferAddCommandMemory", cmd_memory},
    {"nvnCommandBufferAddControlMemory", ctl_memory},
    {"nvnQueueSubmitCommands", queue_submit},
    {"nvnProgramSetShaders", prog_shaders},
    {"nvnCommandBufferBindProgram", bind_program},
    {"nvnTexturePoolInitialize", tpool_init},
    {"nvnSamplerPoolInitialize", spool_init},
    {"nvnTexturePoolRegisterTexture", reg_texture},
    {"nvnTexturePoolRegisterImage", reg_image},
    {"nvnSamplerPoolRegisterSampler", reg_sampler},
    {"nvnDeviceGetTextureHandle", tex_handle},
    {"nvnDeviceGetImageHandle", img_handle},
    {"nvnCommandBufferBindTextures", bind_textures},
    {"nvnCommandBufferBindImages", bind_images},
    {"nvnCommandBufferBindUniformBuffers", bind_ubos},
    {"nvnCommandBufferBindStorageBuffers", bind_ssbos},
    {"nvnCommandBufferBarrier", barrier},
    {"nvnCommandBufferEndRecording", end_recording},
    {"nvnQueueFenceSync", fence_queue},
    {"nvnCommandBufferFenceSync", fence_cmd},
    {"nvnQueueWaitSync", wait_sync},
    {"nvnQueueFlush", queue_flush},
    {"nvnWindowAcquireTexture", acquire},
    {"nvnQueuePresentTexture", present},
    {"nvnCommandBufferSetTexturePool", set_tpool},
    {"nvnCommandBufferSetSamplerPool", set_spool},
    {"nvnWindowBuilderSetTextures", win_textures},
    {"nvnWindowBuilderSetPresentInterval", win_interval},
    {"nvnSamplerBuilderSetMinMagFilter", smp_minmag},
    {"nvnSamplerBuilderSetWrapMode", smp_wrap},
    {"nvnSamplerBuilderSetMaxAnisotropy", smp_aniso},
    {NULL, NULL},
};
NvnHandler nvn_shadow_handler(const char* name) {
    u32 i;
    for (i = 0; g_shadow[i].name; i++)
        if (!strcmp(g_shadow[i].name, name)) return g_shadow[i].handler;
    return NULL;
}
