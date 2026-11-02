/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An explicit guest-thread stack must already be committed below a large unprobed frame. */
#include "nx/runtime/options.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>

int g_opt_prof;
void plat_prof_add_thread(HANDLE h) { (void)h; }
/* Unused platform entry points must never be reached by this thread-only test. */
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
    abort();
}
void log_crash_mode(void) { abort(); }
void crash_report(void* pc, void* fault, int sig) {
    (void)pc;
    (void)fault;
    (void)sig;
    abort();
}
wchar_t* snx_host_wpath(const char* text) {
    (void)text;
    abort();
}

static HANDLE finished;
static int committed;
static void inspect_stack(void* arg) {
    MEMORY_BASIC_INFORMATION info;
    volatile char here;
    (void)arg;
    uintptr_t below = (uintptr_t)&here - (256u << 10);
    committed = VirtualQuery((void*)below, &info, sizeof info) == sizeof info && info.State == MEM_COMMIT &&
                !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    SetEvent(finished);
}
int main(void) {
    finished = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!finished || plat_thread(inspect_stack, NULL, (size_t)32 << 20) ||
        WaitForSingleObject(finished, 10000) != WAIT_OBJECT_0)
        return 2;
    CloseHandle(finished);
    if (!committed) {
        fputs("Explicit guest stack is reserved but not committed below a large frame\n", stderr);
        return 1;
    }
    puts("Explicit guest stack is committed below a large unprobed frame");
    return 0;
}
