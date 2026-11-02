/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shadow handlers (shadow.c, SNX_NVN_SHADOW=1): the game's NVN driver runs, and what each call asked for is noted
   for comparison with B2recomp's NVN layer */
#ifndef SNX_HLE_NVN_SHADOW_H
#define SNX_HLE_NVN_SHADOW_H
#include "nx/hle/nvn/nvn.h"

NvnHandler nvn_shadow_handler(const char* name); /* the shadow handler for an NVN function, NULL for none */

#endif
