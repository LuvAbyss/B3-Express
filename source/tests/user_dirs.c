/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_user_dir and snx_user_path: an installed build's config, state and cache folders (the XDG Base Directory spec),
   a portable copy's (config/, state/ and cache/ of its .scarletnx), and where a file of a data folder goes in them;
   snx_menu_dir: the launcher's own, for it and for a game of its. tests/data_dir.c has the data folder itself. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700 /* realpath */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void expect_dir(int kind, const char* expected) {
    char* dir = snx_user_dir(kind);
    assert(expected ? dir && !strcmp(dir, expected) : !dir);
    free(dir);
}
static void expect_path(const char* path, int kind, const char* expected) {
    char* moved = snx_user_path(path, kind);
    assert(moved && !strcmp(moved, expected));
    free(moved);
}
static void expect_menu(const char* folder, int kind, const char* expected) {
    char* dir = snx_menu_dir(folder, kind);
    assert(dir && !strcmp(dir, expected));
    free(dir);
}

int main(void) {
    static const char* const vars[] = {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
    char *root = snx_temp_dir(NULL), *home, *dir[4], *base, *games, *path;
    int kind;
    assert(root);
    /* the folder as the functions name it, links resolved: macOS's temporary folder is under /var, a link to
       /private/var */
    {
        char* real = realpath(root, NULL);
        assert(real);
        free(root);
        root = real;
    }
    home = join(root, "home");
    setenv("HOME", home, 1);
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) unsetenv(vars[kind]);
#ifdef SNX_XDG
    {
        static const char* const defaults[] = {".local/share", ".config", ".local/state", ".cache"};
        char* expected;
        for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) {
            /* unset, empty or relative: the spec's default under HOME */
            base = join(home, defaults[kind]);
            expected = join(base, "scarletnx");
            expect_dir(kind, expected);
            setenv(vars[kind], "", 1);
            expect_dir(kind, expected);
            setenv(vars[kind], "relative/folder", 1);
            expect_dir(kind, expected);
            free(expected);
            free(base);
            /* without a home only an absolute variable names the folder */
            unsetenv("HOME");
            expect_dir(kind, NULL);
            base = join(root, vars[kind]);
            expected = join(base, "scarletnx");
            setenv(vars[kind], base, 1);
            expect_dir(kind, expected);
            setenv("HOME", home, 1);
            expect_dir(kind, expected);
            free(expected);
            free(base);
        }
    }
#else
    /* config/, state/ and cache/ of the data folder where the spec does not apply */
    {
        static const char* const names[] = {NULL, "config", "state", "cache"};
        base = snx_user_data_dir();
        assert(base);
        expect_dir(SNX_USER_DATA, base);
        for (kind = SNX_USER_CONFIG; kind <= SNX_USER_CACHE; ++kind) {
            path = join(base, names[kind]);
            expect_dir(kind, path);
            free(path);
        }
        free(base);
    }
#endif

    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) {
        base = join(root, vars[kind]);
        setenv(vars[kind], base, 1);
        dir[kind] = snx_user_dir(kind);
        assert(dir[kind]);
        free(base);
    }
    games = join(dir[SNX_USER_DATA], "games");
    assert(!snx_mkdirs(games));
