/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The program's entry: the host driver's commands (extract, recompile) first, and anything else is a game to run */
#include <stdio.h>

#include "nx/runtime/driver/driver.h"
#include "nx/runtime/main.h"
#ifdef SNX_WITH_GUI
#include "gui/gui.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include "gui/apple.h"
#define SNX_MENU_THEN_GAME 1
#endif
#endif
#endif

/* Android's game activity finds it by name in libscarletnx.so (platform/android_run.c does the same) */
__attribute__((visibility("default"))) int main(int argc, char** argv);

int main(int argc, char** argv) {
    int result;
    /* a launcher reads our progress through pipes: lines as they end, errors at once */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef SNX_WITH_GUI
    snx_menu = snx_gui_main;
#endif
    result = snx_driver(argc, argv);
    if (result >= 0) return result;
#ifdef SNX_MENU_THEN_GAME
    /* iOS has one process: the menu runs first, then the game it picked, here */
    if (argc <= 1) {
        char** played;
        int played_argc = 0;
        result = snx_gui_main(argv[0]);
        played = gui_apple_played(&played_argc);
        if (!played) return result;
        return snx_main(played_argc, played);
    }
#endif
    return snx_main(argc, argv);
}
