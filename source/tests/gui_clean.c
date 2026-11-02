/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Clean old settings (src/gui/clean.c): a game's .ini without its [Deprecated] section and nothing else changed, the
   .ini files of a folder, and an entry for each file, done or not */
#include "gui/clean.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SNX_XDG
#include <sys/stat.h>
#include <unistd.h>
#endif

/* text cleaned: what is left as a string */
static const char* cleaned(const char* text) {
    static char buffer[1024];
    size_t n = strlen(text), kept;
    assert(n < sizeof buffer);
    memcpy(buffer, text, n + 1);
    kept = gui_clean_deprecated(buffer, n);
    buffer[kept] = 0;
    return buffer;
}
static char* read_text(const char* path) {
    size_t size;
    return (char*)snx_read_file(path, &size);
}
static void write_text(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
/* the entries as "+file|what" lines */
static const char* entries(const GuiCleaned* done, const char* base) {
    static char out[8192];
    char file[4400], what[4400];
    const char* at = done->text;
    bool ok;
    size_t n = 0, skip = strlen(base);
    out[0] = 0;
    while (at) {
        at = gui_cleaned_entry(at, &ok, file, what, sizeof file);
        n += (size_t)snprintf(out + n, sizeof out - n, "%c%s|%s\n", ok ? '+' : '!',
                              strncmp(file, base, skip) ? file : file + skip, what);
    }
    return out;
}

int main(void) {
    /* the runtime's section, last after [Other]: it and the blank line before it go, the rest as it was */
    const char* game =
        "[Display]\nfullscreen=1\n\n[Other]\nlanguage=en\n\n"
        "[Deprecated]\n; keys an older version used, which do nothing now (docs/DEPRECATIONS.md): delete "
        "them, or use Settings > App >\n;   Clean old settings in the launcher\n"
        "; renderer: Vulkan is the only renderer\nrenderer=opengl\n; touch_controls: now controls\n"
        "touch_controls=1\n";
    assert(!strcmp(cleaned(game), "[Display]\nfullscreen=1\n\n[Other]\nlanguage=en\n"));
    /* none: the size as it was, the text untouched */
    {
        char text[] = "[Other]\nx=1\n\n";
        assert(gui_clean_deprecated(text, strlen(text)) == strlen(text) && !strcmp(text, "[Other]\nx=1\n\n"));
    }
    /* Windows line ends; blanks of spaces and tabs; a section after it stays, as do the blank lines inside the rest */
    assert(!strcmp(cleaned("[A]\r\na=1\r\n \t\r\n\r\n[Deprecated]\r\nold=1\r\n[B]\r\n\r\nb=2\r\n"),
                   "[A]\r\na=1\r\n[B]\r\n\r\nb=2\r\n"));
    /* only that line exactly: other names, a space around it, are other sections */
    assert(!strcmp(cleaned("[deprecated]\nx=1\n[Deprecated] \ny=1\n [Deprecated]\nz=1\n"),
                   "[deprecated]\nx=1\n[Deprecated] \ny=1\n [Deprecated]\nz=1\n"));
    /* two of them, one first in the file, the last without a line end; an indented header ends one */
    assert(!strcmp(cleaned("[Deprecated]\na=1\n  [A]\nb=2\n\n[Deprecated]\nc=3"), "  [A]\nb=2\n"));
    assert(!strcmp(cleaned("[Deprecated]"), ""));

    char* root = snx_temp_dir(NULL);
    char *games = snx_path_join(root, "games"), *path, *text;
    GuiCleaned done = {0};
    assert(root && !snx_mkdirs(games));

    /* a folder's .ini files: the ones with the section counted, then cleaned, an entry each; the others untouched */
    path = snx_path_join(games, "Celeste.ini");
    write_text(path, game);
    free(path);
    path = snx_path_join(games, "Hades.INI");
    write_text(path, "[Deprecated]\nx=1\n");
    free(path);
    path = snx_path_join(games, "Clean.ini");
    write_text(path, "[Other]\nx=1\n");
    free(path);
    path = snx_path_join(games, "notes.txt");
    write_text(path, "[Deprecated]\nx=1\n");
    free(path);
    /* a game's folder: its own .ini, not another file of it, nor a folder that is not a game's */
    {
        char* folder = snx_path_join(games, "0100ABCD12345000");
        char* data = snx_path_join(games, "Hades_data");
        assert(!snx_mkdirs(folder) && !snx_mkdirs(data));
        path = snx_path_join(folder, "0100ABCD12345000.ini");
        write_text(path, "[Deprecated]\nx=1\n");
        free(path);
        path = snx_path_join(folder, "other.ini");
        write_text(path, "[Deprecated]\nx=1\n");
        free(path);
        path = snx_path_join(data, "Hades_data.ini");
        write_text(path, "[Display]\n");
        free(path);
        path = snx_path_join(data, "save.ini");
        write_text(path, "[Deprecated]\nx=1\n");
        free(path);
        free(folder);
        free(data);
    }
    assert(gui_clean_inis(NULL, games) == 3 && !done.count);
    assert(gui_clean_inis(NULL, NULL) == 0 && gui_clean_inis(NULL, root) == 0);
    {
        char* other = snx_path_join(root, "absent");
        assert(gui_clean_inis(&done, other) == 0 && !done.count);
        free(other);
    }
    assert(gui_clean_inis(&done, games) == 3 && done.count == 3 && done.done == 3);
    {
        const char* list = entries(&done, games);
        assert(strstr(list, "+/Celeste.ini|[Deprecated] taken out\n") &&
               strstr(list, "+/Hades.INI|[Deprecated] taken out\n") &&
               strstr(list, "+/0100ABCD12345000/0100ABCD12345000.ini|[Deprecated] taken out\n"));
    }
    path = snx_path_join(games, "0100ABCD12345000/other.ini");
    text = read_text(path);
    assert(!strcmp(text, "[Deprecated]\nx=1\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "Celeste.ini");
    text = read_text(path);
    assert(!strcmp(text, "[Display]\nfullscreen=1\n\n[Other]\nlanguage=en\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "Clean.ini");
    text = read_text(path);
    assert(!strcmp(text, "[Other]\nx=1\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "notes.txt");
    text = read_text(path);
    assert(!strcmp(text, "[Deprecated]\nx=1\n"));
    free(text);
    free(path);
    path = snx_path_join(games, "Celeste.ini.new");
    assert(!snx_path_exists(path)); /* the staged copy went in place */
    free(path);
    assert(gui_clean_inis(NULL, games) == 0); /* nothing left to take out */
    gui_cleaned_free(&done);

#ifdef SNX_XDG
    /* what cannot be done is an entry of its own, why in it, and the file stays */
    if (geteuid()) {
        char *locked = snx_path_join(root, "locked"), *ini = snx_path_join(locked, "Game.ini"), expected[512];
        assert(!snx_mkdirs(locked));
        write_text(ini, "[A]\n[Deprecated]\nx=1\n");
        assert(!chmod(locked, 0555));
        assert(gui_clean_inis(&done, locked) == 1);
        assert(done.count == 1 && !done.done);
        snprintf(expected, sizeof expected, "!/locked/Game.ini|Not written without [Deprecated]: Permission denied\n");
        assert(!strcmp(entries(&done, root), expected));
        text = read_text(ini);
        assert(text && !strcmp(text, "[A]\n[Deprecated]\nx=1\n"));
        free(text);
        assert(!chmod(locked, 0755));
        gui_cleaned_free(&done);
        free(locked);
        free(ini);
    }
#endif
    /* a tab or a line's end in a name keeps the entry whole */
    {
        char file[64], what[64];
        bool ok;
        const char* next;
        gui_cleaned_add(&done, true, "a\tb\nc", "d\te");
        gui_cleaned_add(&done, false, "f", "g");
        next = gui_cleaned_entry(done.text, &ok, file, what, sizeof file);
        assert(ok && !strcmp(file, "a b c") && !strcmp(what, "d e") && next);
        next = gui_cleaned_entry(next, &ok, file, what, sizeof file);
        assert(!ok && !strcmp(file, "f") && !strcmp(what, "g") && !next);
        assert(done.count == 2 && done.done == 1);
        gui_cleaned_free(&done);
        assert(!gui_cleaned_entry(NULL, &ok, file, what, sizeof file));
    }
    assert(!snx_remove_tree(root));
    free(games);
    free(root);
    puts("gui_clean passed");
    return 0;
}
