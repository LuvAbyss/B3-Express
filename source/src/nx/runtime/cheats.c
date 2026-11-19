/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The cheats turned on, run on the thread that shows the game's frame, as Atmosphère's dmnt runs them beside the
   game: their reads and writes go to guest memory the game has mapped, readable for a read and writable for a write,
   so a pointer a cheat follows into nothing is ignored instead of crashing the game */
#include "nx/runtime/cheats.h"
#include "nx/hle/services/hid.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

#define MAX_PROGRAMS 8

static struct {
    SnxCheatProgram p;
    u64 main, text, text_end;
} g_progs[MAX_PROGRAMS];
static int g_nprogs;

int cheats_add(SnxCheatProgram* p, u64 main, u64 text, u64 text_size) {
    if (!p->n || g_nprogs == MAX_PROGRAMS) return 0;
    g_progs[g_nprogs].p = *p;
    g_progs[g_nprogs].main = main;
    g_progs[g_nprogs].text = text;
    g_progs[g_nprogs].text_end = text + text_size;
    g_nprogs++;
    return 1;
}

/* size bytes at addr are in one mapped block with perm */
static int mapped(u64 addr, size_t size, u32 perm) {
    MemBlock b;
    if (addr >= AS_END || size > AS_END - addr) return 0;
    mem_query(addr, &b);
    return b.state != MS_FREE && b.state != MS_INACCESSIBLE && (b.perm & perm) == perm &&
           addr + size <= b.addr + b.size;
}

static int cheat_read(void* ctx, u64 addr, void* out, size_t size) {
    (void)ctx;
    if (!mapped(addr, size, PERM_R)) return -1;
    memcpy(out, GP(addr), size);
    return 0;
}

static int cheat_write(void* ctx, u64 addr, const void* in, size_t size) {
    const u64* code = ctx; /* the main module's code: [0] to [1] */
    if (!mapped(addr, size, PERM_W)) {
        /* code that holds these bytes already: a write every run makes, compiled in (snx_patch_cheats) */
        if (addr < code[1] && addr + size > code[0] && mapped(addr, size, PERM_R) && !memcmp(GP(addr), in, size))
            return 0;
        if (addr < code[1] && addr + size > code[0])
            WARN_LIMIT(1, "cheats: a cheat writes the game code under a condition or through a pointer; the code is "
                          "compiled, so that write does nothing\n");
        return -1;
    }
    memcpy(GP(addr), in, size);
    return 0;
}

/* 0xFFF: into the game's log when the value changed (Atmosphère writes a file per id instead) */
static void cheat_log(void* ctx, unsigned id, u64 value) {
    static u64 last[16];
    static u16 said;
    (void)ctx;
    if (said >> id & 1 && last[id] == value) return;
    said |= (u16)(1u << id);
    last[id] = value;
    LOG("cheats: log %u: 0x%llx (%llu)\n", id, (unsigned long long)value, (unsigned long long)value);
}

void cheats_frame(void) {
    for (int i = 0; i < g_nprogs; i++) {
        u64 code[2] = {g_progs[i].text, g_progs[i].text_end};
        SnxCheatEnv e;
        e.base[SNX_CHEAT_MAIN] = g_progs[i].main;
        e.base[SNX_CHEAT_HEAP] = HEAP_BASE;
        e.base[SNX_CHEAT_ALIAS] = ALIAS_BASE;
        e.base[SNX_CHEAT_ASLR] = AS_BASE;
        e.base[SNX_CHEAT_NONE] = 0;
        e.keys = hid_held();
        e.ctx = code;
        e.read = cheat_read;
        e.write = cheat_write;
        e.log = cheat_log;
        snx_cheat_run(&g_progs[i].p, &e);
    }
}
