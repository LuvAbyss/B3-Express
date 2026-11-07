/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's loader: maps each module the game module was recompiled from (g_mod's list) at its base, from the
   game's data folder, after checking the file is that module, then gives its pages the permissions the console
   would. Written from Switchbrew's NSO and NRO pages and what the recompiler (nx/loader/loader.c) records */
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/loader/patch.h"
#include <stdlib.h>
#include <lz4.h>
#include <stdio.h>
#include <string.h>

u64 g_code_lo, g_code_hi;

#define PAGE 0x1000ull

static u32 rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 page_up(u64 v) { return (v + PAGE - 1) & ~(PAGE - 1); }
static u64 page_down(u64 v) { return v & ~(PAGE - 1); }
static int inside(u64 offset, u64 length, u64 total) { return offset <= total && length <= total - offset; }

/* The module's file: exefs/<file> first, then <file> at the data folder's root. An NRO is <name>.nro, so a stale
   NSO named main can't stand in for main.nro */
static int open_module(const SnxModInfo* mi, PFileMap* map) {
    char file[300], path[320];
    snprintf(file, sizeof file, "%s%s", mi->name, mi->is_nro ? ".nro" : "");
    snprintf(path, sizeof path, "exefs/%s", file);
    if (!fs_map_file(path, map)) return 0;
    if (!fs_map_file(file, map)) return 0;
    WARN("The game's module %s is missing from %sexefs/\n", file, g_datadir);
    return -1;
}

/* An NRO: the image is the file's start, its segments one after another (as the recompiler requires), assets after
   it never mapped. -1 for a file that isn't a usable NRO, -2 for one that isn't the NRO recompiled */
static int check_nro(const SnxModInfo* mi, const PFileMap* map) {
    const u8* f = map->data;
    u32 image, text_off, text_size, ro_off, ro_size, data_off, data_size, bss;
    if (map->size < 0x80 || memcmp(f + 0x10, "NRO0", 4) || (rd32(f + 0x1c) & 2)) goto invalid;
    image = rd32(f + 0x18);
    text_off = rd32(f + 0x20);
    text_size = rd32(f + 0x24);
    ro_off = rd32(f + 0x28);
    ro_size = rd32(f + 0x2c);
    data_off = rd32(f + 0x30);
    data_size = rd32(f + 0x34);
    bss = rd32(f + 0x38);
    if (image < 0x80 || image > map->size || image > mi->size || (u64)bss > mi->size - image) goto invalid;
    if (text_off || text_size < 0x80 || ro_off != text_size || (u64)ro_off + ro_size != data_off ||
        (u64)data_off + data_size != image)
        goto invalid;
    if (memcmp(f + 0x40, mi->build_id, 32) || text_off != mi->text_off || text_size != mi->text_size ||
        ro_off != mi->ro_off || ro_size != mi->ro_size || data_off != mi->data_off || data_size != mi->data_size ||
        bss != mi->bss_size) {
        WARN("%s.nro is not the NRO that was recompiled: recompile the game\n", mi->name);
        return -2;
    }
    return 0;
invalid:
    WARN("%s.nro: invalid or unsupported NRO layout\n", mi->name);
    return -1;
}

/* An NSO: every segment's stored bytes inside the file and its memory inside the mapping. -2 for another build */
static int check_nso(const SnxModInfo* mi, const PFileMap* map) {
    const u8* f = map->data;
    u32 flags, i;
    if (map->size < 0x100 || memcmp(f, "NSO0", 4)) {
        WARN("%s is not an NSO\n", mi->name);
        return -1;
    }
    flags = rd32(f + 0x0c);
    for (i = 0; i < 3; i++) {
        u32 file_off = rd32(f + 0x10 + i * 0x10), mem_off = rd32(f + 0x14 + i * 0x10), size = rd32(f + 0x18 + i * 0x10);
        u32 stored = flags & (1u << i) ? rd32(f + 0x60 + i * 4) : size;
        if (!inside(file_off, stored, map->size) || !inside(mem_off, size, mi->size)) {
            WARN("%s: segment %u lies outside the file or the module\n", mi->name, (unsigned)i);
            return -1;
        }
    }
    if (memcmp(f + 0x40, mi->build_id, 32) || rd32(f + 0x14) != mi->text_off || rd32(f + 0x18) != mi->text_size ||
        rd32(f + 0x24) != mi->ro_off || rd32(f + 0x28) != mi->ro_size || rd32(f + 0x34) != mi->data_off ||
        rd32(f + 0x38) != mi->data_size) {
        WARN("%s is not the NSO that was recompiled: recompile the game\n", mi->name);
        return -2;
    }
    return 0;
}

