/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Do not name this process.h: SDL needs the Windows CRT header of that name. */
#ifndef SNX_GUI_PROCESS_H
#define SNX_GUI_PROCESS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GuiProcess GuiProcess;
typedef struct {
    uint64_t memory_bytes; /* sum of resident sets, shared pages may be counted more than once */
    double cpu_percent;    /* 100% = one fully occupied host CPU */
    bool available;
} GuiUsage;

/* A process tree's CPU use between two looks, from its CPU seconds at now seconds. The base moves only when the clock
   has: looks closer than its resolution (GetTickCount64's 15.6 ms) add up instead of losing the CPU used between them.
   CPU that went down, a process gone from the sum, starts the base again */
typedef struct {
    double cpu, time;
    bool measured;
} GuiCpuBase;
static inline double gui_cpu_percent(GuiCpuBase* base, double cpu, double now) {
    double percent = 0;
    if (base->measured && now <= base->time && cpu >= base->cpu) return 0;
    if (base->measured && now > base->time && cpu >= base->cpu) percent = 100 * (cpu - base->cpu) / (now - base->time);
    base->cpu = cpu;
    base->time = now;
    base->measured = true;
    return percent;
}

/* No shell. stdout and stderr share one pipe inherited by all descendants.
   The window's working directory is also the child's working directory. */
GuiProcess* gui_process_create(void);
bool gui_process_start(GuiProcess* process, const char* const* argv, char* error, size_t size);
/* Nonblocking. >0 bytes, 0 temporarily empty, -1 EOF. */
int gui_process_read(GuiProcess* process, char* buffer, size_t size);
/* True only after the driver AND its descendants have exited. */
bool gui_process_finished(GuiProcess* process, int* status);
GuiUsage gui_process_usage(GuiProcess* process);
/* Force-kill the owned process tree without waiting for exit or pipe EOF.
   Keep reading and polling finished before reusing the process. */
void gui_process_terminate(GuiProcess* process);
/* Closing the window terminates the owned process tree and reaps the driver. */
void gui_process_destroy(GuiProcess* process);
#endif
