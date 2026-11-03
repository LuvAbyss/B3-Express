/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The seam between the game and NVN (nvn.h): which of the game's indirect calls are NVN entry points, and the host
   handler each runs. Phase 1 of diagnostics/nvn-route-plan-20261029/PLAN.md: every handler is the game's own driver,
   so routing changes nothing the game sees */
#include <stdlib.h>
#include <string.h>
#include "nx/hle/nvn/nvn.h"
#include "nx/hle/nvn/cmdlist.h"
#include "nx/hle/nvn/shadow.h"
#include "nx/hle/nvn/kick.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

/* host handlers by NVN function name: none yet, every function runs the game's driver */
static const struct {
    const char* name;
    NvnHandler handler;
} g_handlers[] = {{NULL, NULL}};

#define NVN_SLOTS 4096u /* a power of two above the ~540 entry points the SDK names */
static NvnFn g_fn[NVN_SLOTS];
static PMutex g_lk = PMUTEX_INIT;
/* SNX_NVN_TRACE, SNX_NVN_SHADOW (shadow.c's handlers), SNX_NVN_CMDLIST (cmdlist.c's, ahead of the shadow's) */
static int g_trace, g_shadow, g_cmdlist;
static int g_slow; /* SNX_NVN_SLOW=1: calls of 50 ms or more are logged (nvn_entry) */
static u32 g_routed;
static u64 g_total, g_next_dump = 1u << 24;
static GuestFn g_boot_guest, g_getproc_guest;
static u64 g_getproc;

/* the length of the guest string at a, at most max, when it is readable and ends within max; else 0 */
static int gstr(u64 a, int max) {
    int n = 0;
    MemBlock b;
    if (!g_mem || a < AS_BASE || a >= AS_END) return 0;
    mem_query(a, &b);
    if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !b.size) return 0;
    while (n < max && a + (u64)n < b.addr + b.size && GP(a)[n]) {
        if (GP(a)[n] < 32 || GP(a)[n] >= 127) return 0;
        n++;
    }
    return n < max && a + (u64)n < b.addr + b.size ? n : 0;
}
static int named(u64 name_addr, const char* want) {
    size_t n = strlen(want);
    return gstr(name_addr, (int)n + 1) == (int)n && !memcmp(GP(name_addr), want, n);
}

static NvnFn* fn_find(u64 pc) {
    u32 i = (u32)(pc >> 2) & (NVN_SLOTS - 1), n;
    for (n = 0; n < NVN_SLOTS; n++, i = (i + 1) & (NVN_SLOTS - 1)) {
        u64 at = __atomic_load_n(&g_fn[i].pc, __ATOMIC_ACQUIRE);
        if (at == pc || !at) return at ? &g_fn[i] : NULL;
    }
    return NULL;
}
const NvnFn* nvn_lookup(u64 pc) { return fn_find(pc); }
u32 nvn_routed(void) { return __atomic_load_n(&g_routed, __ATOMIC_RELAXED); }
void nvn_guest(CPU* RESTRICT c, const NvnFn* f) { f->guest(c, f->pc); }

/* the routed functions by name, for nvn_call */
static NvnFn* g_byname[NVN_SLOTS];
static u64 g_device; /* the device the game first passed GetProcAddress, for resolving names it never asked for */
static u32 name_hash(const char* s) {
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}
static NvnFn* by_name(const char* name) {
    u32 i = name_hash(name) & (NVN_SLOTS - 1), n;
    for (n = 0; n < NVN_SLOTS; n++, i = (i + 1) & (NVN_SLOTS - 1)) {
        NvnFn* f = __atomic_load_n(&g_byname[i], __ATOMIC_ACQUIRE);
        if (!f || !strcmp(f->name, name)) return f;
    }
    return NULL;
}
static void by_name_add(NvnFn* f) { /* g_lk held */
    u32 i = name_hash(f->name) & (NVN_SLOTS - 1), n;
    for (n = 0; n < NVN_SLOTS && g_byname[i]; n++) i = (i + 1) & (NVN_SLOTS - 1);
    if (n < NVN_SLOTS) __atomic_store_n(&g_byname[i], f, __ATOMIC_RELEASE);
}
/* the guest function fn at pc with args in x0.., on c's thread, returning its x0. Every register it may change is
   put back (x0-x31, pc, NZCV, v0-v31, FPCR, FPSR: CPU's first bytes). x30 is 0, so its RET leaves c->pc 0, which
   nothing reads before the registers are back. SP first drops by reserve, below data the caller put there */
