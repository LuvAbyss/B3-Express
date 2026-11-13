/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The profile option on Windows: samples the program counter of the runtime's threads every millisecond, and writes
   the functions they spent their time in to profile.txt beside the game every 10 seconds */
#include "nx/loader/module_abi.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_THREADS 256
#define PROF_BUCKETS 16384 /* a power of two */
#define PROF_SHOWN 200
#define PROF_PERIOD_MS 10000

extern char __ImageBase; /* the linker's: the executable's load address */

/* A slot is free when NULL. Threads take one with a compare-exchange; the sampler frees it once the thread ended */
static HANDLE g_threads[PROF_THREADS];
static int g_threads_used; /* slots ever taken: only grows */

/* the sampler's own (no other thread touches these) */
typedef struct {
    u64 key; /* a game function's host address, or a runtime address rounded to 16 bytes; 0 for a free bucket */
    u32 count;
} ProfBucket;
static ProfBucket g_buckets[PROF_BUCKETS];
static u64 g_samples, g_unplaced;
typedef struct {
    u64 host;
    u64 guest;
} ProfFn;
static ProfFn* g_fns; /* the game module's functions by host address */
static u32 g_nfns;

void plat_prof_add_thread(HANDLE h) {
    HANDLE own;
    int i, used;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &own,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                         FALSE, 0))
        return;
    for (i = 0; i < PROF_THREADS; i++) {
        HANDLE none = NULL;
        if (__atomic_compare_exchange_n(&g_threads[i], &none, own, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
            while (used < i + 1 &&
                   !__atomic_compare_exchange_n(&g_threads_used, &used, i + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            }
            return;
        }
    }
    CloseHandle(own); /* all taken: this one goes unsampled */
}

static int fn_cmp(const void* a, const void* b) {
    u64 x = ((const ProfFn*)a)->host, y = ((const ProfFn*)b)->host;
    return x < y ? -1 : x > y;
}
static void fns_index(void) {
    u32 i;
    if (!g_mod || !g_mod->nentries || !g_mod->entry_fn) return;
    g_fns = malloc((size_t)g_mod->nentries * sizeof *g_fns);
    if (!g_fns) return;
    for (i = 0; i < g_mod->nentries; i++) {
        g_fns[i].host = (u64)(size_t)g_mod->entry_fn[i];
        g_fns[i].guest = g_mod->entry_addr[i];
    }
    qsort(g_fns, g_mod->nentries, sizeof *g_fns, fn_cmp);
    g_nfns = g_mod->nentries;
}
/* the game function pc is in: the closest start at or below it, within 1 MiB */
static const ProfFn* fn_at(u64 pc) {
    u32 lo = 0, hi = g_nfns;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_fns[mid].host <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo && pc - g_fns[lo - 1].host < (1u << 20) ? &g_fns[lo - 1] : NULL;
}

/* the last samples, for plat_prof_spike: when, which thread slot, and the key record gave. The sampler's own */
#define PROF_RING 131072 /* a power of two: about 2 s of 64 threads */
static struct {
    u64 ns, key;
    u32 thread;
} g_ring[PROF_RING];
static u32 g_ring_at;
/* a spike to report (plat_prof_spike): set by the frame loop, read and cleared by the sampler */
static u64 g_spike_from, g_spike_to, g_spike_frame;
static const char* g_spike_what;
static int g_spike_req;

#if !defined(_M_ARM64) && !defined(__aarch64__)
/* SNX_PROF_CALLERS: a sample outside every game function but inside the game module (its helpers: soft float,
   SIMD) counts for the game function that called it, found by unwinding with the module's .pdata, tagged with bit 0.
   RtlLookupFunctionEntry reads the loader's function table: a debugging option, since the stopped thread could in
   principle hold that lock */
static u64 caller_of(CONTEXT* c) {
    CONTEXT u = *c;
    int depth;
    if (fn_at(c->Rip)) return c->Rip;
    for (depth = 0; depth < 4; depth++) {
        DWORD64 base = 0;
        PVOID handler = NULL;
        DWORD64 frame = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(u.Rip, &base, NULL);
        const ProfFn* f;
        if (!rf) return c->Rip; /* a leaf, or code without unwind data */
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, rf, &u, &handler, &frame, NULL);
        f = fn_at(u.Rip);
        if (f) return f->host | 1;
    }
    return c->Rip;
}
#endif
static u64 record(u64 pc) {
    const ProfFn* f = (pc & 1) ? NULL : fn_at(pc);
    u64 key = (pc & 1) ? pc : f ? f->host : pc & ~(u64)15;
    u32 i, probe;
    g_samples++;
    if (!key) key = 16; /* 0 marks a free bucket */
    i = (u32)((key >> 4) * 0x9E3779B1u) & (PROF_BUCKETS - 1);
    for (probe = 0; probe < 64; probe++, i = (i + 1) & (PROF_BUCKETS - 1)) {
        if (g_buckets[i].key == key || !g_buckets[i].key) {
            g_buckets[i].key = key;
            g_buckets[i].count++;
            return key;
        }
    }
    g_unplaced++;
    return key;
}
/* key as write_profile names it: a game function's guest address, the runtime's offset, or a host address */
static void key_text(u64 key, char* out, size_t n) {
    u64 image = (u64)(size_t)&__ImageBase;
    const ProfFn* fn = fn_at(key & ~(u64)1);
    if (fn && (key & 1) && fn->host == (key & ~(u64)1))
        snprintf(out, n, "helpers called from game function %llx", (unsigned long long)fn->guest);
    else if (fn && fn->host == key)
        snprintf(out, n, "game function %llx", (unsigned long long)fn->guest);
    else if (key >= image && key - image < 0x10000000u)
        snprintf(out, n, "runtime +%llx", (unsigned long long)(key - image));
    else { /* a system or driver library's: its name and the offset in it */
        HMODULE m = NULL;
        char path[MAX_PATH];
        const char* base;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(size_t)key, &m) &&
            GetModuleFileNameA(m, path, sizeof path)) {
            base = strrchr(path, '\\');
            snprintf(out, n, "%s +%llx", base ? base + 1 : path, (unsigned long long)(key - (u64)(size_t)m));
        } else
            snprintf(out, n, "host %llx", (unsigned long long)key);
    }
}
void plat_prof_spike(u64 from_ns, u64 to_ns, u64 frame, const char* what) {
    if (__atomic_load_n(&g_spike_req, __ATOMIC_ACQUIRE)) return; /* one at a time */
    g_spike_what = what;
    g_spike_from = from_ns;
    g_spike_to = to_ns;
    g_spike_frame = frame;
    __atomic_store_n(&g_spike_req, 1, __ATOMIC_RELEASE);
}
/* the sampler, between rounds (no thread suspended): each thread's samples in the spike, and its top functions */
static void spike_report(void) {
    enum { CELLS = 1024, TOP = 24 };
    static struct {
        u64 key;
        u32 thread, count;
    } cell[CELLS];
    static u32 per_thread[PROF_THREADS];
    u32 i, n = 0, k;
    char text[64];
    memset(per_thread, 0, sizeof per_thread);
    for (i = 0; i < PROF_RING; i++) {
        u32 t = g_ring[i].thread;
        if (g_ring[i].ns < g_spike_from || g_ring[i].ns > g_spike_to || t >= PROF_THREADS) continue;
        per_thread[t]++;
        for (k = 0; k < n && (cell[k].key != g_ring[i].key || cell[k].thread != t); k++) {}
        if (k == n && n < CELLS) cell[n++] = (typeof(cell[0])){g_ring[i].key, t, 0};
        if (k < n) cell[k].count++;
    }
    LOG("profile: frame %llu, %.1f ms %s, by thread (1 ms samples):\n", (unsigned long long)g_spike_frame,
        (double)(g_spike_to - g_spike_from) / 1e6, g_spike_what);
    for (i = 0; i < PROF_THREADS; i++) {
        HANDLE h = __atomic_load_n(&g_threads[i], __ATOMIC_ACQUIRE);
        u32 shown, most = 0;
        if (per_thread[i] < 10) continue;
        /* a thread that sat at one place outside the game and the runtime the whole time was waiting: not shown */
        for (k = 0; k < n; k++)
            if (cell[k].thread == i && (most == 0 || cell[k].count > cell[most - 1].count)) most = k + 1;
        if (most && cell[most - 1].count * 10 >= per_thread[i] * 9 && !fn_at(cell[most - 1].key & ~(u64)1) &&
            cell[most - 1].key - (u64)(size_t)&__ImageBase >= 0x10000000u)
            continue;
        LOG("profile:   thread %lu, %u samples:\n", h ? (unsigned long)GetThreadId(h) : 0ul, per_thread[i]);
        for (shown = 0; shown < TOP; shown++) { /* the largest left for this thread, taken out as it is shown */
            u32 best = CELLS;
            for (k = 0; k < n; k++)
                if (cell[k].thread == i && cell[k].count && (best == CELLS || cell[k].count > cell[best].count))
                    best = k;
            if (best == CELLS) break;
            key_text(cell[best].key, text, sizeof text);
            LOG("profile:     %4u  %s\n", cell[best].count, text);
            cell[best].count = 0;
        }
    }
}

