/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Phase 4 step 4.0 (kick.h): proves where the NVN driver kicks its GPFIFO and lists the release methods of what it
   kicked. nvn_entry tells kick_enter/kick_leave which routed function runs on each thread; nvdrv's SUBMIT_GPFIFO
   reports each submission here with the function running on its thread; the GPU thread reports every release method
   it performs with the job it belongs to. Counts by submitting function and by release kind and channel every 600
   presents; every queue/window call, submission and release of three frames in order */
#include <stdlib.h>
#include <string.h>
#include "nx/hle/nvn/kick.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

int g_kick_on;
static u64 g_first = ~0ull; /* the first traced frame */
static u64 g_frames;        /* presents so far */
static __thread const char* tls_call;
static __thread u32 tls_depth;
static PMutex g_lk = PMUTEX_INIT;

#define CALLERS 16
static const char* g_caller[CALLERS]; /* the outermost NVN call running when a submission was made, NULL for none */
static u64 g_caller_n[CALLERS], g_caller_inner[CALLERS];
static u64 g_submits, g_outside;
static u64 g_rel[128][KR_KINDS][2]; /* by channel, kind and inside a macro */
static u64 g_chan_submits[128], g_chan_nvn[128];
static const char* const g_kind[KR_KINDS] = {
    "puller semaphore release 4B", "puller semaphore release 16B", "puller semaphore acquire",
    "puller semaphore reduction",  "puller syncpoint increment",   "puller syncpoint wait",
    "host increment after job",    "3D SYNC_INFO increment",       "3D report release",
    "3D report counter",           "3D report acquire",            "compute report release",
    "compute report counter",      "compute report acquire",       "copy engine semaphore",
    "inline-to-memory launch",     "copy engine launch",           "2D blit",
    "compute launch"};

/* the traced frames' events */
#define EVENTS 32768u
typedef struct {
    u64 t;
    const void* thr;
    const char* what; /* a function's name, "submit" or a release kind */
    u64 a, b, c, d;
    int kind; /* 0 enter, 1 leave, 2 submit, 3 release */
} KickEvent;
static KickEvent* g_ev;
static u32 g_nev;
#define JOBS 4096u
static u64 g_job[JOBS]; /* the jobs submitted in the traced frames */
static u32 g_njob;
static int g_tracing, g_dumped;

static void kick_submit(u32 sp, u32 num, u32 flags, u32 fence, u64 job);
static void kick_release(u32 ch, u64 job, int kind, int in_macro, u32 entry, u32 entries, u64 detail);
void kick_init(void) {
    const char* e = getenv("SNX_NVN_KICK");
    if (!e || !e[0]) return;
    g_first = strtoull(e, NULL, 0);
    g_ev = (KickEvent*)calloc(EVENTS, sizeof *g_ev);
    g_kick_on = 1;
    g_nvdrv_submit_note = kick_submit;
    g_gpu_release_note = kick_release;
    LOG("nvn-kick: tracing submissions; events of frames %llu to %llu\n", (unsigned long long)g_first,
        (unsigned long long)g_first + 2);
}

static void event(int kind, const char* what, u64 a, u64 b, u64 c) { /* g_lk held */
    KickEvent* v;
    if (!g_tracing || !g_ev || g_nev >= EVENTS) return;
    v = &g_ev[g_nev++];
    v->t = plat_ns();
    v->thr = tls_thread;
    v->what = what;
    v->a = a;
    v->b = b;
    v->c = c;
    v->kind = kind;
}
static int traced_job(u64 job) { /* g_lk held */
    u32 i;
    for (i = 0; i < g_njob; i++)
        if (g_job[i] == job) return 1;
    return 0;
}
static int queue_fn(const char* n) {
    return !strncmp(n, "nvnQueue", 8) || !strncmp(n, "nvnWindow", 9) || !strcmp(n, "nvnCommandBufferFenceSync");
}