/* The NSO's segments, decompressed where they go. The mapping is already zeroed */
static int copy_nso(const SnxModInfo* mi, const PFileMap* map) {
    const u8* f = map->data;
    u32 flags = rd32(f + 0x0c), i;
    for (i = 0; i < 3; i++) {
        u32 file_off = rd32(f + 0x10 + i * 0x10), mem_off = rd32(f + 0x14 + i * 0x10), size = rd32(f + 0x18 + i * 0x10);
        u8* out = GP(mi->base + mem_off);
        if (!(flags & (1u << i))) {
            memcpy(out, f + file_off, size);
            continue;
        }
        if (!size) continue;
        u32 stored = rd32(f + 0x60 + i * 4);
        if (stored > LZ4_MAX_INPUT_SIZE || size > LZ4_MAX_INPUT_SIZE ||
            LZ4_decompress_safe((const char*)f + file_off, (char*)out, (int)stored, (int)size) != (int)size) {
            WARN("LZ4 failed in %s\n", mi->name);
            return -1;
        }
    }
    return 0;
}

/* Text executable, rodata read-only, data and BSS writable to the end of the mapping. A byte-packed NRO can share a
   page between segments: the page takes the earlier segment's permission for text, the later one's for data */
static int protect(const SnxModInfo* mi) {
    u64 text_end = page_up((u64)mi->text_off + mi->text_size);
    u64 data_start = page_down(mi->data_off);
    if (data_start < text_end) data_start = text_end;
    if (text_end > page_down(mi->text_off) &&
        mem_set_state(mi->base + page_down(mi->text_off), text_end - page_down(mi->text_off), MS_CODE, PERM_RX))
        return -1;
    if (data_start > text_end && mem_set_state(mi->base + text_end, data_start - text_end, MS_CODE, PERM_R)) return -1;
    if (mi->size > data_start && mem_set_state(mi->base + data_start, mi->size - data_start, MS_CODEDATA, PERM_RW))
        return -1;
    return 0;
}

static int load_module(const SnxModInfo* mi) {
    PFileMap map;
    int rc;
    memset(&map, 0, sizeof map);
    if (open_module(mi, &map)) return -1;
    if (mem_map(mi->base, mi->size, MS_CODE, PERM_RW)) {
        WARN("Cannot map %s at 0x%llx (0x%llx bytes)\n", mi->name, (unsigned long long)mi->base,
             (unsigned long long)mi->size);
        plat_file_unmap(&map);
        return -1;
    }
    /* everything is checked before guest memory is written */
    rc = mi->is_nro ? check_nro(mi, &map) : check_nso(mi, &map);
    if (!rc) {
        memset(GP(mi->base), 0, (size_t)mi->size);
        if (mi->is_nro)
            memcpy(GP(mi->base), map.data, rd32(map.data + 0x18));
        else
            rc = copy_nso(mi, &map);
    }
    plat_file_unmap(&map);
    return rc;
}

/* The digest of the code patches the game module was built with (<game>.patches beside it, written by the
   recompiler), SNX_PATCH_DIGEST0 when there is none (code built before code-patch mods, so with none) */