static int bucket_cmp(const void* a, const void* b) {
    u32 x = ((const ProfBucket*)a)->count, y = ((const ProfBucket*)b)->count;
    return x > y ? -1 : x < y;
}
static void write_profile(u64 started) {
    static ProfBucket sorted[PROF_BUCKETS];
    char path[1200];
    u64 image = (u64)(size_t)&__ImageBase;
    FILE* f;
    int i, n = 0;
    snprintf(path, sizeof path, "%sprofile.txt", g_exedir);
    f = fopen(path, "w");
    if (!f) return;
    for (i = 0; i < PROF_BUCKETS; i++)
        if (g_buckets[i].key) sorted[n++] = g_buckets[i];
    qsort(sorted, (size_t)n, sizeof *sorted, bucket_cmp);
    fprintf(f, "ScarletNX profile: %llu samples in %.0f s, %d thread slots used, %llu not placed\n",
            (unsigned long long)g_samples, (double)(plat_ns() - started) / 1e9,
            __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE), (unsigned long long)g_unplaced);
    for (i = 0; i < n && i < PROF_SHOWN; i++) {
        const ProfFn* fn = fn_at(sorted[i].key);
        double pct = g_samples ? 100.0 * sorted[i].count / (double)g_samples : 0;
        if (fn && fn->host == sorted[i].key)
            fprintf(f, "%6.2f%% %8u  game function %llx\n", pct, sorted[i].count, (unsigned long long)fn->guest);
        else if (sorted[i].key >= image && sorted[i].key - image < 0x10000000u)
            fprintf(f, "%6.2f%% %8u  runtime +%llx\n", pct, sorted[i].count,
                    (unsigned long long)(sorted[i].key - image));
        else
            fprintf(f, "%6.2f%% %8u  host %llx\n", pct, sorted[i].count, (unsigned long long)sorted[i].key);
    }
    fclose(f);
}

