/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's files as the runtime itself reads them (the loader, shared fonts, the control data), beside the fsp-srv
   service in fs.c */
#ifndef SNX_FS_H
#define SNX_FS_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* FILE, PFileMap */

/* Indexes the game's data folder and maps RomFS and ExeFS for the run. Called once, before any guest file is loaded */
void fs_init(void);
/* Lookups by a path in the index. *size is 0 when the file isn't there */
FILE* fs_open_read(const char* path, u64* size);
int fs_map_file(const char* path, PFileMap* map); /* 0, or -1 */
u8* fs_read_file(const char* path, u64* size);    /* malloc'd, or NULL */
/* a file that exists only in memory, which the lookups find like any other. 0, or -1 */
int fs_add_virtual_file(const char* path, const void* data, u64 size);
/* the RomFS the game opens: romfs.bin as it is, or an image built from the romfs folder */
void romfs_init(void);
#endif