static u64 guest_call(CPU* RESTRICT c, u64 pc, GuestFn fn, const u64* args, u32 nargs, u64 reserve) {
    u8 saved[offsetof(CPU, tpidr)];
    u64 r;
    u32 i;
    memcpy(saved, c, sizeof saved);
    for (i = 0; i < nargs && i < 8; i++) c->x[i] = args[i];
    c->x[30] = 0;
    c->x[31] = (c->x[31] - reserve) & ~(u64)15;
    fn(c, pc);
    r = c->x[0];
    memcpy(c, saved, sizeof saved);
    return r;
}
static void route(u64 pc, u64 name_addr, GuestFn via);
static void nvn_entry(CPU* RESTRICT c, u64 pc);
int nvn_call(CPU* RESTRICT c, const char* name, const u64* args, u32 nargs, u64* ret) {
    NvnFn* f = by_name(name);
    if (!f && g_getproc && g_device) { /* the driver's GetProcAddress, with the name on the guest stack */
        size_t n = strlen(name);
        u64 sp = c->x[31], at = (sp - 128) & ~(u64)15, ga[2], pc;
        if (n >= 63 || at < AS_BASE) return 0;
        memcpy(GP(at), name, n + 1);
        ga[0] = g_device;
        ga[1] = at;
        pc = guest_call(c, g_getproc, g_getproc_guest, ga, 2, sp - at + 64);
        if (pc) route(pc, at, nvn_entry);
        f = by_name(name);
    }
    if (!f) return 0;
    *ret = guest_call(c, f->pc, f->guest, args, nargs, 0);
    return 1;
}
int nvn_call_below(CPU* RESTRICT c, const char* name, const u64* args, u32 nargs, u64* ret, u64 reserve) {
    u64 sp = c->x[31];
    int ok;
    c->x[31] = (sp - reserve) & ~(u64)15; /* every frame of the call, the name lookup's too, below the caller's data */
    ok = nvn_call(c, name, args, nargs, ret);
    c->x[31] = sp;
    return ok;
}

/* ------------------------------------------------------------ the trace (SNX_NVN_TRACE=1) */
/* nvnSyncWait(sync, timeout ns) by calling thread: calls, ns inside, results (0 already signaled, 1 satisfied,
   2 timeout, 3 failed) and timeouts (0, below 1 ms, 1 ms or more) */
typedef struct {
    u64 tid, calls, ns, result[4], timeout[3];
    int core;
    char name[32];
} SyncWaiter;
#define WAITERS 32
static SyncWaiter g_waiter[WAITERS];

