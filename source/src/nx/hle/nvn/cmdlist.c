/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The command set and the sizes of the data each command points at follow B2recomp's front end
   (runtime/nvn_gl.c: rec, rec_state, rec_ranges and the cmd_* handlers), GPL-3.0-or-later. */
/* Phase 3 of diagnostics/nvn-route-plan-20261029/PLAN.md, dual mode (cmdlist.h): host command lists beside the
   game's driver. A command keeps its operation, x1-x7, the low words of v0-v3 and a copy of the data it points at
   (state objects, binding arrays, clear colours, render targets), so a later replay doesn't depend on guest memory
   that the game may have changed since. At nvnQueueSubmitCommands each submitted handle is matched with its list.
   The tables print to the log every 30 seconds */
#include <stdlib.h>
#include <string.h>
#include "nx/hle/nvn/cmdlist.h"
#include "nx/gpu/vk/nvn_res.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/shader/shader.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* what a command points at: in x[ptr], count from x[count] (or fixed when count < 0) elements of size bytes */
typedef struct {
    const char* name;
    int draw;          /* a draw or dispatch, counted per submit */
    int ptr, count;    /* argument registers, 0 for none */
    u32 size;          /* bytes per element, or of the fixed block */
    int ptr2;          /* a second array with the same count (SetRenderTargets' views), 0 for none */
} CmdSpec;
static const CmdSpec g_spec[] = {
    {"nvnCommandBufferBarrier", 0, 0, 0, 0, 0},
    {"nvnCommandBufferBindBlendState", 0, 1, -1, 8, 0},
    {"nvnCommandBufferBindChannelMaskState", 0, 1, -1, 4, 0},
    {"nvnCommandBufferBindColorState", 0, 1, -1, 4, 0},
    {"nvnCommandBufferBindDepthStencilState", 0, 1, -1, 8, 0},
    {"nvnCommandBufferBindImages", 0, 4, 3, 8, 0},
    {"nvnCommandBufferBindMultisampleState", 0, 1, -1, 0x18, 0},
    {"nvnCommandBufferBindPolygonState", 0, 1, -1, 4, 0},
    {"nvnCommandBufferBindProgram", 0, 0, 0, 0, 0},
    {"nvnCommandBufferBindStorageBuffers", 0, 4, 3, 16, 0},
    {"nvnCommandBufferBindTextures", 0, 4, 3, 8, 0},
    {"nvnCommandBufferBindUniformBuffers", 0, 4, 3, 16, 0},
    {"nvnCommandBufferBindVertexAttribState", 0, 2, 1, 4, 0},
    {"nvnCommandBufferBindVertexBuffers", 0, 3, 2, 16, 0},
    {"nvnCommandBufferBindVertexStreamState", 0, 2, 1, 8, 0},
    {"nvnCommandBufferClearBuffer", 0, 0, 0, 0, 0},
    {"nvnCommandBufferClearColor", 0, 2, -1, 16, 0},
    {"nvnCommandBufferClearDepthStencil", 0, 0, 0, 0, 0},
    {"nvnCommandBufferCopyBufferToBuffer", 0, 0, 0, 0, 0},
    {"nvnCommandBufferDispatchCompute", 1, 0, 0, 0, 0},
    {"nvnCommandBufferDispatchComputeIndirect", 1, 0, 0, 0, 0},
    {"nvnCommandBufferDrawArrays", 1, 0, 0, 0, 0},
    {"nvnCommandBufferDrawElements", 1, 0, 0, 0, 0},
    {"nvnCommandBufferDrawElementsIndirect", 1, 0, 0, 0, 0},
    {"nvnCommandBufferDrawElementsInstanced", 1, 0, 0, 0, 0},
    {"nvnCommandBufferFenceSync", 0, 0, 0, 0, 0},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", 1, 0, 0, 0, 0},
    {"nvnCommandBufferRestoreZCullData", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSaveZCullData", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetAlphaRef", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetBlendColor", 0, 1, -1, 16, 0},
    {"nvnCommandBufferSetColorReductionEnable", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetColorReductionThresholds", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetDepthBounds", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetDepthClamp", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetDepthRange", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetLineWidth", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetPatchSize", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetPolygonOffsetClamp", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetRenderTargets", 0, 2, 1, 8, 3},
    {"nvnCommandBufferSetSamplerPool", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetScissor", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetShaderScratchMemory", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetStencilMask", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetStencilRef", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetStencilValueMask", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetTexturePool", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetTiledCacheAction", 0, 0, 0, 0, 0},
    {"nvnCommandBufferSetViewport", 0, 0, 0, 0, 0},
    {"nvnCommandBufferUpdateUniformBuffer", 0, 5, 4, 1, 0},
};
#define NOPS ((int)(sizeof g_spec / sizeof *g_spec))
enum { OP_BEGIN = NOPS, OP_END, OP_SUBMIT, OP_RES }; /* handled apart from the command set */

typedef struct {
    u16 op;
    u16 pad;
    u32 blob, blob_len; /* where its data is in the list's bytes */
    u64 a[7];           /* x1-x7 */
    u32 f[4];           /* the low words of v0-v3 */
} HostCmd;
typedef struct {
    u64 cmdbuf, handle; /* the command buffer it records, and the handle EndRecording returned (0 while recording) */
    int recording;
    HostCmd* cmd;
    u32 n, cap;
    u8* bytes;
    u32 nbytes, bcap;
} CmdList;
#define LISTS 32
static CmdList g_list[LISTS];
static PMutex g_lk = PMUTEX_INIT;
static int g_live; /* SNX_NVN_LIVE=1: every frame replayed, from the first submit on */
static u64 g_last_dump;
static u64 g_op_count[NOPS], g_outside[NOPS], g_lists, g_submits, g_found, g_missing, g_draws, g_bytes, g_max_cmds;
static u64 g_unreadable, g_overflow;

static const u8* cpu_bytes(u64 a, u64 n) {
    MemBlock b;
    if (!g_mem || !a || a < AS_BASE || a >= AS_END || n > AS_END - a) return NULL;
    mem_query(a, &b);
    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !b.size || a + n > b.addr + b.size) return NULL;
    return GP(a);
}
/* the list recording cmdbuf (make: a new one, replacing the oldest finished list when all are used). Lock held */
static CmdList* list_of(u64 cmdbuf, int make) {
    int i, spare = -1;
    for (i = 0; i < LISTS; i++)
        if (g_list[i].recording && g_list[i].cmdbuf == cmdbuf) return &g_list[i];
    if (!make) return NULL;
    for (i = 0; i < LISTS && spare < 0; i++)
        if (!g_list[i].recording && !g_list[i].handle) spare = i;
    for (i = 0; i < LISTS && spare < 0; i++)
        if (!g_list[i].recording) spare = i;
    if (spare < 0) return NULL;
    g_list[spare].cmdbuf = cmdbuf;
    g_list[spare].handle = 0;
    g_list[spare].recording = 1;
    g_list[spare].n = 0;
    g_list[spare].nbytes = 0;
    return &g_list[spare];
}
static int grow(void** p, u32* cap, u32 need, size_t elem) {
    u32 c = *cap ? *cap : 64;
    void* q;
    if (need <= *cap) return 1;
    while (c < need) c *= 2;
    q = realloc(*p, (size_t)c * elem);
    if (!q) return 0;
    *p = q;
    *cap = c;
    return 1;
}
/* appends n bytes at a to the list's data; their offset in *at. 0 when the guest bytes can't be read */
static int keep(CmdList* l, u64 a, u64 n, u32* at) {
    const u8* src;
    *at = l->nbytes;
    if (!n) return 1;
    if (n > 1u << 20 || !grow((void**)&l->bytes, &l->bcap, l->nbytes + (u32)n, 1)) {
        g_overflow++;
        return 0;
    }
    src = cpu_bytes(a, n);
    if (src)
        memcpy(l->bytes + l->nbytes, src, n);
    else
        memset(l->bytes + l->nbytes, 0, n);
    l->nbytes += (u32)n;
    return src != NULL;
}

/* commands release-only mode loses, per function: one warning each, so a frequent one doesn't use up the others' */
#define UNKEPT 32
static struct {
    const char* name;
    u64 n;
} g_unkept[UNKEPT];
static void res_dump(void);
static void dump(void) {
    char line[2048];
    size_t at = 0;
    int i;
    u32 recording = 0;
    for (i = 0; i < NOPS && at < sizeof line - 80; i++)
        if (g_op_count[i])
            at += (size_t)snprintf(line + at, sizeof line - at, " %s:%llu", g_spec[i].name + 16,
                                   (unsigned long long)g_op_count[i]);
    for (i = 0; i < LISTS; i++) recording += (u32)g_list[i].recording;
    LOG("nvn-cmdlist: lists %llu (recording now %u), submitted handles %llu, with a host list %llu, without %llu; "
        "draws and dispatches in submitted lists %llu (%.1f per submit); data kept %llu KiB, largest list %llu "
        "commands; unreadable pointers %llu, data overflows %llu\n",
        (unsigned long long)g_lists, recording, (unsigned long long)g_submits, (unsigned long long)g_found,
        (unsigned long long)g_missing, (unsigned long long)g_draws,
        g_found ? (double)g_draws / (double)g_found : 0.0, (unsigned long long)(g_bytes >> 10),
        (unsigned long long)g_max_cmds, (unsigned long long)g_unreadable, (unsigned long long)g_overflow);
    LOG("nvn-cmdlist: commands kept:%s\n", line);
    at = 0;
    for (i = 0; i < NOPS && at < sizeof line - 80; i++)
        if (g_outside[i])
            at += (size_t)snprintf(line + at, sizeof line - at, " %s:%llu", g_spec[i].name + 16,
                                   (unsigned long long)g_outside[i]);
    if (at) LOG("nvn-cmdlist: commands outside a recording:%s\n", line);
    for (i = 0; i < UNKEPT && g_unkept[i].name; i++)
        LOG("nvn-cmdlist: lost in release-only mode, %llu: %s\n", (unsigned long long)g_unkept[i].n, g_unkept[i].name);
    res_dump();
}
static void maybe_dump(void) { /* lock held */
    u64 now = plat_ns();
    if (!g_last_dump) g_last_dump = now;
    if (now - g_last_dump < 30000000000ull) return;
    g_last_dump = now;
    dump();
}

static int op_named(const char* name);
static u64 g_view_fmt, g_view_fmt_failed;
/* the format a texture view sets (nvnTextureViewGetFormat with its output below SP), 0 for none. Lock held */
static u32 view_format(CPU* RESTRICT c, u64 view) {
    u64 out = (c->x[31] - 256) & ~(u64)15, args[2], r = 0;
    u32 v;
    if (!view || out < AS_BASE || !cpu_bytes(view, 8)) return 0;
    args[0] = view;
    args[1] = out;
    memset(GP(out), 0, 8);
    if (!nvn_call_below(c, "nvnTextureViewGetFormat", args, 2, &r, c->x[31] - out + 64)) {
        g_view_fmt_failed++;
        return 0;
    }
    if (!(r & 0xFF)) return 0;
    memcpy(&v, GP(out), 4);
    g_view_fmt++;
    return v;
}
static u32 view_layer(CPU* RESTRICT c, u64 view) { /* the first layer a view selects (nvnTextureViewGetLayers), 0 */
    u64 out = (c->x[31] - 256) & ~(u64)15, args[3], r = 0;
    u32 v;
    if (!view || out < AS_BASE || !cpu_bytes(view, 8)) return 0;
    args[0] = view;
    args[1] = out;
    args[2] = out + 8;
    memset(GP(out), 0, 16);
    if (!nvn_call_below(c, "nvnTextureViewGetLayers", args, 3, &r, c->x[31] - out + 64) || !(r & 0xFF)) return 0;
    memcpy(&v, GP(out), 4);
    return v;
}
/* SetRenderTargets (cmd, count, textures, views, depth, depth view): after the two arrays its data holds, for each
   view (count of them, then the depth view's), the format it sets and the first layer it selects, 0 for none */
static void rt_view_formats(CPU* RESTRICT c, CmdList* l) {
    u32 n = (u32)(c->x[1] & 0xFFFFFFFFu), i, info[18];
    const u8* views = n && n <= 8 && c->x[3] ? cpu_bytes(c->x[3], (u64)n * 8) : NULL;
    if (n > 8) n = 8;
    for (i = 0; i <= n; i++) {
        u64 v = 0;
        if (i == n)
            v = c->x[5];
        else if (views)
            memcpy(&v, views + i * 8, 8);
        info[i * 2] = view_format(c, v);
        info[i * 2 + 1] = view_layer(c, v);
    }
    if (grow((void**)&l->bytes, &l->bcap, l->nbytes + (n + 1) * 8, 1)) {
        memcpy(l->bytes + l->nbytes, info, (n + 1) * 8);
        l->nbytes += (n + 1) * 8;
    }
}

/* a command: the driver records it as before; the host list gets it too */
static void cmd_record(CPU* RESTRICT c, const NvnFn* f) {
    static int op_rt = -2;
    const CmdSpec* s = &g_spec[f->op];
    HostCmd h;
    u64 cmdbuf = c->x[0], n = 0;
    int i;
    CmdList* l;
    if (op_rt == -2) op_rt = op_named("nvnCommandBufferSetRenderTargets");
    memset(&h, 0, sizeof h);
    h.op = (u16)f->op;
    for (i = 0; i < 7; i++) h.a[i] = c->x[i + 1];
    for (i = 0; i < 4; i++) memcpy(&h.f[i], &c->v[i], 4);
    pm_lock(&g_lk);
    l = list_of(cmdbuf, 0);
    if (!l)
        g_outside[f->op]++;
    else if (grow((void**)&l->cmd, &l->cap, l->n + 1, sizeof *l->cmd)) {
        if (s->ptr) { /* the second array, when there is one, follows the first */
            u32 second;
            n = s->count < 0 ? s->size : (c->x[s->count] & 0xFFFFFFFFu) * s->size;
            if (!keep(l, c->x[s->ptr], n, &h.blob) && c->x[s->ptr]) g_unreadable++;
            if (s->ptr2 && c->x[s->ptr2] && !keep(l, c->x[s->ptr2], n, &second)) g_unreadable++;
            if (f->op == op_rt) rt_view_formats(c, l);
            h.blob_len = (u32)(l->nbytes - h.blob);
        }
        l->cmd[l->n++] = h;
        g_op_count[f->op]++;
    }
    maybe_dump();
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}
static void cmd_begin(CPU* RESTRICT c, const NvnFn* f) { /* (cmdbuf) */
    u64 cmdbuf = c->x[0];
    pm_lock(&g_lk);
    if (list_of(cmdbuf, 1)) g_lists++;
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}
static void cmd_end(CPU* RESTRICT c, const NvnFn* f) { /* (cmdbuf) -> handle */
    u64 cmdbuf = c->x[0];
    CmdList* l;
    nvn_guest(c, f);
    pm_lock(&g_lk);
    l = list_of(cmdbuf, 0);
    if (l) {
        l->recording = 0;
        l->handle = c->x[0];
        g_bytes += l->nbytes;
        if (l->n > g_max_cmds) g_max_cmds = l->n;
    }
    pm_unlock(&g_lk);
}
/* ------------------------------------------------------------ the resources a frame uses (phase 3 step 3a) */
/* Memory pools and textures as the driver reports them (nvn_call on its getters after Initialize), and the ids the
   texture and sampler pools register. At submit every buffer address, texture and image handle and render target in
   the list is looked up here: what a host renderer needs to know before it can make Vulkan objects for them */
#define POOLS    256
#define TEXTURES 16384u
#define IDS      0x8000u
typedef struct {
    u64 obj, gpu, size, cpu; /* cpu: nvnMemoryPoolMap's */
    int live;
} PoolRec;
typedef struct {
    u64 obj, gpu, size;
    u32 format, width, height, target, depth; /* depth: the layers of an array */
    int live;
} TexRec;
static PoolRec g_pool[POOLS];
static TexRec g_tex[TEXTURES];
static u64 g_tex_id[IDS], g_img_id[IDS]; /* the texture object registered under each id (any texture pool) */
static u8 g_smp_id[IDS >> 6];             /* sampler ids registered: 512 at most */
static u64 g_pools_made, g_textures_made, g_getter_failed;
enum { R_BUFFER, R_TEXTURE, R_SAMPLER, R_IMAGE, R_TARGET, R_KINDS };
static const char* const g_rkind[R_KINDS] = {"buffer address", "texture handle", "sampler of a handle", "image handle",
                                             "render target"};
static u64 g_ref[R_KINDS], g_ref_ok[R_KINDS], g_ref_null[R_KINDS];
static u32 g_ref_logged[R_KINDS];

static TexRec* tex_slot(u64 obj, int make) { /* lock held */
    u32 i = (u32)(obj >> 4) & (TEXTURES - 1), n;
    for (n = 0; n < TEXTURES; n++, i = (i + 1) & (TEXTURES - 1)) {
        if (g_tex[i].obj == obj) return &g_tex[i];
        if (!g_tex[i].obj) return make ? (g_tex[i].obj = obj, &g_tex[i]) : NULL;
    }
    return NULL;
}
static int front2(void) { /* SNX_NVN_FRONTEND=2: the host path's picture, live frames and imported pools with it */
    const char* e = getenv("SNX_NVN_FRONTEND");
    return e && e[0] == '2';
}
static int import_on(void) { /* SNX_NVN_IMPORT=1 */
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("SNX_NVN_IMPORT");
        on = (e && e[0] == '1') || front2();
    }
    return on;
}
static u64 call1(CPU* RESTRICT c, const char* name, u64 a0) {
    u64 r = 0;
    if (!nvn_call(c, name, &a0, 1, &r)) g_getter_failed++;
    return r;
}
static void pool_init(CPU* RESTRICT c, const NvnFn* f) { /* nvnMemoryPoolInitialize(pool, builder) -> bool */
    u64 pool = c->x[0], builder = c->x[1], gpu, size, pool_cpu, t0 = plat_ns(), t_guest;
    int i, slot = -1;
    nvn_guest(c, f);
    t_guest = plat_ns() - t0;
    if (!(c->x[0] & 0xFF)) return;
    gpu = call1(c, "nvnMemoryPoolGetBufferAddress", pool);
    size = call1(c, "nvnMemoryPoolGetSize", pool);
    pool_cpu = call1(c, "nvnMemoryPoolMap", pool);
    if (!pool_cpu) pool_cpu = call1(c, "nvnMemoryPoolBuilderGetMemory", builder); /* a pool the CPU can't map */
    if (import_on()) { /* step 3b: the pool's storage as a Vulkan buffer, imported off this thread */
        u64 cpu = call1(c, "nvnMemoryPoolMap", pool), flags = call1(c, "nvnMemoryPoolGetFlags", pool);
        const char* e = getenv("SNX_NVN_POOL_CHECK"); /* 1 (a test): the GPU copy checked at two places, waiting */
        int n = cpu ? vk_nvn_pool_import(cpu, size) : -1, check = n >= 0 && e && e[0] == '1';
        int a = check ? vk_nvn_pool_check(n, 0, 256) : -1;
        int b = check ? vk_nvn_pool_check(n, (size / 2) & ~(u64)255, 256) : -1;
        if (n >= 0) vk_nvn_pool_gpu(n, gpu);
        if (n >= 0) vk_nvn_pool_mirror(n, flags);
        LOG("nvn-cmdlist: pool %llx: GPU %llx, %llu KiB, flags %llx, CPU %llx: import %s, GPU copy equal to guest "
            "memory at the start %s, in the middle %s; %.1f ms here, %.1f of them the game's driver\n",
            (unsigned long long)pool, (unsigned long long)gpu, (unsigned long long)(size >> 10),
            (unsigned long long)flags, (unsigned long long)cpu, n >= 0 ? "queued" : "no",
            a < 0 ? "-" : a ? "yes" : "NO", b < 0 ? "-" : b ? "yes" : "NO", (double)(plat_ns() - t0) / 1e6,
            (double)t_guest / 1e6);
    }
    pm_lock(&g_lk);
    for (i = 0; i < POOLS && slot < 0; i++)
        if (!g_pool[i].live) slot = i;
    if (slot >= 0) {
        g_pool[slot].obj = pool;
        g_pool[slot].gpu = gpu;
        g_pool[slot].size = size;
        g_pool[slot].cpu = pool_cpu;
        g_pool[slot].live = 1;
        g_pools_made++;
    }
    pm_unlock(&g_lk);
}
static void pool_fini(CPU* RESTRICT c, const NvnFn* f) { /* (pool) */
    u64 pool = c->x[0];
    int i;
    pm_lock(&g_lk);
    for (i = 0; i < POOLS; i++)
        if (g_pool[i].live && g_pool[i].obj == pool) g_pool[i].live = 0;
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}
static void tex_init(CPU* RESTRICT c, const NvnFn* f) { /* nvnTextureInitialize(texture, builder) -> bool */
    u64 tex = c->x[0], gpu, size;
    u32 format, width, height, target, depth;
    TexRec* t;
    nvn_guest(c, f);
    if (!(c->x[0] & 0xFF)) return;
    gpu = call1(c, "nvnTextureGetTextureAddress", tex);
    size = call1(c, "nvnTextureGetStorageSize", tex);
    format = (u32)call1(c, "nvnTextureGetFormat", tex);
    width = (u32)call1(c, "nvnTextureGetWidth", tex);
    height = (u32)call1(c, "nvnTextureGetHeight", tex);
    target = (u32)call1(c, "nvnTextureGetTarget", tex);
    depth = (u32)call1(c, "nvnTextureGetDepth", tex);
    pm_lock(&g_lk);
    t = tex_slot(tex, 1);
    if (t) {
        t->gpu = gpu;
        t->size = size;
        t->format = format;
        t->width = width;
        t->height = height;
        t->target = target;
        t->depth = depth ? depth : 1;
        t->live = 1;
        g_textures_made++;
    }
    if (g_live) vk_nvn_live_tex_free(gpu); /* a new texture over that memory: what the image held isn't it */
    pm_unlock(&g_lk);
}
static void tex_fini(CPU* RESTRICT c, const NvnFn* f) { /* (texture) */
    TexRec* t;
    pm_lock(&g_lk);
    t = tex_slot(c->x[0], 0);
    if (t) {
        t->live = 0;
        if (g_live) vk_nvn_live_tex_free(t->gpu);
    }
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}
/* Texture pools and the hardware descriptors (TIC) the driver writes into them: the renderer's texture code builds
   textures from exactly these (vk_tex_from_tic), so a host path can hand it the descriptor of each registered id. Each
   descriptor written is compared with the driver's own account of the texture (address, width, height) */