static u64 built_digest(void) {
    char path[1400], text[200];
    unsigned long hi, lo;
    FILE* f;
    size_t n;
    snprintf(path, sizeof path, "%s%s%s.patches", g_exedir,
             g_exedir[0] && g_exedir[strlen(g_exedir) - 1] != '/' && g_exedir[strlen(g_exedir) - 1] != '\\' ? "/" : "",
             g_exename);
    f = fopen(path, "rb");
    if (!f) return SNX_PATCH_DIGEST0;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    if (sscanf(text, "digest %8lx%8lx", &hi, &lo) != 2) return 0; /* unreadable: matches nothing */
    return (u64)hi << 32 | lo;
}

/* The code-patch mods on (patch.h) applied to the loaded modules, in the recompiler's order: mods.txt's mods, each
   mod's exefs/ files by name, each file to every module it names. -1 when a patch is malformed, or when the game code
   was built with other patches, which would run code that doesn't match its data */
static int apply_patches(void) {
    u64 size = 0;
    char *text = (char*)fs_read_file("mods/mods.txt", &size), *names[256];
    size_t count = 0;
    SnxPatchStats stats;
    unsigned files = 0;
    char error[300];
    memset(&stats, 0, sizeof stats);
    stats.digest = SNX_PATCH_DIGEST0;
    if (text) {
        char* t = realloc(text, (size_t)size + 1);
        if (!t) fatal("Out of memory reading mods.txt\n");
        text = t;
        text[size] = 0;
        if (strlen(text) == size) count = snx_patch_mod_names(text, names, sizeof names / sizeof names[0]);
    }
    for (size_t i = 0; i < count; i++) {
        char dir[600];
        u32 nfiles;
        char** list;
        snprintf(dir, sizeof dir, "mods/%s/exefs", names[i]);
        list = fs_list_files(dir, &nfiles);
        for (u32 j = 0; j < nfiles; j++) {
            char path[900];
            u64 data_size;
            u8* data;
            int applied = 0;
            if (!snx_patch_kind(list[j])) continue;
            snprintf(path, sizeof path, "%s/%s", dir, list[j]);
            if (!(data = fs_read_file(path, &data_size))) continue;
            for (u32 k = 0; k < g_mod->nmods; k++) {
                const SnxModInfo* mi = &g_mod->mods[k];
                SnxPatchTarget target = {GP(mi->base), (size_t)mi->size, mi->build_id, mi->text_off, mi->text_size};
                int got = mi->is_nro
                              ? 0
                              : snx_patch_apply(list[j], data, (size_t)data_size, &target, &stats, error, sizeof error);
                if (got < 0) {
                    WARN("mods: %s: %s\n", names[i], error);
                    free(data);
                    free(list);
                    free(text);
                    return -1;
                }
                if (got > 0) {
                    applied = 1;
                    LOG("mods: %s: %s patches %s\n", names[i], list[j], mi->name);
                }
            }
            files += (unsigned)applied;
            free(data);
        }
        free(list);
    }
    free(text);
    if (files)
        LOG("mods: %u code patch files, %lu bytes patched (%lu in code)\n", files, stats.bytes, stats.text_bytes);
    if (stats.digest != built_digest()) {
        WARN("mods: the game code was built with other code mods than the ones on now: build it again (the "
             "launcher does when you press Play)\n");
        return -1;
    }
    return 0;
}

int loader_load(void) {
    for (u32 i = 0; i < g_mod->nmods; i++)
        if (load_module(&g_mod->mods[i])) return -1;
    /* patched while the pages are still writable, before their permissions are set */
    if (apply_patches()) return -1;
    for (u32 i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* mi = &g_mod->mods[i];
        if (protect(mi)) {
            WARN("Cannot set the permissions of %s\n", mi->name);
            return -1;
        }
        /* the whole mapping counts as the process's code memory (the guard pages between modules don't) */
        g_code_memory += mi->size;
        LOG("Loaded %s at 0x%llx\n", mi->name, (unsigned long long)mi->base);
    }
    return 0;
}