static void trace_dump(void) {
    u32 i, called = 0;
    for (i = 0; i < NVN_SLOTS; i++)
        if (g_fn[i].pc && __atomic_load_n(&g_fn[i].calls, __ATOMIC_RELAXED)) called++;
    LOG("nvn-trace: so far: %u functions called, %llu calls\n", called, (unsigned long long)g_total);
    for (i = 0; i < WAITERS; i++)
        if (g_waiter[i].calls)
            LOG("nvn-trace: syncwait thread %llu '%s' core %d: %llu calls, %llu ms inside, results %llu/%llu/%llu/%llu, "
                "timeouts zero %llu, <1ms %llu, >=1ms %llu\n",
                (unsigned long long)g_waiter[i].tid, g_waiter[i].name, g_waiter[i].core,
                (unsigned long long)g_waiter[i].calls, (unsigned long long)(g_waiter[i].ns / 1000000),
                (unsigned long long)g_waiter[i].result[0], (unsigned long long)g_waiter[i].result[1],
                (unsigned long long)g_waiter[i].result[2], (unsigned long long)g_waiter[i].result[3],
                (unsigned long long)g_waiter[i].timeout[0], (unsigned long long)g_waiter[i].timeout[1],
                (unsigned long long)g_waiter[i].timeout[2]);
    for (i = 0; i < NVN_SLOTS; i++)
        if (g_fn[i].pc && g_fn[i].calls)
            LOG("nvn-trace:   %llu %s\n", (unsigned long long)g_fn[i].calls, g_fn[i].name);
}
static void trace_sync_wait(CPU* RESTRICT c, NvnFn* f) {
    KThread* t = kthread_cur();
    u64 timeout = c->x[1], t0 = plat_ns(), tid = t ? t->id : 0;
    SyncWaiter* w = NULL;
    u32 i;
    f->handler(c, f);
    pm_lock(&g_lk); /* the waiters are few: a lock per call is cheap next to the call */
    for (i = 0; i < WAITERS && !w; i++)
        if (g_waiter[i].calls && g_waiter[i].tid == tid)
            w = &g_waiter[i];
        else if (!g_waiter[i].calls) {
            w = &g_waiter[i];
            w->tid = tid;
            if (t) memcpy(w->name, t->name, sizeof w->name - 1);
        }
    if (w) {
        w->calls++;
        w->ns += plat_ns() - t0;
        w->core = t ? t->core : -1;
        w->result[c->x[0] & 3]++;
        w->timeout[!timeout ? 0 : timeout < 1000000 ? 1 : 2]++;
    }
    pm_unlock(&g_lk);
}

/* ------------------------------------------------------------ routing */
/* what g_disp runs for every routed function */
static void nvn_entry(CPU* RESTRICT c, u64 pc) {
    NvnFn* f = fn_find(pc);
    u64 n;
    if (!f) { /* not one of ours: the interpreter finds the function */
        snx_interp_entry(c, pc);
        return;
    }
    n = __atomic_fetch_add(&f->calls, 1, __ATOMIC_RELAXED);
    if (g_trace) {
        u64 total = __atomic_add_fetch(&g_total, 1, __ATOMIC_RELAXED);
        if (!n) LOG("nvn-trace: first call %s\n", f->name);
        if (total == __atomic_load_n(&g_next_dump, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_next_dump, total + (1u << 24), __ATOMIC_RELAXED);
            trace_dump();
        }
        if (f->sync_wait) {
            trace_sync_wait(c, f);
            return;
        }
    }
    if (g_kick_on) {
        const char* prev = kick_enter(f, c);
        f->handler(c, f);
        kick_leave(f, prev);
        return;
    }
    if (__atomic_load_n(&g_slow, __ATOMIC_RELAXED)) { /* SNX_NVN_SLOW=1 (a test): calls of 50 ms or more, by name */
        u64 t0 = plat_ns(), a0 = c->x[0], a1 = c->x[1], ns;
        f->handler(c, f);
        ns = plat_ns() - t0;
        if (ns >= 50000000)
            LOG_LIMIT(200, "nvn-slow: %s(%llx, %llx) took %.1f ms\n", f->name, (unsigned long long)a0,
                      (unsigned long long)a1, (double)ns / 1e6);
        return;
    }
    f->handler(c, f);
}
static NvnHandler handler_for(const char* name, int* op) {
    u32 i;
    NvnHandler h = g_cmdlist ? nvn_cmd_handler(name, op) : NULL;
    if (h) return h;
    *op = 0;
    h = g_shadow ? nvn_shadow_handler(name) : NULL;
    if (h) return h;
    for (i = 0; g_handlers[i].name; i++)
        if (!strcmp(g_handlers[i].name, name)) return g_handlers[i].handler;
    return nvn_guest;
}
/* routes pc, which the game was handed under the name at name_addr, through via (nvn_entry, or the GetProcAddress
   wrapper). The same function under a second name keeps its first */