#define TPOOLS 16
static struct {
    u64 obj, cpu, count;
} g_tpool[TPOOLS];
static u64 g_tic_checked, g_tic_ok, g_tic_nopool, g_tic_notex;
static u32 g_tic_logged;
static int tex_known(u64 obj);
static void tpool_init(CPU* RESTRICT c, const NvnFn* f) { /* (pool, memory pool, offset, descriptors) -> bool */
    u64 pool = c->x[0], mem = c->x[1], off = c->x[2], count = c->x[3] & 0xFFFFFFFFu, cpu;
    int i;
    nvn_guest(c, f);
    if (!(c->x[0] & 0xFF)) return;
    cpu = call1(c, "nvnMemoryPoolMap", mem);
    pm_lock(&g_lk);
    for (i = 0; i < TPOOLS; i++)
        if (!g_tpool[i].obj || g_tpool[i].obj == pool) {
            g_tpool[i].obj = pool;
            g_tpool[i].cpu = cpu ? cpu + off : 0;
            g_tpool[i].count = count;
            break;
        }
    pm_unlock(&g_lk);
}
/* sampler pools the same way: their descriptors (TSC, 32 bytes) in their memory pool's storage */
static struct {
    u64 obj, cpu, count;
} g_spool[TPOOLS];
static void spool_init(CPU* RESTRICT c, const NvnFn* f) { /* (pool, memory pool, offset, descriptors) -> bool */
    u64 pool = c->x[0], mem = c->x[1], off = c->x[2], count = c->x[3] & 0xFFFFFFFFu, cpu;
    int i;
    nvn_guest(c, f);
    if (!(c->x[0] & 0xFF)) return;
    cpu = call1(c, "nvnMemoryPoolMap", mem);
    pm_lock(&g_lk);
    for (i = 0; i < TPOOLS; i++)
        if (!g_spool[i].obj || g_spool[i].obj == pool) {
            g_spool[i].obj = pool;
            g_spool[i].cpu = cpu ? cpu + off : 0;
            g_spool[i].count = count;
            break;
        }
    pm_unlock(&g_lk);
}
static void tic_check(u64 pool, u64 id, u64 tex) { /* lock held */
    const u8* p = NULL;
    const TexRec* t = tex_known(tex) ? tex_slot(tex, 0) : NULL;
    u32 tic[8], w, h, type;
    u64 gva;
    int i;
    for (i = 0; i < TPOOLS && !p; i++)
        if (g_tpool[i].obj == pool && g_tpool[i].cpu && id < g_tpool[i].count)
            p = cpu_bytes(g_tpool[i].cpu + id * 32, 32);
    if (!p) {
        g_tic_nopool++;
        return;
    }
    if (!t) {
        g_tic_notex++;
        return;
    }
    memcpy(tic, p, 32);
    gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    w = (tic[4] & 0xFFFF) + 1;
    h = (tic[5] & 0xFFFF) + 1;
    type = (tic[4] >> 23) & 0xF;
    if (type == 0 || type == 4) h = 1; /* 1D and 1D arrays, as tex_from_tic reads them */
    g_tic_checked++;
    if (gva == t->gpu && w == t->width && h == t->height)
        g_tic_ok++;
    else if (g_tic_logged < 8) {
        g_tic_logged++;
        LOG("nvn-cmdlist: descriptor %llu of pool %llx: address %llx %ux%u type %u; the driver's texture %llx: address "
            "%llx %ux%u target %u\n",
            (unsigned long long)id, (unsigned long long)pool, (unsigned long long)gva, w, h, type,
            (unsigned long long)tex, (unsigned long long)t->gpu, t->width, t->height, t->target);
    }
}
static void reg_texture(CPU* RESTRICT c, const NvnFn* f) { /* (pool, id, texture, view) */
    u64 pool = c->x[0], id = c->x[1] & 0xFFFFFFFFu, tex = c->x[2];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    if (id < IDS) g_tex_id[id] = tex;
    tic_check(pool, id, tex);
    {
        TexRec* t = tex_slot(tex, 0);
        if (t && t->live) vk_nvn_tex_registered(t->gpu);
    }
    pm_unlock(&g_lk);
}
static void reg_image(CPU* RESTRICT c, const NvnFn* f) { /* (pool, id, texture, view) */
    u64 pool = c->x[0], id = c->x[1] & 0xFFFFFFFFu, tex = c->x[2];
    nvn_guest(c, f);
    pm_lock(&g_lk);
    if (id < IDS) g_img_id[id] = tex;
    tic_check(pool, id, tex);
    pm_unlock(&g_lk);
}
static void reg_sampler(CPU* RESTRICT c, const NvnFn* f) { /* (pool, id, sampler or builder) */
    u64 id = c->x[1] & 0xFFFFFFFFu;
    nvn_guest(c, f);
    pm_lock(&g_lk);
    if (id < IDS) g_smp_id[id >> 3] |= (u8)(1u << (id & 7));
    pm_unlock(&g_lk);
}

