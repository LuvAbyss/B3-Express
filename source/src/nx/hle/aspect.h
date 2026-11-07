/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bayonetta 3 v1.2.0 at other aspect ratios than 16:9 (aspect_ratio=, aspect.c) */
#ifndef SNX_HLE_ASPECT_H
#define SNX_HLE_ASPECT_H

#include "common/cpu/aarch64/cpu.h"

void aspect_init(void); /* after kernel_load_symbols, before the game runs */
/* width / height of the game's own picture, 0 for the game's 16:9 */
float aspect_ratio(void);
/* the memory the game is told it has grows by this (its render-target heap grows as much wider than 16:9) */
u64 aspect_extra_memory(void);

#endif