#ifdef SNX_XDG
    {
        char *expected, *link, *cwd;
        /* the installed games/: each kind in its folder, under the same name */
        path = join(dir[SNX_USER_CONFIG], "games");
        expect_path(games, SNX_USER_CONFIG, path);
        free(path);
        path = join(games, "Game/Game.ini"); /* a game's folder */
        expected = join(dir[SNX_USER_CONFIG], "games/Game/Game.ini");
        expect_path(path, SNX_USER_CONFIG, expected);
        free(expected);
        free(path);
        path = join(dir[SNX_USER_STATE], "games");
        expect_path(games, SNX_USER_STATE, path);
        free(path);
        path = join(dir[SNX_USER_DATA], "work");
        expected = join(dir[SNX_USER_CACHE], "work");
        expect_path(path, SNX_USER_CACHE, expected);
        free(expected);
        free(path);
        expect_path(dir[SNX_USER_DATA], SNX_USER_CACHE, dir[SNX_USER_CACHE]);
        /* a trailing separator, and the folder reached through a link */
        path = snx_format("%s/", games);
        expected = join(dir[SNX_USER_STATE], "games");
        expect_path(path, SNX_USER_STATE, expected);
        free(path);
        link = join(root, "link");
        assert(!symlink(dir[SNX_USER_DATA], link));
        path = join(link, "games");
        expect_path(path, SNX_USER_STATE, expected);
        free(path);
        free(link);
        /* the runner started in games/ as ./Game */
        cwd = getcwd(NULL, 0);
        assert(cwd && !chdir(games));
        expect_path("./", SNX_USER_STATE, expected);
        expect_path(".", SNX_USER_STATE, expected);
        assert(!chdir(cwd));
        free(cwd);
        free(expected);
    }
#endif
    /* data stays; a checkout's or an --outdir's games/, or an older version's portable copy's, keep everything beside
       the game */
    expect_path(games, SNX_USER_DATA, games);
    path = join(root, "portable/games");
    expect_path(path, SNX_USER_CONFIG, path);
    expect_path(path, SNX_USER_CACHE, path);
    free(path);
    /* a portable copy's .scarletnx: its config/, state/ and cache/, on every system */
    {
        char *expected, *copy = join(root, "portable/.scarletnx");
        path = join(copy, "games/Game");
        expected = join(copy, "config/games/Game");
        expect_path(path, SNX_USER_CONFIG, expected);
        free(expected);
        expected = join(copy, "cache/work");
        free(path);
        path = join(copy, "work");
        expect_path(path, SNX_USER_CACHE, expected);
        expect_path(path, SNX_USER_DATA, path);
        free(expected);
        free(path);
        expected = join(copy, "state");
        expect_path(copy, SNX_USER_STATE, expected);
        assert(snx_app_dir(copy) && !snx_app_dir(root));
        free(expected);
        path = join(root, "portable/.scarletnx-other/games"); /* only that name */
        expect_path(path, SNX_USER_STATE, path);
        free(path);
        /* the launcher's settings.ini and gamecontrollerdb.txt: its config/, for it and for a game in its games/ */
        expected = join(copy, "config");
        expect_menu(copy, SNX_USER_CONFIG, expected);
        path = join(copy, "games/0100DA801624E000/");
        expect_menu(path, SNX_USER_CONFIG, expected);
        free(path);
        free(expected);
        free(copy);
    }
    /* the user's data folder's, and any other folder's (a checkout, an --outdir, a folder of games the player added):
       the user's config folder */
    {
        char* user = snx_user_dir(SNX_USER_CONFIG);
        assert(user);
        expect_menu(games, SNX_USER_CONFIG, user);
        path = join(root, "portable/games/0100DA801624E000");
        expect_menu(path, SNX_USER_CONFIG, user);
        free(path);
        expect_menu(NULL, SNX_USER_CONFIG, user);
        free(user);
    }
    path = snx_format("%s-other/games", dir[SNX_USER_DATA]);
    expect_path(path, SNX_USER_STATE, path);
    free(path);
    expect_path("games", SNX_USER_CONFIG, "games");
#ifdef SNX_XDG /* elsewhere the config folder is always the data folder's config/ */
    /* a config folder that is the data folder divides nothing */
    setenv("XDG_CONFIG_HOME", getenv("XDG_DATA_HOME"), 1);
    expect_path(games, SNX_USER_CONFIG, games);
#endif

    puts("snx_user_dir: the XDG variables, their defaults, a relative path ignored; snx_user_path: the installed "
         "and a portable copy's games/ divided, any other folder kept; snx_menu_dir: the launcher's for its games");
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) free(dir[kind]);
    free(games);
    free(home);
    assert(!snx_remove_tree(root));
    free(root);
    return 0;
}