/* the lookups, lock held */
static void ref(int kind, int ok, u64 what) {
    g_ref[kind]++;
    if (ok)
        g_ref_ok[kind]++;
    else if (g_ref_logged[kind] < 4) {
        g_ref_logged[kind]++;
        LOG("nvn-cmdlist: %s %llx not known\n", g_rkind[kind], (unsigned long long)what);
    }
}
static void ref_address(u64 a) {
    int i, ok = 0;
    if (!a) {
        g_ref_null[R_BUFFER]++;
        return;
    }
    for (i = 0; i < POOLS && !ok; i++) ok = g_pool[i].live && a - g_pool[i].gpu < g_pool[i].size;
    ref(R_BUFFER, ok, a);
}
static int tex_known(u64 obj) {
    TexRec* t = obj ? tex_slot(obj, 0) : NULL;
    return t && t->live;
}
static void ref_handle(u64 h) {
    u32 tex = (u32)(h & 0xFFFFF), smp = (u32)((h >> 20) & 0xFFF);
    if (!h) {
        g_ref_null[R_TEXTURE]++;
        return;
    }
    ref(R_TEXTURE, tex < IDS && tex_known(g_tex_id[tex]), h);
    ref(R_SAMPLER, (g_smp_id[smp >> 3] >> (smp & 7)) & 1, h);
}
static int op_named(const char* name) {
    int i;
    for (i = 0; i < NOPS; i++)
        if (!strcmp(g_spec[i].name, name)) return i;
    return -1;
}
/* ------------------------------------------------------------ programs per stage (phase 3 step 3c, part 2) */
/* Each program's stages as nvnProgramSetShaders gives them, framed as step 2 measured (SDK 13.3.2): a graphics stage
   is its 0x50-byte header at data+0x30 and control+0x6f8 bytes of instructions, its constant buffer 1 at data + word 4
   of control+0x6f0; a compute stage (control+0x714 = 5, or no 0x12345678 header) starts at data+0x100 after a
   0x12345679 header (constant data at data + word 5), else at data (data + word 4). The first time a submitted list
   binds a program its stages are translated live and loaded (vk_nvn_shader) */
#define PROGRAMS 8192u
#define VARIANTS 6 /* translations of a stage for the keys draws need (NvnStageKey) */
typedef struct {
    int stage, compute;
    u64 code, cb;
    u32 window, cb_size, cs_local[3], shared;
    int mod; /* -2 not yet, -1 failed, else the module (no key: what its translation reads and writes) */
    int nvar;
    struct {
        NvnStageKey key;
        int mod;
    } var[VARIANTS];
} ProgStage;
typedef struct {
    u64 obj;
    int n;
    ProgStage st[6];
} ProgRec;
static ProgRec g_prog[PROGRAMS];
static u64 g_mod_ok[6], g_mod_fail[6];
static char g_mod_why[16][64];
static u64 g_mod_why_n[16];
static ProgRec* prog_slot(u64 obj, int make) { /* lock held */
    u32 i = (u32)(obj >> 4) & (PROGRAMS - 1), n;
    for (n = 0; n < PROGRAMS; n++, i = (i + 1) & (PROGRAMS - 1)) {
        if (g_prog[i].obj == obj) return &g_prog[i];
        if (!g_prog[i].obj) return make ? (g_prog[i].obj = obj, &g_prog[i]) : NULL;
    }
    return NULL;
}
static u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static void prog_shaders(CPU* RESTRICT c, const NvnFn* f) { /* (program, count, NVNshaderData*) */
    static const int stage_of[6] = {-1, ST_VS, ST_TCS, ST_TES, ST_GS, ST_FS};
    u64 prog = c->x[0], count = c->x[1] & 0xFFFFFFFFu, list = c->x[2], i;
    ProgRec rec;
    memset(&rec, 0, sizeof rec);
    for (i = 0; i < count && i < 6; i++) {
        const u8* e = cpu_bytes(list + i * 16, 16);
        const u8 *ctl, *dat;
        u64 data, control;
        ProgStage* s = &rec.st[rec.n];
        u32 w4, w5, len;
        int header;
        if (!e) break;
        memcpy(&data, e, 8);
        memcpy(&control, e + 8, 8);
        ctl = cpu_bytes(control, 0x760);
        dat = data ? gmem_ptr_untracked(data, 0x40) : NULL;
        if (!ctl || !dat || rd32(ctl) != 0x98761234u) continue;
        header = rd32(dat) == 0x12345678u;
        len = rd32(ctl + 0x6f8);
        w4 = rd32(ctl + 0x700);
        w5 = rd32(ctl + 0x704);
        s->cb_size = rd32(ctl + 0x6fc);
        s->mod = -2;
        s->compute = !header || rd32(ctl + 0x714) == 5;
        if (s->compute) {
            int cs_header = rd32(dat) == 0x12345679u;
            s->stage = ST_CS;
            s->code = data + (cs_header ? 0x100 : 0);
            s->window = len;
            s->cb = data + (cs_header ? w5 : w4);
            s->cs_local[0] = rd32(ctl + 0x748);
            s->cs_local[1] = rd32(ctl + 0x74c);
            s->cs_local[2] = rd32(ctl + 0x750);
            s->shared = rd32(ctl + 0x754);
        } else {
            u32 type = (rd32(dat + 0x30) >> 10) & 15;
            if (type >= 6 || stage_of[type] < 0) continue;
            s->stage = stage_of[type];
            s->code = data + 0x30;
            s->window = 0x50 + len;
            s->cb = data + w4;
        }
        if (!s->cb_size) s->cb = 0;
        rec.n++;
    }
    nvn_guest(c, f);
    for (i = 0; i < (u64)rec.n; i++)
        vk_nvn_prep_add(rec.st[i].stage, rec.st[i].code, rec.st[i].window, rec.st[i].cb, rec.st[i].cb_size,
                        rec.st[i].compute ? rec.st[i].cs_local : NULL, rec.st[i].shared);
    pm_lock(&g_lk);
    {
        ProgRec* p = prog_slot(prog, 1);
        if (p) { /* a program given new shaders is translated again */
            rec.obj = prog;
            *p = rec;
        }
    }
    pm_unlock(&g_lk);
}
static void why_count(char (*why)[64], u64* n, const char* what) { /* a table of 16 reasons and their counts */
    u32 k;
    for (k = 0; k < 16 && n[k] && strcmp(why[k], what); k++) {}
    if (k < 16) {
        if (!n[k]) snprintf(why[k], sizeof why[k], "%s", what);
        n[k]++;
    }
}
static u64 g_var_made, g_var_fail, g_var_full;
static int stage_load(ProgStage* s, const NvnStageKey* key) { /* lock held: s translated for key, -1 if it can't be */
    char why[64];
    int mod = vk_nvn_shader(s->stage, s->code, s->window, s->compute, s->cb, s->cb_size, s->cs_local, s->shared, key,
                            why, sizeof why);
    if (mod < 0) why_count(g_mod_why, g_mod_why_n, why);
    return mod;
}
static void prog_translate(u64 obj) { /* lock held: the stages of a bound program, the first time */
    ProgRec* p = prog_slot(obj, 0);
    int i;
    if (!p) return;
    for (i = 0; i < p->n; i++) {
        ProgStage* s = &p->st[i];
        if (s->mod != -2) continue;
        s->mod = stage_load(s, NULL);
        if (s->mod >= 0)
            g_mod_ok[s->stage]++;
        else
            g_mod_fail[s->stage]++;
    }
}
/* the module of s for a draw's key: the one made for it before, or a new translation */
static int stage_variant(ProgStage* s, const NvnStageKey* key) { /* lock held */
    int i;
    if (s->mod < 0) return -1;
    for (i = 0; i < s->nvar; i++)
        if (!memcmp(&s->var[i].key, key, sizeof *key)) return s->var[i].mod;
    if (s->nvar == VARIANTS) { /* full: the oldest goes (its module stays loaded) */
        memmove(&s->var[0], &s->var[1], sizeof s->var[0] * (VARIANTS - 1));
        s->nvar--;
        g_var_full++;
    }
    s->var[s->nvar].key = *key;
    s->var[s->nvar].mod = stage_load(s, key);
    if (s->var[s->nvar].mod >= 0)
        g_var_made++;
    else
        g_var_fail++;
    return s->var[s->nvar++].mod;
}
static void prog_dump(void) { /* lock held */
    static const char* const names[6] = {"vertex", "tess control", "tess eval", "geometry", "fragment", "compute"};
    int i;
    for (i = 0; i < 6; i++)
        if (g_mod_ok[i] || g_mod_fail[i])
            LOG("nvn-cmdlist: %s stages loaded as modules %llu, not %llu\n", names[i], (unsigned long long)g_mod_ok[i],
                (unsigned long long)g_mod_fail[i]);
    for (i = 0; i < 16 && g_mod_why_n[i]; i++)
        LOG("nvn-cmdlist:   not loaded, %llu: %s\n", (unsigned long long)g_mod_why_n[i], g_mod_why[i]);
    LOG("nvn-cmdlist: stage variants for draw keys made %llu, failed %llu, replaced when full %llu\n",
        (unsigned long long)g_var_made, (unsigned long long)g_var_fail, (unsigned long long)g_var_full);
}

/* ------------------------------------------------------------ pipeline state (phase 3 step 3c, part 1) */
/* The state objects a frame binds, decoded through the driver's own getters: what a host draw path builds its
   pipelines from. A copy (the bytes kept at bind time) goes below the guest SP with places for the getters' outputs,
   and each distinct object (by its bytes) is decoded once into one packed value */
enum { S_BLEND, S_COLOR, S_DEPTH, S_POLYGON, S_CHANMASK, S_MULTISAMPLE, S_VATTR, S_VSTREAM, S_KINDS };
static const char* const g_skind[S_KINDS] = {"blend",        "color",       "depth-stencil",    "polygon",
                                             "channel mask", "multisample", "vertex attribute", "vertex stream"};
