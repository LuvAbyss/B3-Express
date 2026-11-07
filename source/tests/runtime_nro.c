/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real runtime loader with synthetic files and a bounded guest mapping. */
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/loader/patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 file[0x4000], guest[0x5000];
static size_t file_size;
static int root_file, nso, maps, unmaps, permissions;
static unsigned checks;
static SnxModInfo info;
static SnxModule module;
const SnxModule* g_mod = &module;
u8* g_mem = guest;
u64 g_code_memory;
char g_datadir[1024];

#define CHECK(v)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(v)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

char g_exedir[1024], g_exename[256] = "snx-test-runtime-patches";
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}
/* the code-patch mods (patch.h): mods.txt naming mod "m", whose exefs/ holds one IPS patch for build 4242... */
static const char* mods_txt;
static const u8* ips;
static size_t ips_size;
u8* fs_read_file(const char* path, u64* size) {
    const void* src = !strcmp(path, "mods/mods.txt")                       ? (const void*)mods_txt
                      : !strcmp(path, "mods/m/exefs/4242424242424242.ips") ? (const void*)ips
                                                                           : NULL;
    size_t n = src == (const void*)mods_txt && src ? strlen(mods_txt) : ips_size;
    if (!src) return NULL;
    u8* out = malloc(n + 1);
    CHECK(out != NULL);
    memcpy(out, src, n);
    *size = n;
    return out;
}
char** fs_list_files(const char* dir, u32* count) {
    *count = 0;
    if (!ips || strcmp(dir, "mods/m/exefs")) return NULL;
    char** names = malloc(sizeof(char*) + 32);
    CHECK(names != NULL);
    names[0] = strcpy((char*)(names + 1), "4242424242424242.ips");
    *count = 1;
    return names;
}
int fs_map_file(const char* path, PFileMap* map) {
    if (nso) {
        if (strcmp(path, "exefs/main")) return -1;
    } else {
        /* A stale NSO named main must not shadow main.nro. */
        CHECK(strcmp(path, "exefs/main") && strcmp(path, "main"));
        if (strcmp(path, root_file ? "main.nro" : "exefs/main.nro")) return -1;
    }
    u8* bytes = malloc(file_size ? file_size : 1);
    CHECK(bytes != NULL);
    memcpy(bytes, file, file_size);
    map->data = bytes;
    map->size = file_size;
    ++maps;
    return 0;
}
void plat_file_unmap(PFileMap* map) {
    free((void*)map->data);
    ++unmaps;
}
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    CHECK(addr == info.base && size == info.size && addr + size <= sizeof guest);
    CHECK(state == MS_CODE && perm == PERM_RW);
    memset(guest + addr, 0xcc, (size_t)size);
    return 0;
}
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm) {
    static const u32 perms[] = {PERM_RX, PERM_R, PERM_RW};
    CHECK(permissions < 3 && perm == perms[permissions]);
    CHECK(addr == info.base + (u64)permissions * 0x1000);
    CHECK(size == (permissions == 2 ? 0x2000 : 0x1000));
    CHECK(state == (permissions == 2 ? MS_CODEDATA : MS_CODE));
    ++permissions;
    return 0;
}
static void put32(size_t off, u32 value) {
    for (unsigned i = 0; i < 4; ++i) file[off + i] = (u8)(value >> (8 * i));
}
static void fixture(void) {
    memset(file, 0, sizeof file);
    memset(&info, 0, sizeof info);
    memcpy(file + 0x10, "NRO0", 4);
    put32(0x18, 0x3000);
    put32(0x24, 0x1000);
    put32(0x28, 0x1000);
    put32(0x2c, 0x1000);
    put32(0x30, 0x2000);
    put32(0x34, 0x1000);
    put32(0x38, 0x1000);
    memset(file + 0x40, 0x42, 32);
    file[0x100] = 0x71;
    file[0x1000] = 0x72;
    file[0x2000] = 0x73;
    memset(file + 0x3000, 0xff, 0x40);
    memcpy(file + 0x3000, "ASET", 4);
    file_size = sizeof file;
    info.name = "main";
    info.base = 0x1000;
    info.size = 0x4000;
    memset(info.build_id, 0x42, 32);
    info.text_size = info.ro_size = info.data_size = info.bss_size = 0x1000;
    info.ro_off = 0x1000;
    info.data_off = 0x2000;
    info.is_nro = 1;
    module.nmods = 1;
    module.mods = &info;
    maps = unmaps = permissions = 0;
    g_code_memory = 0;
}
/* An NSO laid out as the NRO above: its text LZ4-compressed (one block of literals), rodata and data stored */
static void nso_fixture(void) {
    size_t at = 0x100, i, rest;
    fixture();
    nso = 1;
    memset(file, 0, sizeof file);
    memcpy(file, "NSO0", 4);
    put32(0x0c, 1);
    memset(file + 0x40, 0x42, 32);
    put32(0x3c, 0x1000);
    put32(0x10, (u32)at);
    put32(0x18, 0x1000);
    file[at++] = 0xf0;
    for (rest = 0x1000 - 15; rest >= 255; rest -= 255) file[at++] = 255;
    file[at++] = (u8)rest;
    for (i = 0; i < 0x1000; ++i) file[at + i] = (u8)(i * 7);
    put32(0x60, (u32)(at + 0x1000 - 0x100));
    at += 0x1000;
    for (i = 1; i < 3; ++i) {
        put32(0x10 + i * 0x10, (u32)at);
        put32(0x14 + i * 0x10, (u32)i * 0x1000);
        put32(0x18 + i * 0x10, 0x1000);
        memset(file + at, 0x70 + (int)i, 0x1000);
        at += 0x1000;
    }
    file_size = at;
    info.is_nro = 0;
}
static void rejected(void) {
    CHECK(loader_load() == -1);
    CHECK(maps == 1 && unmaps == 1 && !permissions && !g_code_memory);
    for (size_t i = info.base; i < info.base + info.size; ++i) CHECK(guest[i] == 0xcc);
}
int main(void) {
    for (root_file = 0; root_file < 2; ++root_file) {
        fixture();
        if (root_file) {
            file_size = 0x3000; /* optional assets */
            put32(0x38, 0x40);
            info.bss_size = 0x40; /* the rest of the last page is mapping padding */
        }
        CHECK(!loader_load());
        CHECK(maps == 1 && unmaps == 1 && permissions == 3 && g_code_memory == info.size);
        CHECK(!memcmp(guest + info.base, file, 0x3000));
        for (size_t i = 0x3000; i < info.size; ++i) CHECK(guest[info.base + i] == 0);
    }
    root_file = 0;
    for (size_t i = 0; i < 0x80; ++i) {
        fixture();
        file_size = i;
        rejected();
    }
    static const u32 offsets[] = {0x10, 0x18, 0x18, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0x40};
    static const u32 values[] = {0, 0x7f, 0x4000, 0xffffffff, 2, 4, 0x800, 0x1004, 0x2004, 0xffffffff, 0xffffffff, 0};
    for (size_t i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
        fixture();
        put32(offsets[i], values[i]);
        rejected();
    }
    fixture();
    file_size = 0x2fff;
    rejected();
    fixture();
    info.size = 0x3000; /* no room for BSS */
    rejected();
    fixture();
    info.ro_size -= 4; /* the recompiled descriptor must match the file */
    rejected();
    fixture();
    info.bss_size = 0;
    rejected();
    nso_fixture();
    CHECK(!loader_load());
    CHECK(maps == 1 && unmaps == 1 && permissions == 3 && g_code_memory == info.size);
    for (size_t i = 0; i < 0x1000; ++i) CHECK(guest[info.base + i] == (u8)(i * 7));
    CHECK(guest[info.base + 0x1000] == 0x71 && guest[info.base + 0x2fff] == 0x72);
    for (size_t i = 0x3000; i < info.size; ++i) CHECK(guest[info.base + i] == 0);
    nso_fixture();
    file[0x40] ^= 1; /* another build */
    rejected();
    nso_fixture();
    put32(0x38, 0x4000); /* data past the mapping */
    rejected();
    nso_fixture();
    file_size -= 1; /* data past the file */
    rejected();
    nso_fixture();
    put32(0x60, 0x100); /* a block cut short */
    CHECK(loader_load() == -1 && unmaps == 1 && !permissions && !g_code_memory);

    /* a code patch: two text bytes and one rodata byte, offsets counted with the NSO's 0x100-byte header */
    static const u8 patch[] = {'P',  'A', 'T',  'C',  'H', 0, 0x01, 0x10, 0,   2,  0xab,
                               0xcd, 0,   0x11, 0x05, 0,   1, 0x99, 'E',  'O', 'F'};
    char patches_file[300];
    snprintf(patches_file, sizeof patches_file, "%s.patches", g_exename);
    remove(patches_file);
    mods_txt = "# on\nm\n";
    ips = patch;
    ips_size = sizeof patch;
    nso_fixture();
    /* code built without the patch: refused before any page gets its permissions */
    CHECK(loader_load() == -1 && !permissions && !g_code_memory);
    {
        /* the digest the recompiler would record, from the same patch on the same module */
        static u8 image[0x4000];
        SnxPatchTarget t = {image, sizeof image, info.build_id, 0, 0x1000};
        SnxPatchStats s = {0, 0, 0, SNX_PATCH_DIGEST0};
        char error[200];
        CHECK(snx_patch_apply("4242424242424242.ips", patch, sizeof patch, &t, &s, error, sizeof error) == 1);
        CHECK(s.records == 2 && s.bytes == 3 && s.text_bytes == 2);
        FILE* f = fopen(patches_file, "wb");
        CHECK(f != NULL);
        fprintf(f, "digest %08lx%08lx\nfiles 1\nbytes 3\n", (unsigned long)(s.digest >> 32),
                (unsigned long)(s.digest & 0xffffffffu));
        CHECK(!fclose(f));
    }
    nso_fixture();
    CHECK(!loader_load() && permissions == 3);
    CHECK(guest[info.base + 0x10] == 0xab && guest[info.base + 0x11] == 0xcd &&
          guest[info.base + 0x12] == (u8)(0x12 * 7));
    CHECK(guest[info.base + 0x1005] == 0x99 && guest[info.base + 0x1004] == 0x71);
    /* the patch turned off again: the code built with it no longer matches */
    mods_txt = "# none\n";
    nso_fixture();
    CHECK(loader_load() == -1 && !permissions);
    remove(patches_file);
    mods_txt = NULL;
    ips = NULL;
    printf("Runtime NRO and NSO: %u checks passed\n", checks);
    return 0;
}
