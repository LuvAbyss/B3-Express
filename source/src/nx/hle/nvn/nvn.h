/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's NVN entry points, routed through host handlers. The game finds them with nvnBootstrapLoader and
   nvnDeviceGetProcAddress and calls each through the pointer it got, an indirect call through g_disp: the entry of each
   pointer it is handed runs nvn_entry, which finds the function's record and calls its handler. A handler that has
   nothing of its own to do runs the game's NVN driver (nvn_guest), so the result is the same as without the module.
   SNX_NVN_FRONTEND=1 routes the calls; SNX_NVN_SHADOW=1 routes them through shadow.c's handlers where it has one;
   SNX_NVN_TRACE=1 also logs each function's first call, the call counts and who waits in nvnSyncWait. None set:
   g_disp is left alone */
#ifndef SNX_HLE_NVN_H
#define SNX_HLE_NVN_H
#include "common/cpu/aarch64/cpu.h"

typedef struct NvnFn NvnFn;
typedef void (*NvnHandler)(CPU* RESTRICT c, const NvnFn* f);
struct NvnFn {
    u64 pc;             /* the function's guest address, as the game was handed it */
    GuestFn guest;      /* what g_disp had for it: the game's NVN driver */
    NvnHandler handler; /* nvn_guest unless a handler is registered for name */
    u64 calls;
    int sync_wait; /* nvnSyncWait: the trace counts its callers */
    int op;        /* the handler's own number for the function (cmdlist.c's command), 0 when it has none */
    char name[64];
};

void nvn_init(void);                                /* after kernel_load_symbols, before the game runs */
void nvn_guest(CPU* RESTRICT c, const NvnFn* f);    /* the game's own NVN driver function, as if not routed */
u32 nvn_routed(void);                               /* functions routed so far */
const NvnFn* nvn_lookup(u64 pc);                    /* the record of a routed function, NULL for any other address */
/* From a handler: the game's NVN driver function name with args (at most 8) in x0.., on the calling thread, its x0 in
   *ret. The thread's registers are as before afterwards. A function the game never asked for is found with the
   driver's nvnDeviceGetProcAddress. 0 when it can't be found */
int nvn_call(CPU* RESTRICT c, const char* name, const u64* args, u32 nargs, u64* ret);
/* nvn_call with SP first lowered by reserve bytes: for arguments that point at data the caller put below SP (a state
   object to read, places for a getter's outputs), which the call's frames must not overwrite */
int nvn_call_below(CPU* RESTRICT c, const char* name, const u64* args, u32 nargs, u64* ret, u64 reserve);

#endif