static DWORD WINAPI sampler(LPVOID unused) {
    u64 started = plat_ns(), next_write = started + (u64)PROF_PERIOD_MS * 1000000u;
    const char* e = getenv("SNX_PROF_CALLERS");
    int callers = e && *e && strcmp(e, "0");
    (void)unused;
    (void)callers; /* x86-64 only */
    fns_index();
    for (;;) {
        int i, used = __atomic_load_n(&g_threads_used, __ATOMIC_ACQUIRE);
        Sleep(1);
        for (i = 0; i < used; i++) {
            HANDLE h = __atomic_load_n(&g_threads[i], __ATOMIC_ACQUIRE);
            CONTEXT c;
            if (!h) continue;
            if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { /* the thread ended: its slot is free again */
                __atomic_store_n(&g_threads[i], NULL, __ATOMIC_RELEASE);
                CloseHandle(h);
                continue;
            }
            if (SuspendThread(h) == (DWORD)-1) continue;
            /* nothing here may allocate or lock: the stopped thread could hold the heap's lock */
            memset(&c, 0, sizeof c);
            c.ContextFlags = callers ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
            if (GetThreadContext(h, &c)) {
#if defined(_M_ARM64) || defined(__aarch64__)
                u64 key = record((u64)c.Pc);
#else
                u64 key = record(callers ? caller_of(&c) : (u64)c.Rip);
#endif
                u32 at = g_ring_at++ & (PROF_RING - 1);
                g_ring[at].ns = plat_ns();
                g_ring[at].key = key;
                g_ring[at].thread = (u32)i;
            }
            ResumeThread(h);
        }
        if (__atomic_load_n(&g_spike_req, __ATOMIC_ACQUIRE)) {
            spike_report();
            __atomic_store_n(&g_spike_req, 0, __ATOMIC_RELEASE);
        }
        if (plat_ns() >= next_write) {
            write_profile(started);
            next_write += (u64)PROF_PERIOD_MS * 1000000u;
        }
    }
}

void plat_prof_start(void) {
    HANDLE h;
    plat_prof_add_thread(GetCurrentThread()); /* the main thread (the window and the frame loop) */
    h = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (!h) {
        WARN("profile: the sampling thread could not start\n");
        return;
    }
    SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
    CloseHandle(h);
}