#define SEEN  4096u
#define SVALS 32
static u64 g_seen[S_KINDS][SEEN];     /* hashes of the objects decoded, 0 for none */
static u64 g_seen_val[S_KINDS][SEEN]; /* and their values */
static u8 g_seen_ok[S_KINDS][SEEN];   /* 1 decoded, 0 a getter failed */
static u64 g_state_new[S_KINDS], g_state_fail[S_KINDS];    /* distinct objects, and those a getter failed for */
static u64 g_sval[S_KINDS][SVALS], g_sval_n[S_KINDS][SVALS]; /* decoded values and how many objects had each */
static u32 g_sval_used[S_KINDS], g_seen_full;
#define SCRATCH 2048u /* below SP: the copy at the start, the outputs at +0x100 */
/* the slot of the object with these bytes, *fresh when new; -1 when the table is full. Lock held */
static int seen_slot(int kind, const u8* p, u32 n, int* fresh) {
    u64 h = 1469598103934665603ull ^ (u64)kind;
    u32 i, k;
    *fresh = 0;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    if (!h) h = 1;
    for (k = (u32)h & (SEEN - 1), i = 0; i < SEEN; i++, k = (k + 1) & (SEEN - 1)) {
        if (g_seen[kind][k] == h) return (int)k;
        if (!g_seen[kind][k]) {
            g_seen[kind][k] = h;
            *fresh = 1;
            return (int)k;
        }
    }
    g_seen_full++;
    return -1;
}
static u64 g_scratch; /* the guest address of the scratch area for the call being made */
static int get(CPU* RESTRICT c, const char* name, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64* r) {
    u64 args[6] = {g_scratch, a1, a2, a3, a4, a5};
    return nvn_call_below(c, name, args, 6, r, c->x[31] - g_scratch + 64);
}
static u32 out32(int i) { /* output place i, written by the last getter */
    u32 v;
    memcpy(&v, GP(g_scratch + 0x100 + (u64)i * 8), 4);
    return v;
}
#define SLOT(i) (g_scratch + 0x100 + (u64)(i) * 8)
/* the decoded value of a state object (the bytes kept at its bind) in *out: 1, or 0 when a getter failed for it */
static int state_decode(CPU* RESTRICT c, int kind, const u8* p, u32 n, u64* out) { /* lock held */
    u64 v = 0, r = 0;
    int ok = 1, i, fresh, slot = seen_slot(kind, p, n, &fresh);
    u32 k;
    *out = 0;
    if (slot < 0) return 0;
    if (!fresh) return *out = g_seen_val[kind][slot], g_seen_ok[kind][slot];
    g_state_new[kind]++;
    g_scratch = (c->x[31] - SCRATCH) & ~(u64)15;
    if (g_scratch < AS_BASE) return 0;
    memcpy(GP(g_scratch), p, n);
    memset(GP(SLOT(0)), 0, 64);
    switch (kind) {
        case S_BLEND: /* target, then funcs src/dst RGB and alpha, then equations RGB and alpha: a byte each */
            ok = get(c, "nvnBlendStateGetBlendTarget", 0, 0, 0, 0, 0, &r) &&
                 get(c, "nvnBlendStateGetBlendFunc", SLOT(0), SLOT(1), SLOT(2), SLOT(3), 0, &v) &&
                 get(c, "nvnBlendStateGetBlendEquation", SLOT(4), SLOT(5), 0, 0, 0, &v);
            v = (r & 7) << 48 | (u64)(out32(0) & 0xFF) << 40 | (u64)(out32(1) & 0xFF) << 32 |
                (u64)(out32(2) & 0xFF) << 24 | (u64)(out32(3) & 0xFF) << 16 | (u64)(out32(4) & 0xFF) << 8 |
                (out32(5) & 0xFF);
            break;
        case S_COLOR: /* blend enable per target, logic op, alpha test */
            for (i = 0; i < 8 && ok; i++) {
                ok = get(c, "nvnColorStateGetBlendEnable", (u64)i, 0, 0, 0, 0, &r);
                v |= (u64)(r & 1) << i;
            }
            if (ok) ok = get(c, "nvnColorStateGetLogicOp", 0, 0, 0, 0, 0, &r);
            v |= (r & 31) << 8;
            if (ok) ok = get(c, "nvnColorStateGetAlphaTest", 0, 0, 0, 0, 0, &r);
            v |= (r & 15) << 16;
            break;
        case S_DEPTH: /* test, write, func, stencil test, front stencil func */
            ok = get(c, "nvnDepthStencilStateGetDepthTestEnable", 0, 0, 0, 0, 0, &r);
            v = r & 1;
            if (ok) ok = get(c, "nvnDepthStencilStateGetDepthWriteEnable", 0, 0, 0, 0, 0, &r);
            v |= (r & 1) << 1;
            if (ok) ok = get(c, "nvnDepthStencilStateGetDepthFunc", 0, 0, 0, 0, 0, &r);
            v |= (r & 15) << 4;
            if (ok) ok = get(c, "nvnDepthStencilStateGetStencilTestEnable", 0, 0, 0, 0, 0, &r);
            v |= (r & 1) << 8;
            if (ok) ok = get(c, "nvnDepthStencilStateGetStencilFunc", 1, 0, 0, 0, 0, &r); /* NVN_FACE_FRONT */
            v |= (r & 15) << 12;
            if (ok) ok = get(c, "nvnDepthStencilStateGetStencilFunc", 2, 0, 0, 0, 0, &r); /* NVN_FACE_BACK */
            v |= (r & 15) << 16;
            for (i = 0; i < 2 && ok; i++) { /* fail, depth fail, pass of each face, 4 bits each from bit 20 */
                ok = get(c, "nvnDepthStencilStateGetStencilOp", (u64)i + 1, SLOT(0), SLOT(1), SLOT(2), 0, &r);
                v |= (u64)((out32(0) & 15) | (out32(1) & 15) << 4 | (out32(2) & 15) << 8) << (20 + 12 * i);
            }
            break;
        case S_POLYGON: /* cull face, front face, polygon mode, offset enables */
            ok = get(c, "nvnPolygonStateGetCullFace", 0, 0, 0, 0, 0, &r);
            v = r & 15;
            if (ok) ok = get(c, "nvnPolygonStateGetFrontFace", 0, 0, 0, 0, 0, &r);
            v |= (r & 15) << 4;
            if (ok) ok = get(c, "nvnPolygonStateGetPolygonMode", 0, 0, 0, 0, 0, &r);
            v |= (r & 15) << 8;
            if (ok) ok = get(c, "nvnPolygonStateGetPolygonOffsetEnables", 0, 0, 0, 0, 0, &r);
            v |= (r & 15) << 12;
            break;
        case S_CHANMASK: /* r, g, b, a per target, 4 bits each */
            for (i = 0; i < 8 && ok; i++) {
                ok = get(c, "nvnChannelMaskStateGetChannelMask", (u64)i, SLOT(0), SLOT(1), SLOT(2), SLOT(3), &r);
                v |= (u64)((out32(0) & 1) | (out32(1) & 1) << 1 | (out32(2) & 1) << 2 | (out32(3) & 1) << 3) << (i * 4);
            }
            break;
        case S_MULTISAMPLE: /* samples, alpha to coverage, multisample enable */
            ok = get(c, "nvnMultisampleStateGetSamples", 0, 0, 0, 0, 0, &r);
            v = r & 0xFF;
            if (ok) ok = get(c, "nvnMultisampleStateGetAlphaToCoverageEnable", 0, 0, 0, 0, 0, &r);
            v |= (r & 1) << 8;
            if (ok) ok = get(c, "nvnMultisampleStateGetMultisampleEnable", 0, 0, 0, 0, 0, &r);
            v |= (r & 1) << 9;
            break;
        case S_VATTR: /* format, relative offset, stream */
            ok = get(c, "nvnVertexAttribStateGetFormat", SLOT(0), SLOT(1), 0, 0, 0, &r);
            v = (u64)(out32(0) & 0xFF) | (u64)(out32(1) & 0xFFFF) << 8;
            if (ok) ok = get(c, "nvnVertexAttribStateGetStreamIndex", 0, 0, 0, 0, 0, &r);
            v |= (r & 31) << 24;
            break;
        case S_VSTREAM: /* stride, divisor */
            ok = get(c, "nvnVertexStreamStateGetStride", 0, 0, 0, 0, 0, &r);
            v = r & 0xFFFF;
            if (ok) ok = get(c, "nvnVertexStreamStateGetDivisor", 0, 0, 0, 0, 0, &r);
            v |= (r & 0xFFFF) << 16;
            break;
    }
    g_seen_val[kind][slot] = v;
    g_seen_ok[kind][slot] = (u8)ok;
    *out = v;
    if (!ok) {
        g_state_fail[kind]++;
        return 0;
    }
    for (k = 0; k < g_sval_used[kind] && g_sval[kind][k] != v; k++) {}
    if (k < g_sval_used[kind])
        g_sval_n[kind][k]++;
    else if (k < SVALS) {
        g_sval[kind][k] = v;
        g_sval_n[kind][k] = 1;
        g_sval_used[kind]++;
    }
    return 1;
}
static void state_dump(void) { /* lock held */
    char line[1200];
    int kind;
    u32 k;
    for (kind = 0; kind < S_KINDS; kind++) {
        size_t at = 0;
        if (!g_state_new[kind]) continue;
        for (k = 0; k < g_sval_used[kind] && at < sizeof line - 48; k++)
            at += (size_t)snprintf(line + at, sizeof line - at, " %llx:%llu", (unsigned long long)g_sval[kind][k],
                                   (unsigned long long)g_sval_n[kind][k]);
        LOG("nvn-cmdlist: %s states: %llu distinct, getters failed for %llu; decoded values%s:%s\n", g_skind[kind],
            (unsigned long long)g_state_new[kind], (unsigned long long)g_state_fail[kind],
            g_sval_used[kind] == SVALS ? " (first 32)" : "", line);
    }
}
/* ------------------------------------------------------------ pipelines per draw (phase 3 step 3c, part 2b) */
/* The state the submitted lists set, followed command by command across lists (the queue's state carries on), and at
   each draw or dispatch the Vulkan pipeline it needs: the bound program of each stage translated for the draw's key,
   the decoded state objects, and the formats of the render targets (a view's format when it sets one, else the
   texture's from the driver). Counted per draw and per frame (nvnQueuePresentTexture) */
typedef struct {
    u64 prog[6]; /* the program bound to each stage (ST_*), 0 for none */
    u64 blend[8], color, chanmask, polygon, multisample;
    u64 vattr[16], vstream[16];
    u32 nattr, nstream;
    u32 bad; /* bit per state kind: a bound object a getter failed for */
    u32 ncolor, color_fmt[8], depth_fmt, rt_unknown;
    u32 patch, depth_clamp;
    /* the bindings by NVNshaderStage (0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess eval, 5 compute), and
       the texture and sampler pools set */
    NvnRange ubo[6][16], ssbo[6][16];
    u64 tex[6][32], img[6][8];
    u64 tpool, spool;
    /* what a replay draws with besides (part 3): the render targets, depth-stencil value, vertex buffers, and the
       dynamic state the commands set */
    NvnTarget rt[8], zt;
    u64 depth;
    NvnRange vbuf[16];
    i32 viewport[4], scissor[4];
    float depth_range[2], alpha_ref, blend_color[4], line_width, offset[3];
    u32 stencil_ref[2], stencil_wmask[2], stencil_vmask[2];
    u32 depth_bounds_on;
    float depth_bounds[2];
} NvnDrawState;
/* nvnDeviceSetWindowOriginMode (1 upper left) and nvnDeviceSetDepthMode (1 near is zero), and the window's textures
   (nvnWindowBuilderSetTextures), by present index */
static u32 g_upper_left, g_depth_zero;
static u64 g_win_tex[8];
static u32 g_nwin_tex;
/* SNX_NVN_REPLAY=frame: that frame's lists replayed (nvn_res.c), its picture in SNX_NVN_REPLAY_OUT.ppm */
static u64 g_replay_frame = ~0ull;
static int g_replaying;
static char g_live_out[600]; /* SNX_NVN_LIVE_OUT: path prefix of the pictures saved every 600 presents */
/* nvn_capture_frame: g_cap_req set by the caller, taken at the next present, which arms the frame after it */
static char g_cap_out[600];
static int g_cap_req;
static u64 g_cap_frame;
/* the clip distances a vertex pipeline's last stage clips with (SNX_NVN_CLIP): the driver sets SetUserClipEnable to
   0xFF, seen on every draw of the route, so whatever a program writes clips */
