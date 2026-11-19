/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi's display clock, which present.c drives */
#ifndef SNX_VI_H
#define SNX_VI_H

/* the display's refresh rate: vsync ticks, and the swap interval a game asks for, count in it */
#define VI_REFRESH_HZ 60
/* the rate the runtime ticks it at: VI_REFRESH_HZ, or frame_rate once hle/scene60.c keeps the game's clock in real time
   at it. Set before the game runs */
extern unsigned g_vi_hz;

void vi_vsync_tick(void); /* one display refresh: signals the vsync event */
#endif
