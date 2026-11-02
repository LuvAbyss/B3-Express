/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/gdbstub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* gdbstub -s|-S MARKER PORT: the stub on PORT, then MARKER written once the program runs */
int main(int argc, char** argv) {
    FILE* f;
    if (argc != 4) return 2;
    if (snx_gdb_start(!strcmp(argv[1], "-s"), atoi(argv[3]))) return 2;
    f = fopen(argv[2], "w");
    if (!f) return 3;
    fputs("running\n", f);
    fclose(f);
    for (;;) pause();
}
