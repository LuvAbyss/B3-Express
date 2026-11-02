/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hle/nvn's routing (nvn.c, included): with it off g_disp is untouched; on, nvnBootstrapLoader's and
   nvnDeviceGetProcAddress's answers are routed through nvn_entry, which runs the game's function with the same result,
   counts the call and names it; a function is routed once whatever name it comes under, an address outside the code
   or none is left alone, and an unrouted address reaching nvn_entry goes to the interpreter. The game's NVN driver
   is a set of host functions standing in for its code */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx/hle/nvn/nvn.c"

int g_kick_on;
void kick_init(void) {}
const char* kick_enter(const NvnFn* f, CPU* RESTRICT c) {
    (void)f;
    (void)c;
    return NULL;
}
void kick_leave(const NvnFn* f, const char* prev) {
    (void)f;
    (void)prev;
}
NvnHandler nvn_cmd_handler(const char* name, int* op) {
    (void)name;
    (void)op;
    return NULL;
}
NvnHandler nvn_shadow_handler(const char* name) {
    (void)name;
    return NULL;
}

u8* g_mem;
MemLayout g_layout;
GuestFn* g_disp;
u64 g_code_lo, g_code_hi;
static unsigned test_checks;
static int test_locks;
static u64 test_interp_pc;
static int test_foo_runs, test_boot_runs, test_getproc_runs;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "nvn_seam.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_lock(PMutex* m) {
    (void)m;
    test_locks++;
}
void pm_unlock(PMutex* m) {
    (void)m;
    test_locks--;
}
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
u64 plat_ns(void) { return 0; }
KThread* kthread_cur(void) { return NULL; }
void mem_query(u64 addr, MemBlock* out) { /* the whole test address space is one mapped block */
    (void)addr;
    memset(out, 0, sizeof *out);
    out->addr = g_layout.base;
    out->size = g_layout.end - g_layout.base;
    out->state = 3;
}

#define CODE_LO    0x10000ull
#define CODE_HI    0x20000ull
#define BOOT_PC    0x10100ull
#define GETPROC_PC 0x10200ull
#define FOO_PC     0x10300ull
#define WAIT_PC    0x10400ull
#define OTHER_PC   0x10500ull
#define BAR_PC     0x10600ull /* a function the game never asks for */
#define STR        0x30000ull /* guest strings */
#define DEVICE     0xDE00ull
#define STACK      0x80000ull
static u64 test_bar_sp, test_getproc_sp;
u64 kernel_symbol_addr(const char* name) { return !strcmp(name, "nvnBootstrapLoader") ? BOOT_PC : 0; }
static void test_interp(CPU* RESTRICT c, u64 pc) {
    (void)c;
    test_interp_pc = pc;
}
GuestFn snx_interp_entry = test_interp;

static u64 put_str(u64 at, const char* s) {
    memcpy(GP(at), s, strlen(s) + 1);
    return at;
}
static int guest_is(u64 a, const char* s) { return !strcmp((const char*)GP(a), s); }
/* the game's driver: nvnBootstrapLoader hands out GetProcAddress, which hands out the rest */
static void drv_boot(CPU* RESTRICT c, u64 pc) {
    (void)pc;
    test_boot_runs++;
    c->x[0] = guest_is(c->x[0], "nvnDeviceGetProcAddress") ? GETPROC_PC
              : guest_is(c->x[0], "nvnOutside")            ? CODE_HI + 0x100
                                                           : 0;
}
static void drv_getproc(CPU* RESTRICT c, u64 pc) {
    (void)pc;
    test_getproc_runs++;
    test_getproc_sp = c->x[31];
    c->x[0] = guest_is(c->x[1], "nvnFooA") || guest_is(c->x[1], "nvnFooAlias") ? FOO_PC
              : guest_is(c->x[1], "nvnSyncWait")                              ? WAIT_PC
              : guest_is(c->x[1], "nvnBarB")                                  ? BAR_PC
                                                                              : 0;
}
static void drv_bar(CPU* RESTRICT c, u64 pc) { /* x0 + x1, and changes registers a callee may change */
    CHECK(pc == BAR_PC);
    test_bar_sp = c->x[31];
    c->x[0] += c->x[1];
    c->x[5] = 99;
    c->x[30] = 0;
    c->pc = c->x[30];
    c->z = 1;
    c->fpsr = 7;
}
static void drv_foo(CPU* RESTRICT c, u64 pc) {
    CHECK(pc == FOO_PC);
    test_foo_runs++;
    c->x[0] = c->x[0] * 2 + 1;
}
static void drv_wait(CPU* RESTRICT c, u64 pc) {
    (void)pc;
    c->x[0] = 1;
}
static GuestFn* slot(u64 pc) { return &g_disp[(pc - CODE_LO) / 4]; }
static void call(CPU* c, u64 pc) { (*slot(pc))(c, pc); } /* the game's indirect call (CALLDYN) */

