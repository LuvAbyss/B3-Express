/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Cheat codes in Atmosphère's format (docs/features/cheats.md in Atmosphère, read for the format, not ported): a text
   file of "[Name]" cheats and "{Name}" master codes, each a run of 8-digit hex words, read from a mod's
   cheats/<build id>.txt. The words are instructions of a small machine run once a frame: memory writes and loads
   relative to a region (the main module, the heap, the alias region, the address space, or none), registers,
   conditions on memory, registers and held buttons, loops and arithmetic.
   A recompiled game's code is compiled already, so a write into its code changes nothing while it runs. The writes
   that happen on every run whatever the game does (snx_cheat_fixed) are compiled in instead, as a code patch would be
   (patch.c); the rest runs as on the console */
#ifndef SNX_NX_CHEAT_H
#define SNX_NX_CHEAT_H

#include <stddef.h>
#include <stdint.h>

/* the memory regions an instruction names: base[] of SnxCheatEnv in this order */
enum { SNX_CHEAT_MAIN, SNX_CHEAT_HEAP, SNX_CHEAT_ALIAS, SNX_CHEAT_ASLR, SNX_CHEAT_NONE, SNX_CHEAT_REGIONS };

/* the master codes and the cheats turned on, their words in the file's order; op holds once[] after them (one
   allocation) */
typedef struct {
    uint32_t* op;
    unsigned char* once; /* per word: an extended key condition (0xC4) that ran and whose keys are still held */
    size_t n;
    unsigned cheats, masters; /* cheats turned on, master codes */
    uint64_t statics[256];    /* static registers (0xC3): kept from run to run */
} SnxCheatProgram;

/* what a run reads and writes: read and write return 0 when the memory was there (and writable) */
typedef struct {
    uint64_t base[SNX_CHEAT_REGIONS];
    uint64_t keys; /* the buttons held, as nn::hid::NpadButton bits */
    void* ctx;
    int (*read)(void* ctx, uint64_t addr, void* out, size_t size);
    int (*write)(void* ctx, uint64_t addr, const void* in, size_t size);
    void (*log)(void* ctx, unsigned id, uint64_t value); /* 0xFFF's debug log, id 0 to 15; NULL: ignored */
} SnxCheatEnv;

/* a write of the main module that every run makes: width bytes of value (little-endian) at offset from its start */
typedef struct {
    uint64_t offset, value;
    unsigned width;
} SnxCheatWrite;

/* The file's master codes and the cheats named in enabled (a cheat's name, without its brackets, on a line of its
   own) into p. 0, or -1 with error saying why (a word that isn't 8 hex digits, codes before any name) */
int snx_cheat_parse(const char* text, size_t size, const char* enabled, size_t enabled_size, SnxCheatProgram* p,
                    char* error, size_t error_size);
void snx_cheat_free(SnxCheatProgram* p);
/* one run of the program, as Atmosphère runs it: registers cleared, static registers kept */
void snx_cheat_run(SnxCheatProgram* p, const SnxCheatEnv* env);
/* the writes into the main module that every run makes (outside conditions and loops, with an offset register no
   earlier instruction set), up to max into out; their count */
size_t snx_cheat_fixed(const SnxCheatProgram* p, SnxCheatWrite* out, size_t max);

#endif
