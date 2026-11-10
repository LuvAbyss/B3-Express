/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Host command lists (cmdlist.c, SNX_NVN_CMDLIST=1): while the game records an NVN command buffer, each command is
   also kept on the host with its arguments and the data they point at, the way B2recomp's front end records them. The
   game's driver still records and runs everything (dual mode); the lists are checked at submit */
#ifndef SNX_HLE_NVN_CMDLIST_H
#define SNX_HLE_NVN_CMDLIST_H
#include "nx/hle/nvn/nvn.h"

/* the recording handler for an NVN function and its operation number in *op, NULL for a function it doesn't keep */
NvnHandler nvn_cmd_handler(const char* name, int* op);
/* F6: the next whole frame saved, every host image of it at prefix-fNNNNNN, each draw logged with its shaders and
   state (as SNX_NVN_LIVE_DUMP does for a frame number given beforehand). Any thread */
void nvn_capture_frame(const char* prefix);

#endif