static void dump_counts(void) { /* g_lk held */
    u32 i, k, ch;
    LOG("nvn-kick: %llu presents, %llu SUBMIT_GPFIFO; with no NVN call on the thread %llu\n",
        (unsigned long long)g_frames, (unsigned long long)g_submits, (unsigned long long)g_outside);
    for (i = 0; i < CALLERS && g_caller[i]; i++)
        LOG("nvn-kick:   made in %s: %llu (in a nested call %llu)\n", g_caller[i], (unsigned long long)g_caller_n[i],
            (unsigned long long)g_caller_inner[i]);
    for (ch = 0; ch < 128; ch++) {
        if (!g_chan_submits[ch]) continue;
        LOG("nvn-kick:   channel %u: %llu submissions, %llu from NVN calls\n", ch,
            (unsigned long long)g_chan_submits[ch], (unsigned long long)g_chan_nvn[ch]);
        for (k = 0; k < KR_KINDS; k++)
            if (g_rel[ch][k][0] || g_rel[ch][k][1])
                LOG("nvn-kick:     %-30s %10llu  in macros %llu\n", g_kind[k], (unsigned long long)g_rel[ch][k][0],
                    (unsigned long long)g_rel[ch][k][1]);
    }
}
static void dump_events(void) { /* g_lk held */
    u32 i;
    u64 t0 = g_nev ? g_ev[0].t : 0;
    LOG("nvn-kick: %u events of frames %llu-%llu (time us, thread, event)\n", g_nev, (unsigned long long)g_first,
        (unsigned long long)g_first + 2);
    for (i = 0; i < g_nev; i++) {
        const KickEvent* v = &g_ev[i];
        u64 us = (v->t - t0) / 1000;
        switch (v->kind) {
            case 0: LOG("nvn-kick: %8llu %p > %s(%llx, %llx)\n", (unsigned long long)us, v->thr, v->what,
                        (unsigned long long)v->a, (unsigned long long)v->b); break;
            case 1: LOG("nvn-kick: %8llu %p < %s\n", (unsigned long long)us, v->thr, v->what); break;
            case 2:
                LOG("nvn-kick: %8llu %p   SUBMIT_GPFIFO in %s: channel %llu, %llu entries, flags %llx, job %llu\n",
                    (unsigned long long)us, v->thr, v->what, v->a & 0xFFFF, (v->a >> 16) & 0xFFFF, v->a >> 32,
                    (unsigned long long)v->c);
                break;
            default:
                LOG("nvn-kick: %8llu %p     GPU: %s, channel %llu, job %llu, entry %llu of %llu, detail %llx%s\n",
                    (unsigned long long)us, v->thr, v->what, (unsigned long long)v->a, (unsigned long long)v->b,
                    (unsigned long long)((v->c >> 8) & 0xFFFFFF), (unsigned long long)(v->c >> 32),
                    (unsigned long long)v->d, v->c & 1 ? " (macro)" : "");
                break;
        }
    }
}

const char* kick_enter(const NvnFn* f, CPU* RESTRICT c) {
    const char* prev = tls_call;
    if (!tls_depth++) tls_call = f->name;
    if (queue_fn(f->name)) {
        pm_lock(&g_lk);
        if (!strcmp(f->name, "nvnQueuePresentTexture")) {
            g_frames++;
            if (g_frames == g_first) g_tracing = 1;
            if (g_frames == g_first + 3) g_tracing = 0;
            if (g_frames == g_first + 6) { /* the GPU thread's releases of the traced jobs are in by now */
                if (!g_dumped) {
                    g_dumped = 1;
                    dump_events();
                }
            }
            if (g_frames % 600 == 0) dump_counts();
        }
        event(0, f->name, c->x[0], c->x[1], 0);
        pm_unlock(&g_lk);
    }
    return prev;
}
void kick_leave(const NvnFn* f, const char* prev) {
    tls_depth--;
    tls_call = prev;
    if (queue_fn(f->name)) {
        pm_lock(&g_lk);
        event(1, f->name, 0, 0, 0);
        pm_unlock(&g_lk);
    }
}
static void kick_submit(u32 sp, u32 num, u32 flags, u32 fence, u64 job) {
    const char* who = tls_call;
    u32 i;
    pm_lock(&g_lk);
    g_submits++;
    if (sp < 128) {
        g_chan_submits[sp]++;
        if (who) g_chan_nvn[sp]++;
    }
    if (!who)
        g_outside++;
    else {
        for (i = 0; i < CALLERS && g_caller[i] && strcmp(g_caller[i], who); i++) continue;
        if (i < CALLERS) {
            g_caller[i] = who;
            g_caller_n[i]++;
            if (tls_depth > 1) g_caller_inner[i]++;
        }
    }
    if (g_tracing) {
        event(2, who ? who : "(no NVN call)", (u64)sp | (u64)(num & 0xFFFF) << 16 | (u64)flags << 32, fence, job);
        if (g_njob < JOBS) g_job[g_njob++] = job;
    }
    pm_unlock(&g_lk);
}
static void kick_release(u32 ch, u64 job, int kind, int in_macro, u32 entry, u32 entries, u64 detail) {
    const void* thr;
    if (kind < 0 || kind >= KR_KINDS) return;
    pm_lock(&g_lk);
    if (ch < 128) g_rel[ch][kind][in_macro != 0]++;
    if (!g_dumped && g_ev && g_nev < EVENTS && traced_job(job)) { /* after the frames too: the GPU thread runs behind */
        KickEvent* v = &g_ev[g_nev++];
        v->t = plat_ns();
        v->thr = tls_thread;
        v->what = g_kind[kind];
        v->a = ch;
        v->b = job;
        v->c = (u64)(in_macro != 0) | (u64)entry << 8 | (u64)entries << 32;
        v->d = detail;
        v->kind = 3;
    }
    pm_unlock(&g_lk);
}
