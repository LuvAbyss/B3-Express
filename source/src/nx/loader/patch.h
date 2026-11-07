/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Code-patch mods: the files in mods/<name>/exefs/ of the game's data folder, laid out as yuzu's and Ryujinx's are.
   An IPS or IPS32 patch is named after the build ID of the module it patches (<build id>.ips, at least 16 hex
   digits of it); an IPSwitch text patch (.pchtxt) names it inside (@nsobid-<build id>). Offsets count from the start
   of the NSO file with its 0x100-byte header, as those tools write them. The recompiler applies them before compiling,
   so patched instructions are what gets compiled, and the runtime applies them again to the module in guest memory,
   so patched data is what the game reads. Both go through this file, in mods.txt's order (a later mod wins) */
#ifndef SNX_NX_PATCH_H
#define SNX_NX_PATCH_H

#include <stddef.h>
#include <stdint.h>

/* one module's memory image, as the loaders lay it out */
typedef struct {
    unsigned char* image;
    size_t image_size;
    const unsigned char* build_id; /* 32 bytes */
    uint64_t text_off, text_size;
} SnxPatchTarget;

/* what the patches applied so far did. digest starts at SNX_PATCH_DIGEST0 and takes in every patch record (the build
   ID, its offset, size and bytes), in order: the recompiler records it beside the code it built (<game>.patches), and
   the runtime refuses to run code built with other code patches than the ones on */
typedef struct {
    unsigned long records, bytes, text_bytes;
    uint64_t digest;
} SnxPatchStats;
#define SNX_PATCH_DIGEST0                                                                                              \
    ((uint64_t)0xcbf29ce4u << 32 | 0x84222325u) /* FNV-1a's offset basis (C90 has no 64-bit literal) */

/* 1 for a .ips or .pchtxt file name (in any case), else 0 */
int snx_patch_kind(const char* file_name);
/* Applies the patch file to t when it is meant for t's build: 1 applied, 0 meant for another module, -1 malformed or
   writing outside the module (error says why, and the image may be partly patched) */
int snx_patch_apply(const char* file_name, const unsigned char* data, size_t size, const SnxPatchTarget* t,
                    SnxPatchStats* stats, char* error, size_t error_size);
/* The mod names mods.txt turns on, in its order, as the RomFS overlay reads them: text is changed in place, names[]
   points into it. '#' starts a comment, blank lines are skipped, a name that is a path (/ \ :, ".", "..") is left
   out, and a name given twice counts once. The count, at most max */
size_t snx_patch_mod_names(char* text, char** names, size_t max);

#endif