static void route(u64 pc, u64 name_addr, GuestFn via) {
    GuestFn* slot;
    NvnFn* f;
    u32 i, n;
    if (pc < g_code_lo || pc >= g_code_hi || (pc & 3)) return;
    slot = &g_disp[(pc - g_code_lo) / 4];
    pm_lock(&g_lk);
    if (fn_find(pc)) {
        pm_unlock(&g_lk);
        return;
    }
    i = (u32)(pc >> 2) & (NVN_SLOTS - 1);
    for (n = 0; n < NVN_SLOTS && g_fn[i].pc; n++) i = (i + 1) & (NVN_SLOTS - 1);
    if (n < NVN_SLOTS) {
        f = &g_fn[i];
        f->guest = *slot ? *slot : snx_interp_entry;
        n = (u32)gstr(name_addr, (int)sizeof f->name - 1);
        if (n) memcpy(f->name, GP(name_addr), n);
        f->name[n] = 0;
        f->handler = handler_for(f->name, &f->op);
        f->sync_wait = !strcmp(f->name, "nvnSyncWait");
        __atomic_store_n(&f->pc, pc, __ATOMIC_RELEASE);
        if (f->name[0]) by_name_add(f);
        __atomic_store_n(slot, via, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_routed, 1, __ATOMIC_RELAXED);
    }
    pm_unlock(&g_lk);
}
static void nvn_getproc(CPU* RESTRICT c, u64 pc) { /* nvnDeviceGetProcAddress(device, name) */
    u64 name = c->x[1];
    if (!g_device && c->x[0]) g_device = c->x[0];
    g_getproc_guest(c, pc);
    if (c->x[0]) route(c->x[0], name, nvn_entry);
}
static void nvn_boot(CPU* RESTRICT c, u64 pc) { /* nvnBootstrapLoader(name) */
    u64 name = c->x[0], fn;
    int len = gstr(name, 63);
    g_boot_guest(c, pc);
    fn = c->x[0];
    LOG("nvn: bootstrap %.*s -> %llx\n", len, len ? (const char*)GP(name) : "", (unsigned long long)fn);
    if (!fn || fn < g_code_lo || fn >= g_code_hi || (fn & 3)) return;
    if (named(name, "nvnDeviceGetProcAddress")) {
        if (!g_getproc) {
            GuestFn* slot = &g_disp[(fn - g_code_lo) / 4];
            g_getproc = fn;
            g_getproc_guest = *slot ? *slot : snx_interp_entry;
            __atomic_store_n(slot, nvn_getproc, __ATOMIC_RELEASE);
        }
    } else
        route(fn, name, nvn_entry);
}

void nvn_init(void) {
    const char* front = getenv("SNX_NVN_FRONTEND");
    const char* trace = getenv("SNX_NVN_TRACE");
    const char* shadow = getenv("SNX_NVN_SHADOW");
    const char* cmdlist = getenv("SNX_NVN_CMDLIST");
    u64 boot;
    g_trace = trace && trace[0] == '1';
    g_slow = getenv("SNX_NVN_SLOW") && getenv("SNX_NVN_SLOW")[0] == '1';
    g_shadow = shadow && shadow[0] == '1';
    g_cmdlist = (cmdlist && cmdlist[0] == '1') || (front && front[0] == '2');
    kick_init();
    if (!g_trace && !g_shadow && !g_cmdlist && !g_kick_on && !(front && front[0] == '1')) return;
    if (!g_disp) return;
    boot = kernel_symbol_addr("nvnBootstrapLoader");
    if (!boot || boot < g_code_lo || boot >= g_code_hi || (boot & 3)) {
        LOG("nvn: nvnBootstrapLoader not found, NVN calls are not routed\n");
        return;
    }
    g_boot_guest = g_disp[(boot - g_code_lo) / 4] ? g_disp[(boot - g_code_lo) / 4] : snx_interp_entry;
    g_disp[(boot - g_code_lo) / 4] = nvn_boot;
    LOG("nvn: routing NVN calls%s%s, nvnBootstrapLoader at %llx\n", g_trace ? " (traced)" : "",
        g_shadow ? " (shadowed)" : g_cmdlist ? " (host command lists)" : "", (unsigned long long)boot);
}