int main(void) {
    CPU c;
    const NvnFn* f;
    memset(&c, 0, sizeof c);
    g_layout.base = 0x1000;
    g_layout.end = 0x100000;
    g_mem = calloc(1, (size_t)g_layout.end);
    g_code_lo = CODE_LO;
    g_code_hi = CODE_HI;
    g_disp = calloc((size_t)((CODE_HI - CODE_LO) / 4), sizeof *g_disp);
    CHECK(g_mem && g_disp);
    *slot(BOOT_PC) = drv_boot;
    *slot(GETPROC_PC) = drv_getproc;
    *slot(FOO_PC) = drv_foo;
    *slot(WAIT_PC) = drv_wait;
    *slot(BAR_PC) = drv_bar;

    /* off: nothing routed */
    nvn_init();
    CHECK(*slot(BOOT_PC) == drv_boot && nvn_routed() == 0);

    putenv("SNX_NVN_FRONTEND=1");
    nvn_init();
    CHECK(*slot(BOOT_PC) != drv_boot && !g_trace);

    /* bootstrap -> GetProcAddress: the driver still answers, and GetProcAddress is wrapped, not counted as routed */
    c.x[0] = put_str(STR, "nvnDeviceGetProcAddress");
    call(&c, BOOT_PC);
    CHECK(test_boot_runs == 1 && c.x[0] == GETPROC_PC && *slot(GETPROC_PC) == nvn_getproc && nvn_routed() == 0);

    /* GetProcAddress -> a routed function with the driver's result; the device it was asked with is kept */
    c.x[0] = DEVICE;
    c.x[1] = put_str(STR + 0x40, "nvnFooA");
    call(&c, GETPROC_PC);
    CHECK(test_getproc_runs == 1 && c.x[0] == FOO_PC && *slot(FOO_PC) == nvn_entry && nvn_routed() == 1);
    CHECK(g_device == DEVICE);
    c.x[0] = 20;
    call(&c, FOO_PC);
    CHECK(test_foo_runs == 1 && c.x[0] == 41);
    f = nvn_lookup(FOO_PC);
    CHECK(f && f->calls == 1 && !strcmp(f->name, "nvnFooA") && f->handler == nvn_guest && f->guest == drv_foo);

    /* asked again, or under another name: routed once, first name kept */
    c.x[1] = STR + 0x40;
    call(&c, GETPROC_PC);
    c.x[1] = put_str(STR + 0x80, "nvnFooAlias");
    call(&c, GETPROC_PC);
    CHECK(nvn_routed() == 1 && !strcmp(nvn_lookup(FOO_PC)->name, "nvnFooA"));

    c.x[1] = put_str(STR + 0xC0, "nvnSyncWait");
    call(&c, GETPROC_PC);
    CHECK(nvn_routed() == 2 && nvn_lookup(WAIT_PC)->sync_wait);
    call(&c, WAIT_PC);
    CHECK(c.x[0] == 1 && nvn_lookup(WAIT_PC)->calls == 1);

    /* no function, or one outside the code: nothing routed */
    c.x[1] = put_str(STR + 0x100, "nvnUnknown");
    call(&c, GETPROC_PC);
    c.x[0] = put_str(STR + 0x140, "nvnOutside");
    call(&c, BOOT_PC);
    CHECK(nvn_routed() == 2 && !nvn_lookup(CODE_HI + 0x100) && !nvn_lookup(OTHER_PC));

    /* nvn_entry at an address it never routed hands it to the interpreter */
    nvn_entry(&c, OTHER_PC);
    CHECK(test_interp_pc == OTHER_PC);

    /* nvn_call: a routed function by name, then one the game never asked for (found with GetProcAddress, its name on
       the guest stack below SP, the call's SP below the name), with every register as before afterwards */
    {
        CPU before;
        u64 r = 0, a[2] = {5, 7};
        c.x[31] = STACK;
        c.x[30] = 0x1234;
        c.pc = 0x5678;
        c.n = 1;
        memcpy(&before, &c, sizeof before);
        CHECK(nvn_call(&c, "nvnFooA", a, 1, &r) && r == 11 && test_foo_runs == 2);
        CHECK(!memcmp(&before, &c, offsetof(CPU, tpidr)));
        CHECK(nvn_call(&c, "nvnBarB", a, 2, &r) && r == 12 && nvn_lookup(BAR_PC) && nvn_routed() == 3);
        CHECK(!memcmp(&before, &c, offsetof(CPU, tpidr)));
        CHECK(guest_is(STACK - 128, "nvnBarB") && test_getproc_sp <= STACK - 128 - 64 && !(test_getproc_sp & 15));
        CHECK(test_bar_sp == STACK);
        CHECK(!strcmp(nvn_lookup(BAR_PC)->name, "nvnBarB"));
        CHECK(nvn_call(&c, "nvnBarB", a, 2, &r) && r == 12 && test_getproc_runs == 6); /* found by name this time */
        CHECK(!nvn_call(&c, "nvnNoSuch", a, 0, &r));
        CHECK(!memcmp(&before, &c, offsetof(CPU, tpidr)));
    }

    CHECK(test_locks == 0);
    printf("nvn_seam: %u checks passed\n", test_checks);
    return 0;
}
