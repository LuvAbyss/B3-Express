/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_EXECUTION_H
#define SNX_GNU_EXECUTION_H
#include "gnuabi/process/process.h"
#include "common/cpu/execution/execution.h"
/* Runs the process's program, and the programs it execs, on this thread until it exits; its exit status. Single
   guest thread. Protected arena faults terminate the guest; guest signal-frame delivery is not implemented. */
int gnu_runtime_run(GnuProcess* process);
#endif