static u32 g_clip_on = 0xFF;
static char g_replay_out[600];
static NvnDrawState g_ds;
static int g_ds_set;
/* descriptors (part 2c): bindings resolved and not by kind (NVN_B_*), and draws with every one resolved */
static u64 g_db_ok[NVN_B_KINDS], g_db_fail[NVN_B_KINDS], g_db_draws, g_db_complete;
static char g_db_why[16][64];
static u64 g_db_why_n[16];
static void db_why(const char* why) { why_count(g_db_why, g_db_why_n, why); }
static int nvn_stage_of(int stage) { /* ST_* -> NVNshaderStage */
    static const int m[6] = {0, 3, 4, 2, 1, 5};
    return m[stage];
}
/* what stage s binds now, with the program constants of ps */
static void stage_bind(const ProgStage* ps, NvnStageBind* b) { /* lock held */
    int n = nvn_stage_of(ps->stage), i;
    memset(b, 0, sizeof *b);
    b->nvn_stage = n;
    b->cb1 = ps->cb;
    b->cb1_size = ps->cb_size;
    memcpy(b->ubo, g_ds.ubo[n], sizeof b->ubo);
    memcpy(b->ssbo, g_ds.ssbo[n], sizeof b->ssbo);
    memcpy(b->tex, g_ds.tex[n], sizeof b->tex);
    memcpy(b->img, g_ds.img[n], sizeof b->img);
    for (i = 0; i < TPOOLS; i++) {
        if (g_ds.tpool && g_tpool[i].obj == g_ds.tpool) b->tex_pool = g_tpool[i].cpu, b->tex_count = g_tpool[i].count;
        if (g_ds.spool && g_spool[i].obj == g_ds.spool) b->smp_pool = g_spool[i].cpu, b->smp_count = g_spool[i].count;
    }
}
/* the replay's view of a draw (part 3): its dynamic state, and the call from its command's arguments */
static void replay_dyn(NvnDrawDyn* y) { /* lock held */
    u64 v = g_ds.depth;
    u32 i, f;
    memset(y, 0, sizeof *y);
    memcpy(y->viewport, g_ds.viewport, sizeof y->viewport);
    memcpy(y->scissor, g_ds.scissor, sizeof y->scissor);
    memcpy(y->depth_range, g_ds.depth_range, sizeof y->depth_range);
    y->upper_left = g_upper_left;
    y->depth_zero = g_depth_zero;
    y->cull = (u32)g_ds.polygon & 15;
    y->front_ccw = (u32)(g_ds.polygon >> 4) & 15;
    y->offset_enables = (u32)(g_ds.polygon >> 12) & 15;
    y->depth_test = (u32)v & 1;
    y->depth_write = (u32)(v >> 1) & 1;
    y->depth_func = (u32)(v >> 4) & 15;
    y->stencil_test = (u32)(v >> 8) & 1;
    y->stencil_func[0] = (u32)(v >> 12) & 15;
    y->stencil_func[1] = (u32)(v >> 16) & 15;
    for (f = 0; f < 2; f++)
        for (i = 0; i < 3; i++) y->stencil_op[f][i] = (u32)(v >> (20 + 12 * f + 4 * i)) & 15;
    memcpy(y->stencil_ref, g_ds.stencil_ref, sizeof y->stencil_ref);
    memcpy(y->stencil_wmask, g_ds.stencil_wmask, sizeof y->stencil_wmask);
    memcpy(y->stencil_vmask, g_ds.stencil_vmask, sizeof y->stencil_vmask);
    memcpy(y->offset, g_ds.offset, sizeof y->offset);
    memcpy(y->blend_color, g_ds.blend_color, sizeof y->blend_color);
    y->line_width = g_ds.line_width;
    y->alpha_func = (u32)(g_ds.color >> 16) & 15;
    y->alpha_ref = g_ds.alpha_ref;
    y->depth_bounds_on = g_ds.depth_bounds_on;
    memcpy(y->depth_bounds, g_ds.depth_bounds, sizeof y->depth_bounds);
    memcpy(y->vbuf, g_ds.vbuf, sizeof y->vbuf);
    for (i = 0; i < g_ds.nstream && i < 16; i++) y->stride[i] = (u32)g_ds.vstream[i] & 0xFFFF;
}
static void stage_bind(const ProgStage* ps, NvnStageBind* b);
static void replay_draw(const HostCmd* h, const NvnPipeDesc* d, ProgStage* const* st) { /* lock held */
    const char* nm = g_spec[h->op].name + 16; /* after "nvnCommandBuffer" */
    NvnStageBind bind[5];
    NvnDrawDyn y;
    NvnDrawCall call;
    int s;
    memset(bind, 0, sizeof bind);
    for (s = 0; s < 5; s++)
        if (st[s]) stage_bind(st[s], &bind[s]);
    memset(&call, 0, sizeof call);
    call.primitive = (u32)h->a[0];
    call.index_type = (u32)h->a[1];
    call.instances = 1;
    if (!strcmp(nm, "DrawArrays")) { /* (cmd, mode, first, count) */
        call.kind = 0;
        call.first = (u32)h->a[1];
        call.count = (u32)h->a[2];
    } else if (!strcmp(nm, "DrawElements") || !strcmp(nm, "DrawElementsInstanced")) {
        /* (cmd, mode, type, count, index buffer[, base vertex, base instance, instances]) */
        call.kind = 1;
        call.count = (u32)h->a[2];
        call.index = h->a[3];
        if (nm[12]) {
            call.base_vertex = (i32)h->a[4];
            call.base_instance = (u32)h->a[5];
            call.instances = (u32)h->a[6];
        }
    } else if (!strcmp(nm, "DrawElementsIndirect")) { /* (cmd, mode, type, index buffer, indirect buffer) */
        call.kind = 2;
        call.index = h->a[2];
        call.indirect = h->a[3];
    } else if (!strcmp(nm, "MultiDrawElementsIndirectCount")) {
        /* (cmd, mode, type, index, indirect, parameter, max draws, stride) */
        call.kind = 3;
        call.index = h->a[2];
        call.indirect = h->a[3];
        call.parameter = h->a[4];
        call.max_draws = (u32)h->a[5];
        call.stride = (u32)h->a[6];
    } else {
        vk_nvn_replay_skip("a draw the replay doesn't know");
        return;
    }
    replay_dyn(&y);
    vk_nvn_replay_draw(d, bind, &y, &call);
}
/* a binding command (cmd, stage, first, count, array): into the state's table of that kind */
static void ds_bind(const HostCmd* h, const u8* blob, int kind) {
    u32 stage = (u32)h->a[0] % 6, first = (u32)h->a[1], n = (u32)h->a[2], i;
    for (i = 0; i < n; i++) {
        u32 at = first + i;
        if (kind == NVN_B_UBO && at < 16 && (i + 1) * 16 <= h->blob_len)
            memcpy(&g_ds.ubo[stage][at], blob + i * 16, 16);
        else if (kind == NVN_B_SSBO && at < 16 && (i + 1) * 16 <= h->blob_len)
            memcpy(&g_ds.ssbo[stage][at], blob + i * 16, 16);
        else if (kind == NVN_B_TEX && at < 32 && (i + 1) * 8 <= h->blob_len)
            memcpy(&g_ds.tex[stage][at], blob + i * 8, 8);
        else if (kind == NVN_B_IMG && at < 8 && (i + 1) * 8 <= h->blob_len)
            memcpy(&g_ds.img[stage][at], blob + i * 8, 8);
    }
}
static void draw_descriptors(ProgStage* const* st, const int* mods, int n) { /* lock held */
    int s;
    u32 fails = 0;
    g_db_draws++;
    for (s = 0; s < n; s++) {
        NvnStageBind b;
        if (!st[s] || mods[s] < 0) continue;
        stage_bind(st[s], &b);
        fails += vk_nvn_stage_check(mods[s], &b, g_db_ok, g_db_fail, db_why);
    }
    g_db_complete += !fails;
}
static u64 g_pd_draws, g_pd_ok, g_pd_made, g_pc_disp, g_pc_ok, g_pc_made;
static char g_pd_why[16][64];
static u64 g_pd_why_n[16];
/* per frame: made and failed now, and over the frames presented */
static u64 g_fr_made, g_fr_fail, g_fr_draws, g_fr_pipe_ns, g_pipe_ns, g_pipe_worst, g_pipe_long;
static u64 g_frames, g_frames_making, g_frames_failing, g_fr_max_made, g_fr_hist[4];
static u32 g_fr_logged;
static void ds_reset(void) { /* NVN's defaults: blending ONE, ZERO, ADD; every channel written; fill */
    u32 i;
    memset(&g_ds, 0, sizeof g_ds);
    for (i = 0; i < 8; i++) g_ds.blend[i] = (u64)i << 48 | 2ull << 40 | 1ull << 32 | 2u << 24 | 1u << 16 | 1u << 8 | 1u;
    g_ds.color = 3u << 8; /* logic op copy */
    g_ds.chanmask = 0xFFFFFFFFu;
    g_ds.polygon = 2u << 8;
    g_ds.depth_range[1] = 1.0f;
    g_ds.scissor[2] = -1; /* none set */
    g_ds.stencil_wmask[0] = g_ds.stencil_wmask[1] = g_ds.stencil_vmask[0] = g_ds.stencil_vmask[1] = 0xFF;
    g_ds.line_width = 1.0f;
    g_ds_set = 1;
}
#define FAIL(why)                                                                                                      \
    do {                                                                                                               \
        pd_fail(why);                                                                                                  \
        return;                                                                                                        \
    } while (0)
