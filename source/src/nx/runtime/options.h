/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's settings file, <Game>.ini (options.c): its sections, the command line's overrides, reloading it while the
   game runs and writing back what the game's menu changes; and the settings the runtime itself owns */
#ifndef SNX_OPTIONS_H
#define SNX_OPTIONS_H
#include <stddef.h>

#include "nx/runtime/startup.h"

extern int g_opt_fullscreen; /* fullscreen: the .ini's, or this run's from -f or --fullscreen=0|1 (startup.h) */

/* --key=value: k's value for this run, over the file's, which keeps its own. Before ini_open; v is kept, not copied */
void ini_override(const char* k, const char* v);
/* a value the game's menu set, already applied: written to the file at the next poll, even over an edit made meanwhile */
void ini_set(const char* k, const char* v);
/* every setting of the file's sections but the game's own (a control's place), a line each on stdout, nine fields
   between tabs: the section (with /page for a page of its own), the key, its default, int | float | text, the lowest
   and highest number, the help on one line, the menus' name, and the values as value=word between '|' (empty for a
   typed one). scarletnx --settings prints it for the launcher's Settings */
void ini_list(void);
/* 1 for a key of the file or an unattended-run option (platform/unattended.h): the driver passes only those on to a
   game (driver.h) */
int snx_setting_known(const char* key);
/* reads path, applies it with the overrides, writes it back with its help, and watches it: ini_poll reloads it when it
   changes and saves what the game's menu set */
void ini_open(const char* path);
/* about once a second, from the main loop. final: at exit, where an edit still being saved is left for the next start
   unless the menu's changes need writing, which is then done over the file as it is */
void ini_poll(int final);
extern int g_opt_docked, g_opt_lang;
/* fps_limit: 0 = no limit, SNX_FPS_GAME = the game's own pace (its swap interval), else frames a second (10 to 1000) */
#define SNX_FPS_GAME (-1)
extern int g_opt_fps;                              /* atomic: the game's threads read it */
void fps_limit_text(int fps, char* out, size_t n); /* g_opt_fps as fps_limit's value: "game", "0", "30"... */
extern int g_opt_diag, g_opt_shadows, g_opt_prof;
/* auto_stub_ipc: 1 = a service command the runtime doesn't declare answers success with no outputs, 0 = the
   console's UnknownCommandId (hle/kernel/ipc.c's stub_ok, sm's stand-in for a service there isn't) */
extern int g_opt_auto_stub_ipc;
extern int g_opt_network; /* network: 1 = the game is online through the host (hle/services/network) */
extern float g_opt_resf;  /* resolution: the scale of render targets */
extern float g_opt_fsr, g_opt_fsr_sharpness; /* fsr: scene smaller than resolution by this, 0 off; stops */
extern int g_opt_vsync;   /* vsync: 0 = at once, 1 = mailbox, 2 = FIFO */
extern int g_opt_mute_unfocused; /* mute_unfocused: 1 = the sound stops while another window has the focus */
extern int g_opt_cutscene_fps;   /* cutscene_fps: 30 as the game asks, 60 where hle/scene60.c knows the game */
extern int g_opt_aniso;   /* anisotropy: 0 = the game's, else at least that much for mipmapped 2D textures */

#endif
