/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture pack (texpack.c): the game's ASTC textures as BC7, made ahead by texpack.exe from the game's own
   archives, for GPUs without ASTC. A texture is found by a hash of its ASTC bytes with their size, format and
   dimensions (Witch Time's design, tex_pack.c), so nothing of the game's files changes, a texture a mod changes is
   simply not found, and anything not found is decoded as before.

   <dir>/index.b7i     "B7PI", u32 version, u32 shards, u32 entries; shards x 32 bytes of file name; the entries, by hash
   <dir>/<family>.b7p  the textures' BC7 blocks one after another, each LZ4 (stored == raw: plain), layer after layer,
                       level after level, rows of blocks; then the shard's own entries, its entry count (u64), version
                       and "B7P1": a shard is complete once renamed from .part, and texpack.exe resumes from those */
#ifndef SNX_TEXPACK_H
#define SNX_TEXPACK_H
#include <stddef.h>
#include "common/cpu/aarch64/cpu.h"

#define TEXPACK_VERSION 1 /* raised with the format or the encoder's settings, so a pack is made again */
typedef struct {
    u64 hash, data_size, offset;
    u32 stored, raw, format, width, height;
    u16 levels, layers, shard, pad;
    u32 pad2;
} TexPackEntry; /* 56 bytes, little endian, in the index and at a shard's end */

/* the key's hash of a texture's n bytes (Witch Time's tex_pack_hash: every whole 8-byte word) */
u64 texpack_hash(const u8* p, size_t n);
/* LZ4 block format: dst holds at least texpack_lz4_bound(n); returns the compressed size */
size_t texpack_lz4_bound(size_t n);
size_t texpack_lz4_compress(const u8* src, size_t n, u8* dst);
/* 1 when src (n bytes) decompresses to exactly dn bytes; never writes outside dst */
int texpack_lz4_decompress(const u8* src, size_t n, u8* dst, size_t dn);

/* the runtime's side: the pack in dir, the number of its textures (0: none usable) */
u32 texpack_open(const char* dir);
const TexPackEntry* texpack_find(u64 hash, u64 data_size, u32 format, u32 width, u32 height, u32 levels, u32 layers);
/* the sizes (up to max, distinct) the pack holds a w x h texture in; the entry of that hash, size and dimensions */
u32 texpack_sizes(u32 width, u32 height, u64* sizes, u32 max);
const TexPackEntry* texpack_find_any(u64 hash, u64 data_size, u32 width, u32 height);
/* e->raw bytes of BC7 into out; 1 when read whole. texpack_read opens e's shard for the read; texpack_read_with keeps
   the shards it opens in files (texpack_shards() of them, NULL at first), for one thread's reads */
int texpack_read(const TexPackEntry* e, u8* out);
u32 texpack_shards(void);
int texpack_read_with(void** files, const TexPackEntry* e, u8* out);

#endif