static void pd_fail(const char* why) {
    g_fr_fail++;
    why_count(g_pd_why, g_pd_why_n, why);
}
static ProgStage* bound_stage(int stage, int* err) { /* lock held: NULL for none, or with *err when not known */
    ProgRec* p;
    int i;
    *err = 0;
    if (!g_ds.prog[stage]) return NULL;
    p = prog_slot(g_ds.prog[stage], 0);
    if (!p) return *err = 1, NULL;
    for (i = 0; i < p->n; i++)
        if (p->st[i].stage == stage) return &p->st[i];
    return NULL; /* bound for a stage it has no shader for: the stage is off */
}
static void draw_pipeline(const HostCmd* h) { /* lock held: a draw command, its first argument the primitive */
    u32 primitive = (u32)h->a[0];
    static const int gfx[5] = {ST_VS, ST_TCS, ST_TES, ST_GS, ST_FS};
    NvnPipeDesc d;
    NvnStageKey key[5];
    ProgStage* st[5];
    u32 io_in[5], io_out[5], io_frag[5], i, last_out = 0;
    int s, err, made = 0;
    u64 t0;
    char why[64];
    g_pd_draws++;
    g_fr_draws++;
    if (g_ds.bad) FAIL("a state object not decoded");
    if (g_ds.rt_unknown) FAIL("a render target not known");
    memset(key, 0, sizeof key);
    for (s = 0; s < 5; s++) {
        st[s] = bound_stage(gfx[s], &err);
        if (err) FAIL("bound program not recorded");
        io_in[s] = io_out[s] = io_frag[s] = 0;
        if (!st[s]) continue;
        if (st[s]->mod < 0) FAIL("a bound stage not translated");
        vk_nvn_shader_io(st[s]->mod, &io_in[s], &io_out[s], &io_frag[s]);
        if (s < 4) last_out = io_out[s];
    }
    if (!st[0]) FAIL("no vertex program");
    for (i = 0; i < g_ds.nattr && i < 16; i++) { /* the vertex program's integer attributes */
        u32 t = vk_nvn_attr_int((u32)g_ds.vattr[i] & 0xFF);
        if (!(io_in[0] >> i & 1) || !t) continue;
        key[0].int_mask |= 1u << i;
        if (t == 3) key[0].sint_mask |= 1u << i;
    }
    key[0].clip = st[1] || st[2] || st[3] ? SNX_SH_NOT_LAST : g_clip_on;
    key[1].clip = SNX_SH_NOT_LAST;
    key[2].clip = st[3] ? SNX_SH_NOT_LAST : g_clip_on;
    key[3].clip = g_clip_on;
    for (i = 0; i < g_ds.ncolor && io_frag[4]; i++)
        if (g_ds.color_fmt[i] && vk_nvn_color_int(g_ds.color_fmt[i])) {
            key[4].rt_int |= 1u << i;
            if (vk_nvn_attr_int(g_ds.color_fmt[i]) == 3) key[4].rt_sint |= 1u << i;
        }
    key[4].unfed = io_in[4] & ~last_out;
    if (st[4]) { /* the fragment program's integer and depth-compared colour textures, as bound */
        NvnStageBind b;
        stage_bind(st[4], &b);
        vk_nvn_tex_key(st[4]->mod, &b, &key[4]);
    }
    memset(&d, 0, sizeof d);
    t0 = plat_ns();
    for (s = 0; s < 5; s++) {
        d.mod[s] = st[s] ? stage_variant(st[s], &key[s]) : -1;
        if (st[s] && d.mod[s] < 0) FAIL("a stage not translated for the draw's key");
    }
    g_fr_pipe_ns += plat_ns() - t0;
    if (!g_live) draw_descriptors(st, d.mod, 5);
    d.primitive = primitive;
    d.patch = g_ds.patch;
    d.polygon = (u32)(g_ds.polygon >> 8) & 15;
    d.depth_clamp = g_ds.depth_clamp;
    d.samples = (u32)g_ds.multisample & 0xFF;
    if (!(g_ds.multisample >> 9 & 1) && d.samples) d.samples = 0; /* multisampling off */
    d.logic_op = (u32)(g_ds.color >> 8) & 31;
    d.nattr = g_ds.nattr;
    for (i = 0; i < g_ds.nattr && i < 16; i++) {
        d.attr_format[i] = (u32)g_ds.vattr[i] & 0xFF;
        d.attr_offset[i] = (u32)(g_ds.vattr[i] >> 8) & 0xFFFF;
        d.attr_stream[i] = (u32)(g_ds.vattr[i] >> 24) & 31;
    }
    d.nstream = g_ds.nstream;
    for (i = 0; i < g_ds.nstream && i < 16; i++) d.stream_divisor[i] = (u32)(g_ds.vstream[i] >> 16) & 0xFFFF;
    d.ncolor = g_ds.ncolor;
    memcpy(d.color_format, g_ds.color_fmt, sizeof d.color_format);
    d.depth_format = g_ds.depth_fmt;
    d.blend_on = (u32)g_ds.color & 0xFF;
    for (i = 0; i < 8; i++) {
        u64 b = g_ds.blend[i];
        d.blend[i][0] = (u8)(b >> 40);
        d.blend[i][1] = (u8)(b >> 32);
        d.blend[i][2] = (u8)(b >> 24);
        d.blend[i][3] = (u8)(b >> 16);
        d.blend[i][4] = (u8)(b >> 8);
        d.blend[i][5] = (u8)b;
    }
    d.chanmask = (u32)g_ds.chanmask;
    /* depth test on, NVN_DEPTH_FUNC_EQUAL (3): the colour pass of a depth prepass */
    d.fade_cut = d.depth_format && (g_ds.depth & 1) && (g_ds.depth >> 4 & 15) == 3 && vk_nvn_shader_fade_cut(d.mod[4]);
    t0 = plat_ns();
    s = vk_nvn_pipeline(&d, &made, why, sizeof why);
    g_fr_pipe_ns += plat_ns() - t0;
    if (!s) {
        if (g_replaying) vk_nvn_replay_skip("no pipeline");
        FAIL(why);
    }
    g_pd_ok++;
    g_pd_made += (u64)made;
    g_fr_made += (u64)made;
    if (g_replaying) replay_draw(h, &d, st);
}
static void dispatch_pipeline(const HostCmd* h, int indirect) { /* lock held: DispatchCompute(Indirect) */
    int err, made = 0;
    char why[64];
    ProgStage* s = bound_stage(ST_CS, &err);
    NvnStageBind b;
    NvnStageKey key;
    int mod;
    u64 t0;
    g_pc_disp++;
    if (!s) FAIL(err ? "bound compute program not recorded" : "no compute program");
    if (s->mod < 0) FAIL("compute stage not translated");
    memset(&key, 0, sizeof key);
    stage_bind(s, &b);
    vk_nvn_tex_key(s->mod, &b, &key); /* as compute.c: its integer textures and storage images' store kinds */
    t0 = plat_ns();
    mod = key.tex_int || key.rt_int ? stage_variant(s, &key) : s->mod;
    if (mod < 0) FAIL("compute stage not translated for its key");
    if (!g_live) draw_descriptors(&s, &mod, 1);
    err = vk_nvn_compute_pipeline(mod, &made, why, sizeof why);
    g_fr_pipe_ns += plat_ns() - t0;
    if (!err) FAIL(why);
    g_pc_ok++;
    g_pc_made += (u64)made;
    g_fr_made += (u64)made;
    if (g_replaying) /* (cmd, x, y, z) or (cmd, indirect) */
        vk_nvn_replay_dispatch(mod, &b, (u32)h->a[0], (u32)h->a[1], (u32)h->a[2], indirect ? h->a[0] : 0);
}
/* SetRenderTargets in a submitted list: the target formats */
static void ds_targets(const HostCmd* h, const u8* blob) {
    u32 n = (u32)(h->a[0] & 0xFFFFFFFFu), i, fmt[9], layer[9], info[18];
    if (n > 8) n = 8;
    g_ds.ncolor = n;
    g_ds.rt_unknown = 0;
    if (h->blob_len < (n + 1) * 8 + n * 8) {
        g_ds.rt_unknown = 1;
        return;
    }
    memcpy(info, blob + h->blob_len - (n + 1) * 8, (n + 1) * 8);
    for (i = 0; i <= n; i++) {
        fmt[i] = info[i * 2];
        layer[i] = info[i * 2 + 1];
    }
    memset(g_ds.rt, 0, sizeof g_ds.rt);
    memset(&g_ds.zt, 0, sizeof g_ds.zt);
    for (i = 0; i < 8; i++) {
        u64 t = 0;
        const TexRec* r;
        g_ds.color_fmt[i] = 0;
        if (i >= n) continue;
        memcpy(&t, blob + i * 8, 8);
        if (!t) continue;
        r = tex_known(t) ? tex_slot(t, 0) : NULL;
        if (fmt[i])
            g_ds.color_fmt[i] = fmt[i];
        else if (r)
            g_ds.color_fmt[i] = r->format;
        else
            g_ds.rt_unknown = 1;
        if (r) g_ds.rt[i] = (NvnTarget){r->gpu, r->width, r->height, g_ds.color_fmt[i], r->depth, layer[i]};
    }
    g_ds.depth_fmt = 0;
    if (h->a[3]) {
        const TexRec* r = tex_known(h->a[3]) ? tex_slot(h->a[3], 0) : NULL;
        if (fmt[n])
            g_ds.depth_fmt = fmt[n];
        else if (r)
            g_ds.depth_fmt = r->format;
        else
            g_ds.rt_unknown = 1;
        if (r) g_ds.zt = (NvnTarget){r->gpu, r->width, r->height, g_ds.depth_fmt, r->depth, layer[n]};
    }
    if (g_replaying) vk_nvn_replay_targets(g_ds.rt, g_ds.ncolor, &g_ds.zt);
}
static void frame_end(void) { /* lock held: a present */
    g_frames++;
    if (g_fr_made) g_frames_making++;
    if (g_fr_fail) g_frames_failing++;
    if (g_fr_made > g_fr_max_made) g_fr_max_made = g_fr_made;
    g_fr_hist[!g_fr_made ? 0 : g_fr_made < 10 ? 1 : g_fr_made < 100 ? 2 : 3]++;
    if ((g_fr_made || g_fr_fail) && g_fr_logged < 40) {
        g_fr_logged++;
        LOG("nvn-cmdlist: frame %llu: draws and dispatches %llu, pipelines made %llu, failed %llu\n",
            (unsigned long long)g_frames, (unsigned long long)g_fr_draws, (unsigned long long)g_fr_made,
            (unsigned long long)g_fr_fail);
    }
    g_pipe_ns += g_fr_pipe_ns;
    if (g_fr_pipe_ns > g_pipe_worst) g_pipe_worst = g_fr_pipe_ns;
    g_pipe_long += g_fr_pipe_ns > 16000000;
    g_fr_made = g_fr_fail = g_fr_draws = g_fr_pipe_ns = 0;
}
static void pipe_dump(void) { /* lock held */
    int i;
    LOG("nvn-cmdlist: time making pipelines and stage variants %llu ms, worst frame %llu ms, frames over 16 ms %llu\n",
        (unsigned long long)(g_pipe_ns / 1000000), (unsigned long long)(g_pipe_worst / 1000000),
        (unsigned long long)g_pipe_long);
    LOG("nvn-cmdlist: pipelines: draws %llu, with a pipeline %llu, without %llu; graphics pipelines made %llu; "
        "dispatches %llu, with a pipeline %llu, compute pipelines made %llu\n",
        (unsigned long long)g_pd_draws, (unsigned long long)g_pd_ok, (unsigned long long)(g_pd_draws - g_pd_ok),
        (unsigned long long)g_pd_made, (unsigned long long)g_pc_disp, (unsigned long long)g_pc_ok,
        (unsigned long long)g_pc_made);
    LOG("nvn-cmdlist: frames %llu: making pipelines %llu (0: %llu, 1-9: %llu, 10-99: %llu, 100+: %llu; most %llu), "
        "with a draw failing %llu; render target views with a format %llu, view getter failed %llu, state table full "
        "%u\n",
        (unsigned long long)g_frames, (unsigned long long)g_frames_making, (unsigned long long)g_fr_hist[0],
        (unsigned long long)g_fr_hist[1], (unsigned long long)g_fr_hist[2], (unsigned long long)g_fr_hist[3],
        (unsigned long long)g_fr_max_made, (unsigned long long)g_frames_failing, (unsigned long long)g_view_fmt,
        (unsigned long long)g_view_fmt_failed, g_seen_full);
    for (i = 0; i < 16 && g_pd_why_n[i]; i++)
        LOG("nvn-cmdlist:   without a pipeline, %llu: %s\n", (unsigned long long)g_pd_why_n[i], g_pd_why[i]);
    {
        static const char* const kind[NVN_B_KINDS] = {"constant buffer 0 (host)", "constant buffer 1 (program)",
                                                      "constant buffer 2 (host)", "uniform buffers", "uniform buffers not bound (zeros)",
                                                      "storage buffers",          "textures",
                                                      "images"};
        LOG("nvn-cmdlist: descriptors: draws and dispatches checked %llu, every binding resolved %llu\n",
            (unsigned long long)g_db_draws, (unsigned long long)g_db_complete);
        for (i = 0; i < NVN_B_KINDS; i++)
            if (g_db_ok[i] || g_db_fail[i])
                LOG("nvn-cmdlist:   %s: resolved %llu, not %llu\n", kind[i], (unsigned long long)g_db_ok[i],
                    (unsigned long long)g_db_fail[i]);
        for (i = 0; i < 16 && g_db_why_n[i]; i++)
            LOG("nvn-cmdlist:   not resolved, %llu: %s\n", (unsigned long long)g_db_why_n[i], g_db_why[i]);
        vk_nvn_slots_dump();
    }
}
static u64 cpu_of(u64 gpu) { /* lock held: the CPU address of a GPU address in a live pool, 0 for none */
    int i;
    for (i = 0; i < POOLS; i++)
        if (g_pool[i].live && g_pool[i].cpu && gpu - g_pool[i].gpu < g_pool[i].size) return g_pool[i].cpu + gpu - g_pool[i].gpu;
    return 0;
}
static u64 g_sync_ns, g_sync_n; /* how long the game's threads wait in NVN for the GPU, all threads together */
void nvn_capture_frame(const char* prefix) {
    pm_lock(&g_lk);
    snprintf(g_cap_out, sizeof g_cap_out, "%s", prefix);
    pm_unlock(&g_lk);
    __atomic_store_n(&g_cap_req, 1, __ATOMIC_RELEASE);
    if (!g_live) LOG("capture: needs the live renderer (the default), nothing saved\n");
}
static void present(CPU* RESTRICT c, const NvnFn* f) { /* nvnQueuePresentTexture(queue, window, index) */
    u32 index = (u32)c->x[2];
    pm_lock(&g_lk);
    if (g_live) { /* every 600 presents its picture, and the renderer's of the same buffer */
        const TexRec* t = index < g_nwin_tex && tex_known(g_win_tex[index]) ? tex_slot(g_win_tex[index], 0) : NULL;
        u64 gpu = t ? t->gpu : 0, frame = g_frames + 1;
        char path[640];
        const char* d = getenv("SNX_NVN_LIVE_DUMP"); /* a frame to save every image of */
        int all = d && strtoull(d, NULL, 10) == frame, snap = (frame % 600 == 0 || all) && g_live_out[0];
        const char* at = getenv("SNX_NVN_CAPTURE_AT"); /* F6 at these presents (a comma list), for scripted runs */
        while (at && *at && !g_cap_frame) {
            char* e;
            if (strtoull(at, &e, 10) == frame) {
                snprintf(g_cap_out, sizeof g_cap_out, "%s", g_live_out[0] ? g_live_out : "capture");
                __atomic_store_n(&g_cap_req, 1, __ATOMIC_RELEASE);
            }
            at = e != at && *e == ',' ? e + 1 : NULL;
        }
        int cap = g_cap_frame == frame;
        snprintf(path, sizeof path, "%s-f%06llu", cap ? g_cap_out : g_live_out, (unsigned long long)frame);
        vk_nvn_live_present(gpu, gpu ? gmmu_translate(gpu) : 0, snap || cap ? path : NULL, all || cap);
        if (cap) {
            g_cap_frame = 0;
            LOG("capture: frame %llu saved to %s*\n", (unsigned long long)frame, path);
        }
        if (__atomic_exchange_n(&g_cap_req, 0, __ATOMIC_ACQ_REL)) {
            /* two presents on, so the frame saved is logged whole whichever side of a present its submits fall */
            g_cap_frame = frame + 2;
            LOG("capture: frame %llu, the draws up to it follow\n", (unsigned long long)g_cap_frame);
        }
        vk_nvn_live_debug((d && strtoull(d, NULL, 10) == frame + 1) || (g_cap_frame && g_cap_frame <= frame + 2));
        if (frame % 600 == 0) {
            static u64 ns_seen, n_seen;
            u64 ns = __atomic_load_n(&g_sync_ns, __ATOMIC_RELAXED), n = __atomic_load_n(&g_sync_n, __ATOMIC_RELAXED);
            LOG("nvn-cmdlist: the game's waits for the GPU, a frame on average: %.2f ms in %.1f calls\n",
                (double)(ns - ns_seen) / 6e8, (double)(n - n_seen) / 600.0);
            ns_seen = ns;
            n_seen = n;
            vk_nvn_live_stats(frame);
        }
    } else if (g_replaying) { /* the frame replayed ends: its picture, and the renderer's of the same buffer */
        const TexRec* t = index < g_nwin_tex && tex_known(g_win_tex[index]) ? tex_slot(g_win_tex[index], 0) : NULL;
        int k;
        LOG("nvn-cmdlist: frame %llu presents window texture %u (%llx)\n", (unsigned long long)g_frames, index,
            (unsigned long long)(t ? t->gpu : 0));
        for (k = 0; k < POOLS; k++)
            if (g_pool[k].live)
                LOG("nvn-cmdlist:   pool %llx: GPU %llx + %llx, CPU %llx\n", (unsigned long long)g_pool[k].obj,
                    (unsigned long long)g_pool[k].gpu, (unsigned long long)g_pool[k].size,
                    (unsigned long long)g_pool[k].cpu);
        vk_nvn_replay_end(t ? t->gpu : 0, t ? cpu_of(t->gpu) : 0, g_replay_out);
        g_replaying = 0;
        g_replay_frame = ~0ull;
    }
    frame_end();
    vk_nvn_frame();
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}
static void win_textures(CPU* RESTRICT c, const NvnFn* f) { /* nvnWindowBuilderSetTextures(builder, count, textures) */
    u32 n = (u32)c->x[1], i;
    const u8* p = n && n <= 8 ? cpu_bytes(c->x[2], (u64)n * 8) : NULL;
    pm_lock(&g_lk);
    if (p) {
        for (i = 0; i < n; i++) memcpy(&g_win_tex[i], p + i * 8, 8);
        g_nwin_tex = n;
        if (tex_known(g_win_tex[0])) vk_nvn_window_size(tex_slot(g_win_tex[0], 0)->width, tex_slot(g_win_tex[0], 0)->height);
    }
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}
static void device_mode(CPU* RESTRICT c, const NvnFn* f) { /* nvnDeviceSetWindowOriginMode / SetDepthMode */
    pm_lock(&g_lk);
    if (strstr(f->name, "Origin"))
        g_upper_left = (u32)c->x[1] & 1;
    else
        g_depth_zero = (u32)c->x[1] & 1;
    LOG("nvn-cmdlist: %s %llu\n", f->name, (unsigned long long)c->x[1]);
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}

