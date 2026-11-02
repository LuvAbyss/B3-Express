/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini's settings tables (runtime/ini.c): a value read into its variable, clamped, and written back; and as the
   menus word it and step it */
#include "nx/runtime/ini.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int number, told;
static float ratio;
static char text[16];
static void tell(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    ++told;
}
static void keep(const IniOpt* o, const char* v) {
    (void)o;
    snprintf(text, sizeof text, "%s", v);
}
static const IniOpt TABLE[] = {{"number", "1", "help", INI_INT, 0, &number, -5, 10, tell, NULL, NULL, 0, 0},
                               {"ratio", "1", NULL, INI_FLOAT, INI_STARTUP, &ratio, 0.25, 4, NULL, NULL, NULL, 0, 0},
                               {"text", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, keep, NULL, NULL, 0, 0},
                               {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};

int main(void) {
    const IniOpt *n = ini_find(TABLE, "number"), *r = ini_find(TABLE, "ratio"), *t = ini_find(TABLE, "text");
    char out[32];
    assert(n == &TABLE[0] && r == &TABLE[1] && t == &TABLE[2] && !ini_find(TABLE, "numbe") && !ini_find(TABLE, ""));
    /* a number, clamped, and set told after each read */
    ini_read(n, "7");
    assert(number == 7 && told == 1);
    ini_read(n, "99");
    assert(number == 10 && told == 2);
    ini_read(n, "-1e9");
    assert(number == -5 && told == 3);
    ini_read(n, "3.9"); /* an int keeps the whole part */
    assert(number == 3);
    /* not a number, or NaN: the variable stays and set isn't called */
    ini_read(n, "seven");
    ini_read(n, "");
    ini_read(n, "nan");
    assert(number == 3 && told == 4);
    ini_read(r, "inf");
    assert(ratio == 4);
    ini_read(r, "0.5");
    assert(ratio == 0.5f);
    /* written back as it reads */
    ini_format(n, out, sizeof out);
    assert(!strcmp(out, "3"));
    ini_format(r, out, sizeof out);
    assert(!strcmp(out, "0.5"));
    /* text: set reads it whole, and nothing is written back */
    ini_read(t, "a b c");
    assert(!strcmp(text, "a b c"));
    ini_format(t, out, sizeof out);
    assert(!out[0]);
    /* the menus' words: one a value from lo by step; a '#' word for its value and the ones above, plus shift; the
       number where there is none. Words go round, numbers stop at the ends */
    {
        static int v;
        static const IniOpt onoff = {"onoff", "0", NULL, INI_INT, 0, &v, 0, 1, NULL, "On or off", "Off|On", 0, 0};
        static const IniOpt three = {"three", "0", NULL, INI_INT, 0, &v, 0, 2, NULL, "Three", "Off|Fingers|Mouse too",
                                     0,       0};
        static const IniOpt scale = {"scale", "50", NULL, INI_INT, 0, &v, 0, 100, NULL, "Scale", "#%", 10, 50};
        static const IniOpt rumble = {"rumble", "100", NULL,        INI_INT,  0,  &v, 0,
                                      100,      NULL,  "Vibration", "Off|#%", 10, 0};
        static const IniOpt plain = {"plain", "1", NULL, INI_INT, 0, &v, -5, 10, NULL, NULL, NULL, 0, 0};
        ini_word(&onoff, 0, out, sizeof out);
        assert(!strcmp(out, "Off"));
        ini_word(&onoff, 1, out, sizeof out);
        assert(!strcmp(out, "On"));
        ini_word(&three, 2, out, sizeof out);
        assert(!strcmp(out, "Mouse too"));
        ini_word(&three, 7, out, sizeof out); /* past its words: the number */
        assert(!strcmp(out, "7"));
        ini_word(&scale, 50, out, sizeof out);
        assert(!strcmp(out, "100%"));
        ini_word(&scale, 55, out, sizeof out); /* off its steps, as a hand-made .ini has it */
        assert(!strcmp(out, "105%"));
        ini_word(&rumble, 0, out, sizeof out);
        assert(!strcmp(out, "Off"));
        ini_word(&rumble, 5, out, sizeof out);
        assert(!strcmp(out, "5%"));
        ini_word(&rumble, 100, out, sizeof out);
        assert(!strcmp(out, "100%"));
        ini_word(&plain, -3, out, sizeof out);
        assert(!strcmp(out, "-3"));
        ini_word(&scale, 50, out, 3); /* cut to the room given */
        assert(!strcmp(out, "10"));
        assert(ini_step(&onoff, 0, 1) == 1 && ini_step(&onoff, 1, 1) == 0 && ini_step(&onoff, 0, -1) == 1);
        assert(ini_step(&three, 2, 1) == 0 && ini_step(&three, 0, -1) == 2 && ini_step(&three, 1, 1) == 2);
        assert(ini_step(&scale, 50, 1) == 60 && ini_step(&scale, 100, 1) == 100 && ini_step(&scale, 0, -1) == 0);
        assert(ini_step(&rumble, 0, 1) == 10 && ini_step(&rumble, 10, -1) == 0 && ini_step(&rumble, 5, 1) == 15);
        assert(ini_step(&plain, -5, -1) == -5 && ini_step(&plain, 3, 1) == 4);
    }
    puts("ini passed");
    return 0;
}
