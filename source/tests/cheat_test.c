/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Cheat codes (nx/loader/cheat.c, and snx_patch_cheats in patch.c): parsing, the machine's instructions on a made-up
   memory, and the writes every run makes, compiled in as patches */
#include "nx/loader/patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(v)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(v)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* guest memory: 0x1000 bytes at 0x10000 (the main module there, its first 0x400 bytes code), the rest unmapped */
#define MEM_AT 0x10000u
static unsigned char mem[0x1000];
static unsigned refused;

static int rd(void* ctx, uint64_t addr, void* out, size_t size) {
    (void)ctx;
    if (addr < MEM_AT || addr + size > MEM_AT + sizeof mem) return -1;
    memcpy(out, mem + (addr - MEM_AT), size);
    return 0;
}
static int wr(void* ctx, uint64_t addr, const void* in, size_t size) {
    (void)ctx;
    if (addr < MEM_AT + 0x400 || addr + size > MEM_AT + sizeof mem) {
        refused++;
        return -1;
    }
    memcpy(mem + (addr - MEM_AT), in, size);
    return 0;
}

static unsigned nlogged, log_id[4];
static uint64_t log_val[4];
static void logged(void* ctx, unsigned id, uint64_t value) {
    (void)ctx;
    if (nlogged < 4) log_id[nlogged] = id, log_val[nlogged] = value;
    nlogged++;
}

static SnxCheatProgram prog;
static SnxCheatEnv env;
static char error[300];

static int parse(const char* text, const char* on) {
    snx_cheat_free(&prog);
    return snx_cheat_parse(text, strlen(text), on, on ? strlen(on) : 0, &prog, error, sizeof error);
}
/* the cheat file text with every cheat named "c" turned on, run once */
static void run(const char* text) {
    CHECK(parse(text, "c") == 0);
    snx_cheat_run(&prog, &env);
}
static uint32_t at32(unsigned off) {
    return (uint32_t)mem[off] | (uint32_t)mem[off + 1] << 8 | (uint32_t)mem[off + 2] << 16 |
           (uint32_t)mem[off + 3] << 24;
}
static void set32(unsigned off, uint32_t v) {
    unsigned i;
    for (i = 0; i < 4; ++i) mem[off + i] = (unsigned char)(v >> (8 * i));
}

