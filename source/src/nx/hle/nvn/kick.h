/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Phase 4 step 4.0's trace (kick.c, SNX_NVN_KICK=<frame>): which NVN call is running on the thread that makes each
   nvdrv SUBMIT_GPFIFO, and which release methods the GPU thread then performs for those submissions. Counts every
   600 presents; every event of three frames from <frame> on, in order. Off unless set */
#ifndef SNX_HLE_NVN_KICK_H
#define SNX_HLE_NVN_KICK_H
#include "nx/hle/nvn/nvn.h"

extern int g_kick_on;
void kick_init(void);
/* nvn_entry around a routed function: the call it runs on this thread, restored by kick_leave */
const char* kick_enter(const NvnFn* f, CPU* RESTRICT c);
void kick_leave(const NvnFn* f, const char* prev);

#endif
