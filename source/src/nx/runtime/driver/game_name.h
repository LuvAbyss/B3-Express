/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a recompiled game is called: the name it is shown by, and the name of its files */
#ifndef SNX_GAME_NAME_H
#define SNX_GAME_NAME_H
#include "nx/loader/loader.h"

/* name when given, else the NPDM's when it is not the SDK's "Application", else the input's file name without its
   extension; UTF-8 kept, path delimiters and C/OS metacharacters taken out, "Game" when nothing is left */
char* snx_game_title(const char* name, const SnxNpdm* npdm, const char* input);
/* name when given, else the title ID, 16 hex digits, when the NPDM gives the game one of its own, else its title: the
   same game is the same files whatever its dump was called */
char* snx_game_name(const char* name, const SnxNpdm* npdm, const char* input);
/* what the game is shown as (its window, the recompile's lines, its settings' head): name when given, else the name
   in its control.nacp, else its title */
char* snx_game_shown(const char* name, const SnxProgram* program);
#endif