int main(void) {
    SnxCheatWrite w[8];
    unsigned i;
    env.base[SNX_CHEAT_MAIN] = MEM_AT;
    env.base[SNX_CHEAT_HEAP] = MEM_AT + 0x800;
    env.read = rd;
    env.write = wr;

    /* parsing: names, what's on, masters, junk */
    CHECK(parse("[Title line]\n\n[A]\n04000000 00000500 00000001\n{Master}\n04000000 00000504 00000002\n"
                "[B]\r\n04000000 00000508 00000003 \r\n",
                "  B \r\nnot a cheat\n") == 0);
    CHECK(prog.cheats == 1 && prog.masters == 1 && prog.n == 6);
    CHECK(prog.op[0] == 0x04000000u && prog.op[2] == 2 && prog.op[5] == 3);
    CHECK(parse("[A]\n04000000 0000050 00000001\n", "A") == -1 && strstr(error, "line 2"));
    CHECK(parse("04000000\n[A]\n", "A") == -1);
    CHECK(parse("[A\n", "A") == -1);
    CHECK(parse("[A]\n04000000 00000500 00000001\n", NULL) == 0 && prog.n == 0); /* off by default */

    /* 0x0: write to main and the heap, with an offset register */
    memset(mem, 0, sizeof mem);
    run("[c]\n04000000 00000500 12345678\n08100000 00000010 00000001 00000002\n");
    CHECK(at32(0x500) == 0x12345678u && at32(0x810) == 2 && at32(0x814) == 1);
    /* writes into code are refused and the run goes on */
    refused = 0;
    run("[c]\n04000000 00000100 00000001\n04000000 00000600 00000007\n");
    CHECK(refused == 1 && at32(0x600) == 7);

    /* 0x1 with Else and End, nested */
    memset(mem, 0, sizeof mem);
    set32(0x500, 5);
    run("[c]\n14050000 00000500 00000005\n04000000 00000600 00000001\n21000000\n04000000 00000604 00000001\n"
        "20000000\n14060000 00000500 00000005\n04000000 00000608 00000001\n14050000 00000500 00000005\n"
        "04000000 0000060C 00000001\n20000000\n21000000\n04000000 00000610 00000001\n20000000\n");
    CHECK(at32(0x600) == 1 && at32(0x604) == 0 && at32(0x608) == 0 && at32(0x60c) == 0 && at32(0x610) == 1);

    /* 0x3 loop with 0x6's increment: four words written */
    memset(mem, 0, sizeof mem);
    run("[c]\n40010000 00000000 00010700\n30000000 00000004\n64011000 00000000 0000ABCD\n31000000\n");
    for (i = 0; i < 4; ++i) CHECK(at32(0x700 + 4 * i) == 0xabcdu);
    CHECK(at32(0x710) == 0);

    /* 0x5 pointer chain, 0x9 arithmetic, 0xA store: [[main+0x500]+8] = [..] * 3 + 1 */
    memset(mem, 0, sizeof mem);
    set32(0x500, MEM_AT + 0x900);
    set32(0x908, 10);
    run("[c]\n58010000 00000500\n54011000 00000008\n58020000 00000500\n94211100 00000003\n94011100 00000001\n"
        "A4120200 00000008\n");
    CHECK(at32(0x908) == 31);
    /* a pointer into nothing reads 0, and the write through it is refused instead of crashing */
    refused = 0;
    set32(0x500, 0x7000000u);
    run("[c]\n58010000 00000500\n64010000 00000000 00000001\n");
    CHECK(refused == 1);

    /* 0x8 keys and 0xC4's once */
    memset(mem, 0, sizeof mem);
    env.keys = 0;
    run("[c]\n80000041\n04000000 00000600 00000001\n20000000\n");
    CHECK(at32(0x600) == 0);
    env.keys = 0x41;
    run("[c]\n80000041\n04000000 00000600 00000001\n20000000\n");
    CHECK(at32(0x600) == 1);
    CHECK(parse("[c]\nC4000000 00000000 00000080\n58000000 00000600\n94000100 00000001\nA4000400 00000600\n20000000\n",
                "c") == 0);
    memset(mem, 0, sizeof mem);
    env.keys = 0x80;
    snx_cheat_run(&prog, &env);
    snx_cheat_run(&prog, &env);
    CHECK(at32(0x600) == 1); /* once while held */
    env.keys = 0;
    snx_cheat_run(&prog, &env);
    env.keys = 0x80;
    snx_cheat_run(&prog, &env);
    CHECK(at32(0x600) == 2); /* and again after a release */
    env.keys = 0;

    /* 0xC0 register condition against a value; 0xC3 static registers: 0x01 read, 0x81 written */
    memset(mem, 0, sizeof mem);
    run("[c]\n40000000 00000000 00000009\nC0430400 00000009\n04000000 00000600 00000001\n20000000\n");
    CHECK(at32(0x600) == 0); /* 9 < 9 fails */
    CHECK(parse("[c]\nC3000010\nA4000400 00000600\n94000100 00000001\nC3000810\n", "c") == 0);
    prog.statics[1] = 5;
    snx_cheat_run(&prog, &env);
    CHECK(at32(0x600) == 5 && prog.statics[0x81] == 6);

    /* 0xFFF logs: a register, and memory at a region's base */
    env.log = logged;
    memset(mem, 0, sizeof mem);
    set32(0x500, 77);
    run("[c]\n40000000 00000000 00001234\nFFF83400\nFFF45000 00000500\n");
    CHECK(nlogged == 2 && log_id[0] == 3 && log_val[0] == 0x1234 && log_id[1] == 5 && log_val[1] == 77);
    env.log = NULL;

    /* a loop that never ends stops at the budget */
    run("[c]\n30000000 00000000\n31000000\n");

    /* the writes every run makes: top level, main, with no offset register set before */
    CHECK(parse("[c]\n04000000 00000100 00000001\n14050000 00000500 00000005\n04000000 00000104 00000002\n20000000\n"
                "04100000 00000108 00000003\n40010000 00000000 00000004\n04010000 0000010C 00000004\n"
                "08000000 00000110 11112222 33334444\n",
                "c") == 0);
    CHECK(snx_cheat_fixed(&prog, NULL, 0) == 2);
    CHECK(snx_cheat_fixed(&prog, w, 8) == 2);
    CHECK(w[0].offset == 0x100 && w[0].width == 4 && w[0].value == 1);
    CHECK(w[1].offset == 0x110 && w[1].width == 8 && w[1].value == ((uint64_t)0x11112222u << 32 | 0x33334444u));

    /* compiled in: the code's writes patched, a data one left to the cheat, another build's file not applied */
    {
        static unsigned char image[0x1000], build_id[32];
        SnxPatchTarget t;
        SnxPatchStats s;
        uint64_t before;
        for (i = 0; i < 20; ++i) build_id[i] = (unsigned char)(0xb0 + i);
        t.image = image;
        t.image_size = sizeof image;
        t.build_id = build_id;
        t.text_off = 0;
        t.text_size = 0x400;
        memset(&s, 0, sizeof s);
        s.digest = before = SNX_PATCH_DIGEST0;
        CHECK(parse("[c]\n04000000 00000100 D503201F\n04000000 00000800 00000009\n", "c") == 0);
        CHECK(snx_patch_cheat_kind("B0B1B2B3B4B5B6B7.txt") && !snx_patch_cheat_kind("enabled.txt"));
        CHECK(!snx_patch_cheat_kind("B0B1B2B3B4B5B6B7.txt.bak") && !snx_patch_cheat_kind("B0B1B2B3B4B5B6.txt"));
        CHECK(snx_patch_cheats("0102030405060708.txt", &prog, &t, &s, error, sizeof error) == 0);
        CHECK(s.digest == before);
        CHECK(snx_patch_cheats("b0b1b2b3b4b5b6b7.txt", &prog, &t, &s, error, sizeof error) == 1);
        CHECK(image[0x100] == 0x1f && image[0x103] == 0xd5 && image[0x800] == 0);
        CHECK(s.records == 1 && s.text_bytes == 4 && s.digest != before);
        CHECK(parse("[c]\n04000000 00002000 00000001\n", "c") == 0);
        CHECK(snx_patch_cheats("B0B1B2B3B4B5B6B7.txt", &prog, &t, &s, error, sizeof error) == 1); /* data: past code */
        t.text_size = 0x4000;
        CHECK(snx_patch_cheats("B0B1B2B3B4B5B6B7.txt", &prog, &t, &s, error, sizeof error) == -1);
    }
    snx_cheat_free(&prog);
    printf("cheat: %u checks passed\n", checks);
    return 0;
}