/* the commands that set dynamic state (part 3), and in a replay the clears and the commands it leaves out */
static void ds_dynamic(const HostCmd* h, const u8* blob) { /* lock held */
    enum {
        VP,
        SC,
        DR,
        SREF,
        SMASK,
        SVMASK,
        AREF,
        BCOLOR,
        LWIDTH,
        POFF,
        VBUF,
        CCOLOR,
        CDEPTH,
        DISP,
        DISPI,
        COPY,
        CLEARB,
        UPDATE,
        DBOUNDS,
        NDYN
    };
    static const char* const names[NDYN] = {
        "SetViewport",         "SetScissor",  "SetDepthRange",       "SetStencilRef",   "SetStencilMask",
        "SetStencilValueMask", "SetAlphaRef", "SetBlendColor",       "SetLineWidth",    "SetPolygonOffsetClamp",
        "BindVertexBuffers",   "ClearColor",  "ClearDepthStencil",   "DispatchCompute", "DispatchComputeIndirect",
        "CopyBufferToBuffer",  "ClearBuffer", "UpdateUniformBuffer", "SetDepthBounds"};
    static int op[NDYN] = {-2};
    u32 i, k;
    if (op[0] == -2)
        for (k = 0; k < NDYN; k++) {
            char full[64];
            snprintf(full, sizeof full, "nvnCommandBuffer%s", names[k]);
            op[k] = op_named(full);
        }
    for (k = 0; k < NDYN && op[k] != h->op; k++) {}
    switch (k) {
        case VP: /* (cmd, x, y, w, h) */
        case SC:
            for (i = 0; i < 4; i++) (k == VP ? g_ds.viewport : g_ds.scissor)[i] = (i32)h->a[i];
            break;
        case DR: memcpy(g_ds.depth_range, h->f, 8); break;
        case SREF: /* (cmd, face: 1 front, 2 back, 3 both, value) */
        case SMASK:
        case SVMASK:
            for (i = 0; i < 2; i++)
                if (h->a[0] >> i & 1)
                    (k == SREF ? g_ds.stencil_ref : k == SMASK ? g_ds.stencil_wmask : g_ds.stencil_vmask)[i] =
                        (u32)h->a[1] & 0xFF;
            break;
        case AREF: memcpy(&g_ds.alpha_ref, h->f, 4); break;
        case DBOUNDS: /* (cmd, enable, min, max) */
            g_ds.depth_bounds_on = (u32)h->a[0] & 1;
            memcpy(g_ds.depth_bounds, h->f, 8);
            break;
        case BCOLOR:
            if (h->blob_len >= 16) memcpy(g_ds.blend_color, blob, 16);
            break;
        case LWIDTH: memcpy(&g_ds.line_width, h->f, 4); break;
        case POFF: memcpy(g_ds.offset, h->f, 12); break; /* factor, units, clamp */
        case VBUF:                                         /* (cmd, first, count, ranges) */
            for (i = 0; i < (u32)h->a[1] && (u32)h->a[0] + i < 16 && (i + 1) * 16 <= h->blob_len; i++)
                memcpy(&g_ds.vbuf[(u32)h->a[0] + i], blob + i * 16, 16);
            break;
        case CCOLOR: /* (cmd, index, colour, mask) */
            if (g_replaying && h->blob_len >= 16) {
                NvnDrawDyn y;
                float rgba[4];
                memcpy(rgba, blob, 16);
                replay_dyn(&y);
                vk_nvn_replay_clear_color((u32)h->a[0], rgba, (u32)h->a[2], &y);
            }
            break;
        case CDEPTH: /* (cmd, depth in v0, depth mask, stencil, stencil mask) */
            if (g_replaying) {
                NvnDrawDyn y;
                float depth;
                memcpy(&depth, h->f, 4);
                replay_dyn(&y);
                vk_nvn_replay_clear_depth(depth, (int)(h->a[0] & 1), (u32)h->a[1] & 0xFF, (u32)h->a[2] & 0xFF, &y);
            }
            break;
        case DISP: /* replayed with their pipelines (dispatch_pipeline) */
        case DISPI: break;
        case COPY: /* (cmd, src, dst, size, flags) */
            if (g_replaying) vk_nvn_replay_buffer(h->a[0], h->a[1], h->a[2], 0);
            break;
        case CLEARB: /* (cmd, dst, size, value) */
            if (g_replaying) vk_nvn_replay_buffer(0, h->a[0], h->a[1], (u32)h->a[2]);
            break;
        case UPDATE: /* (cmd, buffer, buffer size, offset, size, data): its bytes kept at record time */
            if (g_replaying && h->a[0] && h->a[4] && h->blob_len)
                vk_nvn_replay_update(h->a[0] + h->a[2], blob, h->blob_len);
            break;
    }
}

