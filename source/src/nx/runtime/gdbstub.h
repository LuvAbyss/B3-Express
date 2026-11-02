/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GDBSTUB_H
#define SNX_GDBSTUB_H
#define SNX_GDB_PORT 1234
/* Native host debugging via a separate UNIX gdbserver, on localhost's PORT (SNX_GDB_PORT but in its test). Call
   before threads start. */
int snx_gdb_start(int stop, int port);
#endif
