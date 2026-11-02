/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi's display clock, which present.c drives */
#ifndef SNX_VI_H
#define SNX_VI_H

/* the display's refresh rate: vsync ticks, and the swap interval a game asks for, count in it */
#define VI_REFRESH_HZ 60

void vi_vsync_tick(void); /* one display refresh: signals the vsync event */
#endif