/* every resource reference in a submitted list, and its state objects decoded */
static void check_list(CPU* RESTRICT c, const CmdList* l) {
    static const struct {
        const char* name;
        int kind;
        u32 size; /* of one object */
    } states[] = {{"nvnCommandBufferBindBlendState", S_BLEND, 8},
                  {"nvnCommandBufferBindColorState", S_COLOR, 4},
                  {"nvnCommandBufferBindDepthStencilState", S_DEPTH, 8},
                  {"nvnCommandBufferBindPolygonState", S_POLYGON, 4},
                  {"nvnCommandBufferBindChannelMaskState", S_CHANMASK, 4},
                  {"nvnCommandBufferBindMultisampleState", S_MULTISAMPLE, 0x18},
                  {"nvnCommandBufferBindVertexAttribState", S_VATTR, 4},
                  {"nvnCommandBufferBindVertexStreamState", S_VSTREAM, 8}};
    static int state_op[8] = {-2};
    u32 si;
    static int op_vb = -2, op_ubo, op_ssbo, op_tex, op_img, op_rt, op_de, op_dei, op_dein, op_mdi, op_dci, op_copy,
                   op_clear;
    u32 k, j;
    if (op_vb == -2) {
        op_vb = op_named("nvnCommandBufferBindVertexBuffers");
        op_ubo = op_named("nvnCommandBufferBindUniformBuffers");
        op_ssbo = op_named("nvnCommandBufferBindStorageBuffers");
        op_tex = op_named("nvnCommandBufferBindTextures");
        op_img = op_named("nvnCommandBufferBindImages");
        op_rt = op_named("nvnCommandBufferSetRenderTargets");
        op_de = op_named("nvnCommandBufferDrawElements");
        op_dein = op_named("nvnCommandBufferDrawElementsInstanced");
        op_dei = op_named("nvnCommandBufferDrawElementsIndirect");
        op_mdi = op_named("nvnCommandBufferMultiDrawElementsIndirectCount");
        op_dci = op_named("nvnCommandBufferDispatchComputeIndirect");
        op_copy = op_named("nvnCommandBufferCopyBufferToBuffer");
        op_clear = op_named("nvnCommandBufferClearBuffer");
    }
    static int op_prog = -2;
    if (state_op[0] == -2)
        for (si = 0; si < 8; si++) state_op[si] = op_named(states[si].name);
    static int op_patch, op_clamp, op_disp, op_tpool, op_spool;
    if (op_prog == -2) {
        op_prog = op_named("nvnCommandBufferBindProgram");
        op_patch = op_named("nvnCommandBufferSetPatchSize");
        op_clamp = op_named("nvnCommandBufferSetDepthClamp");
        op_disp = op_named("nvnCommandBufferDispatchCompute");
        op_tpool = op_named("nvnCommandBufferSetTexturePool");
        op_spool = op_named("nvnCommandBufferSetSamplerPool");
    }
    if (!g_ds_set) ds_reset();
    if (!g_replaying && g_frames == g_replay_frame) { /* the frame to replay: from its first submitted list */
        if (import_on() && vk_nvn_replay_begin()) {
            g_replaying = 1;
            vk_nvn_replay_targets(g_ds.rt, g_ds.ncolor, &g_ds.zt);
            LOG("nvn-cmdlist: replaying frame %llu\n", (unsigned long long)g_frames);
        } else {
            LOG("nvn-cmdlist: frame %llu can't be replayed (SNX_NVN_IMPORT=1 needed)\n", (unsigned long long)g_frames);
            g_replay_frame = ~0ull;
        }
    }
    for (k = 0; k < l->n; k++) {
        const HostCmd* h = &l->cmd[k];
        const u8* blob = l->bytes + h->blob;
        int op = h->op;
        static int op_fence = -2;
        if (op_fence == -2) op_fence = op_named("nvnCommandBufferFenceSync");
        if (op == op_fence) vk_nvn_live_fence();
        ds_dynamic(h, blob);
        for (si = 0; si < 8; si++) {
            int kind = states[si].kind;
            u32 n = 0;
            if (op != state_op[si]) continue;
            if (kind == S_VATTR || kind == S_VSTREAM) { /* an array: these, and the rest off */
                if (kind == S_VATTR)
                    g_ds.nattr = 0;
                else
                    g_ds.nstream = 0;
                g_ds.bad &= ~(1u << kind);
            }
            for (j = 0; j + states[si].size <= h->blob_len; j += states[si].size, n++) {
                u64 v;
                int ok = state_decode(c, kind, blob + j, states[si].size, &v);
                if (kind == S_VATTR || kind == S_VSTREAM) {
                    if (n >= 16) continue;
                    if (kind == S_VATTR)
                        g_ds.vattr[n] = v, g_ds.nattr = n + 1;
                    else
                        g_ds.vstream[n] = v, g_ds.nstream = n + 1;
                    if (!ok) g_ds.bad |= 1u << kind;
                    continue;
                }
                g_ds.bad = ok ? g_ds.bad & ~(1u << kind) : g_ds.bad | 1u << kind;
                if (!ok) continue;
                if (kind == S_BLEND)
                    g_ds.blend[(v >> 48) & 7] = v;
                else if (kind == S_COLOR)
                    g_ds.color = v;
                else if (kind == S_DEPTH)
                    g_ds.depth = v;
                else if (kind == S_CHANMASK)
                    g_ds.chanmask = v;
                else if (kind == S_POLYGON)
                    g_ds.polygon = v;
                else if (kind == S_MULTISAMPLE)
                    g_ds.multisample = v;
            }
        }
        if (op == op_prog) { /* (cmd, program, stage bits): vertex 1, fragment 2, geometry 4, tess 8 and 0x10, compute 0x20 */
            static const int bit_stage[6] = {ST_VS, ST_FS, ST_GS, ST_TCS, ST_TES, ST_CS};
            u32 bits = (u32)h->a[1], b;
            prog_translate(h->a[0]);
            for (b = 0; b < 6; b++)
                if (bits >> b & 1) g_ds.prog[bit_stage[b]] = h->a[0];
        } else if (op == op_rt)
            ds_targets(h, blob);
        else if (op == op_patch)
            g_ds.patch = (u32)h->a[0];
        else if (op == op_clamp)
            g_ds.depth_clamp = (u32)h->a[0] & 1;
        else if (op == op_tpool)
            g_ds.tpool = h->a[0];
        else if (op == op_spool)
            g_ds.spool = h->a[0];
        else if (op == op_ubo)
            ds_bind(h, blob, NVN_B_UBO);
        else if (op == op_ssbo)
            ds_bind(h, blob, NVN_B_SSBO);
        else if (op == op_tex)
            ds_bind(h, blob, NVN_B_TEX);
        else if (op == op_img)
            ds_bind(h, blob, NVN_B_IMG);
        else if (op == op_disp || op == op_dci)
            dispatch_pipeline(h, op == op_dci);
        else if (g_spec[op].draw) /* every draw's first argument is its NVNdrawPrimitive */
            draw_pipeline(h);
        if (g_live) continue; /* what follows is statistics */
        if (op == op_vb || op == op_ubo || op == op_ssbo) /* {address, size} each */
            for (j = 0; j + 16 <= h->blob_len; j += 16) {
                u64 a;
                memcpy(&a, blob + j, 8);
                ref_address(a);
            }
        else if (op == op_tex)
            for (j = 0; j + 8 <= h->blob_len; j += 8) {
                u64 v;
                memcpy(&v, blob + j, 8);
                ref_handle(v);
            }
        else if (op == op_img)
            for (j = 0; j + 8 <= h->blob_len; j += 8) {
                u64 v;
                memcpy(&v, blob + j, 8);
                if (!v)
                    g_ref_null[R_IMAGE]++;
                else
                    ref(R_IMAGE, (v & 0xFFFFF) < IDS && tex_known(g_img_id[v & 0xFFFFF]), v);
            }
        else if (op == op_rt) { /* the colour textures (the first array), and the depth texture in x4 */
            u32 n = (u32)(h->a[0] & 0xFFFFFFFFu);
            for (j = 0; j < n && (j + 1) * 8 <= h->blob_len; j++) {
                u64 t;
                memcpy(&t, blob + j * 8, 8);
                if (!t)
                    g_ref_null[R_TARGET]++;
                else
                    ref(R_TARGET, tex_known(t), t);
            }
            if (h->a[3])
                ref(R_TARGET, tex_known(h->a[3]), h->a[3]);
            else
                g_ref_null[R_TARGET]++;
        } else if (op == op_de || op == op_dein) /* (cmd, mode, type, count, index buffer, ...) */
            ref_address(h->a[3]);
        else if (op == op_dei) { /* (cmd, mode, type, index buffer, indirect buffer) */
            ref_address(h->a[2]);
            ref_address(h->a[3]);
        } else if (op == op_mdi) { /* (cmd, mode, type, index, indirect, parameter, max, stride) */
            ref_address(h->a[2]);
            ref_address(h->a[3]);
            ref_address(h->a[4]);
        } else if (op == op_dci) /* (cmd, indirect buffer) */
            ref_address(h->a[0]);
        else if (op == op_copy) { /* (cmd, src, dst, size, flags) */
            ref_address(h->a[0]);
            ref_address(h->a[1]);
        } else if (op == op_clear) /* (cmd, dst, size, value) */
            ref_address(h->a[0]);
    }
}
static void res_dump(void) { /* lock held */
    state_dump();
    prog_dump();
    pipe_dump();
    int k, pools = 0;
    u32 i, textures = 0;
    for (k = 0; k < POOLS; k++) pools += g_pool[k].live;
    for (i = 0; i < TEXTURES; i++) textures += (u32)g_tex[i].live;
    LOG("nvn-cmdlist: resources: pools %d live (%llu made), textures %u live (%llu made), getters failed %llu\n", pools,
        (unsigned long long)g_pools_made, textures, (unsigned long long)g_textures_made,
        (unsigned long long)g_getter_failed);
    LOG("nvn-cmdlist: texture pool descriptors checked %llu, equal to the driver's texture %llu; pool not known %llu, "
        "texture not known %llu\n",
        (unsigned long long)g_tic_checked, (unsigned long long)g_tic_ok, (unsigned long long)g_tic_nopool,
        (unsigned long long)g_tic_notex);
    for (k = 0; k < R_KINDS; k++)
        if (g_ref[k] || g_ref_null[k])
            LOG("nvn-cmdlist: references: %s %llu, known %llu, not known %llu, null %llu\n", g_rkind[k],
                (unsigned long long)g_ref[k], (unsigned long long)g_ref_ok[k],
                (unsigned long long)(g_ref[k] - g_ref_ok[k]), (unsigned long long)g_ref_null[k]);
}
static void queue_submit(CPU* RESTRICT c, const NvnFn* f) { /* (queue, count, handles) */
    static int replay_read;
    u64 count = c->x[1] & 0xFFFFFFFFu, i;
    const u8* p = count && count <= 64 ? cpu_bytes(c->x[2], count * 8) : NULL;
    u64 handles[64];
    if (p) memcpy(handles, p, count * 8);
    pm_lock(&g_lk);
    if (!replay_read) { /* SNX_NVN_REPLAY=frame [SNX_NVN_REPLAY_OUT=path without .ppm] */
        const char* e = getenv("SNX_NVN_REPLAY");
        const char* o = getenv("SNX_NVN_REPLAY_OUT");
        replay_read = 1;
        if (e && *e) g_replay_frame = strtoull(e, NULL, 10);
        if ((e = getenv("SNX_NVN_CLIP")) != NULL) g_clip_on = (u32)strtoul(e, NULL, 0) & 0xFF;
        snprintf(g_replay_out, sizeof g_replay_out, "%s", o && *o ? o : "nvn-replay");
        o = getenv("SNX_NVN_LIVE_OUT");
        snprintf(g_live_out, sizeof g_live_out, "%s", o ? o : "");
        if (front2() || ((e = getenv("SNX_NVN_LIVE")) != NULL && e[0] == '1')) {
            if (import_on() && vk_nvn_live_start()) {
                g_live = g_replaying = 1;
                if (front2()) vk_nvn_live_show();
                if (front2() && (e = getenv("SNX_NVN_RELEASE_ONLY")) != NULL && e[0]) {
                    vk_nvn_live_real();
                    g_gpu_release_only = e[0] == '1'; /* 2: real writes with the stream still run, for comparing */
                    /* a debug aid: the engines still run in release-only mode, 2 3D, 4 compute, 8 inline, 16 2D, 32 copy */
                    if ((e = getenv("SNX_NVN_STREAM_KEEP")) != NULL) g_gpu_stream_keep = (u32)strtoul(e, NULL, 0);
                }
            }
            else
                LOG("nvn-cmdlist: live frames need SNX_NVN_IMPORT=1\n");
            g_replay_frame = ~0ull;
        }
    }
    if (g_live) vk_nvn_live_window(1);
    for (i = 0; p && i < count; i++) {
        int k;
        CmdList* l = NULL;
        g_submits++;
        for (k = 0; k < LISTS && !l; k++)
            if (!g_list[k].recording && g_list[k].handle == handles[i]) l = &g_list[k];
        if (!l) {
            g_missing++;
            continue;
        }
        g_found++;
        for (k = 0; k < (int)l->n; k++) g_draws += (u64)g_spec[l->cmd[k].op].draw;
        check_list(c, l);
        l->handle = 0; /* submitted: the list may record again */
    }
    if (g_live) vk_nvn_live_window(0);
    maybe_dump();
    pm_unlock(&g_lk);
    nvn_guest(c, f);
}

static void sync_wait(CPU* RESTRICT c, const NvnFn* f) {
    u64 t0 = plat_ns(), obj = c->x[0], timeout = c->x[1], ns;
    nvn_guest(c, f);
    ns = plat_ns() - t0;
    __atomic_add_fetch(&g_sync_ns, ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_sync_n, 1, __ATOMIC_RELAXED);
    if (ns > 50000000) /* a wait long enough to show as a hitch */
        LOG_LIMIT(200, "nvn-cmdlist: %s(%llx, timeout %llx) took %.1f ms, result %llu, frame %llu\n", f->name,
                  (unsigned long long)obj, (unsigned long long)timeout, (double)ns / 1e6,
                  (unsigned long long)c->x[0], (unsigned long long)g_frames);
}
/* a command the host path has no case for, which release-only mode loses with the stream; or a CPU read of a texture
   the host drew, which finds guest memory unwritten */
static void cmd_unkept(CPU* RESTRICT c, const NvnFn* f) {
    if (g_gpu_release_only) {
        int i;
        pm_lock(&g_lk);
        for (i = 0; i < UNKEPT && g_unkept[i].name && g_unkept[i].name != f->name; i++) {}
        if (i < UNKEPT) {
            if (!g_unkept[i].name) {
                g_unkept[i].name = f->name;
                WARN_LIMIT(UNKEPT, "nvn-cmdlist: %s isn't kept: lost in release-only mode\n", f->name);
            }
            g_unkept[i].n++;
        }
        pm_unlock(&g_lk);
    }
    nvn_guest(c, f);
}
NvnHandler nvn_cmd_handler(const char* name, int* op) {
    int i;
    if (!strcmp(name, "nvnCommandBufferBeginRecording")) return *op = OP_BEGIN, cmd_begin;
    if (!strcmp(name, "nvnCommandBufferEndRecording")) return *op = OP_END, cmd_end;
    if (!strcmp(name, "nvnQueueSubmitCommands")) return *op = OP_SUBMIT, queue_submit;
    if (!strcmp(name, "nvnMemoryPoolInitialize")) return *op = OP_RES, pool_init;
    if (!strcmp(name, "nvnMemoryPoolFinalize")) return *op = OP_RES, pool_fini;
    if (!strcmp(name, "nvnTextureInitialize")) return *op = OP_RES, tex_init;
    if (!strcmp(name, "nvnTextureFinalize")) return *op = OP_RES, tex_fini;
    if (!strcmp(name, "nvnTexturePoolInitialize")) return *op = OP_RES, tpool_init;
    if (!strcmp(name, "nvnSamplerPoolInitialize")) return *op = OP_RES, spool_init;
    if (!strcmp(name, "nvnProgramSetShaders")) return *op = OP_RES, prog_shaders;
    if (!strcmp(name, "nvnQueuePresentTexture")) return *op = OP_RES, present;
    if (!strcmp(name, "nvnSyncWait") || !strcmp(name, "nvnQueueWaitSync") || !strcmp(name, "nvnQueueFinish"))
        return *op = OP_RES, sync_wait;
    if (!strcmp(name, "nvnWindowBuilderSetTextures")) return *op = OP_RES, win_textures;
    if (!strcmp(name, "nvnDeviceSetWindowOriginMode") || !strcmp(name, "nvnDeviceSetDepthMode"))
        return *op = OP_RES, device_mode;
    if (!strcmp(name, "nvnTexturePoolRegisterTexture")) return *op = OP_RES, reg_texture;
    if (!strcmp(name, "nvnTexturePoolRegisterImage")) return *op = OP_RES, reg_image;
    if (!strcmp(name, "nvnSamplerPoolRegisterSampler") || !strcmp(name, "nvnSamplerPoolRegisterSamplerBuilder"))
        return *op = OP_RES, reg_sampler;
    for (i = 0; i < NOPS; i++)
        if (!strcmp(g_spec[i].name, name)) return *op = i, cmd_record;
    if (!strcmp(name, "nvnTextureReadTexels") || !strcmp(name, "nvnTextureReadTexelsStrided") ||
        (!strncmp(name, "nvnCommandBuffer", 16) && strcmp(name, "nvnCommandBufferInitialize") &&
         strcmp(name, "nvnCommandBufferFinalize") && strncmp(name, "nvnCommandBufferGet", 19) &&
         strncmp(name, "nvnCommandBufferAdd", 19) && strcmp(name, "nvnCommandBufferSetMemoryCallback") &&
         strcmp(name, "nvnCommandBufferSetMemoryCallbackData") && strcmp(name, "nvnCommandBufferSetDebugLabel") &&
         strcmp(name, "nvnCommandBufferSetDebugLabelData")))
        return cmd_unkept;
    return NULL;
}
