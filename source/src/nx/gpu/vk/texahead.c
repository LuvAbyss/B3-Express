/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture pack read ahead (texture.c pack_*): read on the render thread, a pack texture cost about 3 ms there, and
   the renderer drops textures unused for ten seconds, so the same ones were read again and again (1.8 GB in 110 s of
   play). The game registers nearly every texture 30 or more presents before it draws it (nvnTexturePoolRegister-
   Texture), so a worker thread takes the registrations: for each size the pack holds textures of that width and height
   in, the guest bytes are hashed, and a texture found is read and decompressed into a cache. The render thread finds
   its blocks there; a miss reads as before, into the cache too. The cache keeps up to CACHE_BYTES: the game registers
   far more textures than it draws (8,585 registrations in a load's first 10 s), and what goes first is in victim().
   Each thread keeps the pack's shards open. Guest memory is read without landing GPU results (gmem_ptr_untracked): a texture still being written
   only hashes to nothing, and the render thread checks the bytes it creates the texture from in any case */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/texture/texpack.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define CACHE_BYTES (768ull << 20)
#define QUEUE       4096
#define RECENT                                                                                                         \
    4096 /* registrations handled lately, by address and size: a texture registered every frame is hashed once */

typedef struct {
    const TexPackEntry* e;
    u8* data;
    u64 used;  /* a clock: the least recently used goes first */
    int taken; /* a texture's creation took it */
    u32 at;    /* the present it came in at */
} Cached;
static PMutex g_lk = PMUTEX_INIT; /* the cache and the queue */
static PCond g_cv;
static Cached* g_cache;
static u32 g_ncache, g_capcache;
static u64 g_bytes, g_clock;
static struct {
    u64 gva;
    u32 w, h;
} g_queue[QUEUE];
static u32 g_qhead, g_qtail;
static int g_started;
static struct {
    u64 queued, dropped, found, read, hits, misses;
} g_st;

static Cached* cache_find(const TexPackEntry* e) {
    for (u32 i = 0; i < g_ncache; i++)
        if (g_cache[i].e == e) return &g_cache[i];
    return NULL;
}
/* the entry to make room with, or -1: one already taken (its texture is on the GPU, and comes back only if dropped
   there), the least recently used first; then one read ahead and not taken for STALE presents; for a texture being
   created (taken) any, the oldest read ahead first. A texture read ahead but not yet taken is about to be drawn: a read
   ahead finding only those makes no room. Under the lock */
#define STALE 1200
static int evictable(u32 i, int taken) {
    return g_cache[i].taken || taken || (u32)(g_vk_present - g_cache[i].at) > STALE;
}
static int victim(int taken) {
    int best = -1;
    for (u32 i = 0; i < g_ncache; i++) {
        if (!evictable(i, taken)) continue;
        if (best < 0 || g_cache[i].taken > g_cache[best].taken ||
            (g_cache[i].taken == g_cache[best].taken && g_cache[i].used < g_cache[best].used))
            best = (int)i;
    }
    return best;
}
/* whether raw more bytes fit, after what may go; under the lock */
static int room(u32 raw, int taken) {
    u64 free_ = CACHE_BYTES - g_bytes;
    for (u32 i = 0; i < g_ncache && free_ < raw; i++)
        if (evictable(i, taken)) free_ += g_cache[i].e->raw;
    return free_ >= raw;
}
/* data (malloc'd, e->raw bytes) kept for e, older entries going while over the limit; under the lock */
static void cache_put(const TexPackEntry* e, u8* data, int taken) {
    if (cache_find(e) || e->raw > CACHE_BYTES / 4 || !room(e->raw, taken)) {
        free(data);
        return;
    }
    while (g_ncache && g_bytes + e->raw > CACHE_BYTES) {
        int old = victim(taken);
        if (old < 0) break;
        g_bytes -= g_cache[old].e->raw;
        free(g_cache[old].data);
        g_cache[old] = g_cache[--g_ncache];
    }
    if (g_ncache == g_capcache) {
        u32 cap = g_capcache ? g_capcache * 2 : 256;
        Cached* c = realloc(g_cache, cap * sizeof *c);
        if (!c) {
            free(data);
            return;
        }
        g_cache = c;
        g_capcache = cap;
    }
    g_cache[g_ncache].e = e;
    g_cache[g_ncache].data = data;
    g_cache[g_ncache].used = ++g_clock;
    g_cache[g_ncache].taken = taken;
    g_cache[g_ncache].at = g_vk_present;
    g_ncache++;
    g_bytes += e->raw;
}

/* one registration: each size the pack holds a w x h texture in, hashed from the guest's bytes */
static void* g_wfiles[256]; /* the worker's open shards */
static void* g_rfiles[256]; /* the render thread's (vk_lock) */
static void ahead(u64 gva, u32 w, u32 h) {
    u64 sizes[16];
    u32 n = texpack_sizes(w, h, sizes, 16);
    for (u32 i = 0; i < n; i++) {
        const u8* p = gmem_ptr_untracked(gva, sizes[i]);
        const TexPackEntry* e;
        u8* data;
        int have;
        if (!p || !(e = texpack_find_any(texpack_hash(p, sizes[i]), sizes[i], w, h))) continue;
        pm_lock(&g_lk);
        have = cache_find(e) != NULL || !room(e->raw, 0); /* in already, or no room without dropping one on its way */
        g_st.found++;
        pm_unlock(&g_lk);
        if (have || !(data = malloc(e->raw ? e->raw : 1))) return;
        if (!texpack_read_with(g_wfiles, e, data)) {
            free(data);
            return;
        }
        pm_lock(&g_lk);
        g_st.read++;
        cache_put(e, data, 0);
        pm_unlock(&g_lk);
        return;
    }
}
static void worker(void* arg) {
    static struct {
        u64 gva;
        u32 w, h;
    } recent[RECENT];
    (void)arg;
    for (;;) {
        u64 gva;
        u32 w, h, slot;
        pm_lock(&g_lk);
        while (g_qhead == g_qtail) pc_wait(&g_cv, &g_lk, -1);
        gva = g_queue[g_qhead % QUEUE].gva;
        w = g_queue[g_qhead % QUEUE].w;
        h = g_queue[g_qhead % QUEUE].h;
        g_qhead++;
        pm_unlock(&g_lk);
        slot = (u32)((gva >> 8) ^ w * 31u ^ h) % RECENT;
        if (recent[slot].gva == gva && recent[slot].w == w && recent[slot].h == h) continue;
        recent[slot].gva = gva;
        recent[slot].w = w;
        recent[slot].h = h;
        ahead(gva, w, h);
    }
}

void vk_texahead_registered(u64 gva, u32 w, u32 h) {
    if (!vk_texpack_ready()) return;
    pm_lock(&g_lk);
    if (!g_started) {
        pc_init(&g_cv);
        g_started = plat_thread(worker, NULL, 0) == 0 ? 1 : -1;
    }
    if (g_started > 0 && g_qtail - g_qhead < QUEUE) {
        g_queue[g_qtail % QUEUE].gva = gva;
        g_queue[g_qtail % QUEUE].w = w;
        g_queue[g_qtail % QUEUE].h = h;
        g_qtail++;
        g_st.queued++;
        pc_signal(&g_cv);
    } else
        g_st.dropped++;
    pm_unlock(&g_lk);
}

int vk_texahead_copy(const TexPackEntry* e, u8* out) {
    Cached* c;
    u8* data;
    pm_lock(&g_lk);
    if ((c = cache_find(e)) != NULL) {
        memcpy(out, c->data, e->raw);
        c->used = ++g_clock;
        c->taken = 1;
        g_st.hits++;
        pm_unlock(&g_lk);
        return 1;
    }
    g_st.misses++;
    pm_unlock(&g_lk);
    if (!texpack_read_with(g_rfiles, e, out)) return 0;
    if ((data = malloc(e->raw ? e->raw : 1)) != NULL) { /* kept for when the texture comes back */
        memcpy(data, out, e->raw);
        pm_lock(&g_lk);
        cache_put(e, data, 1);
        pm_unlock(&g_lk);
    }
    return 1;
}

void vk_texahead_report(void) {
    pm_lock(&g_lk);
    if (g_st.queued || g_st.hits || g_st.misses)
        LOG("texpack: read ahead: %llu registrations (%llu dropped), %llu found, %llu read; at a texture's creation %llu "
            "from the cache, %llu read there; cache %llu textures, %.0f MB\n",
            (unsigned long long)g_st.queued, (unsigned long long)g_st.dropped, (unsigned long long)g_st.found,
            (unsigned long long)g_st.read, (unsigned long long)g_st.hits, (unsigned long long)g_st.misses,
            (unsigned long long)g_ncache, (double)g_bytes / 1e6);
    memset(&g_st, 0, sizeof g_st);
    pm_unlock(&g_lk);
}
