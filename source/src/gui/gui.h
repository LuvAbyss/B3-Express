/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_H
#define SNX_GUI_H

/* The menu (main.c), until it is closed; its exit status. self is the program that runs Switch inputs, the runtime
   the menu is part of; NULL looks for scarletnx beside the menu, then on PATH */
int snx_gui_main(const char* self);
#endif
