/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture pack's format, hash and LZ4, and its reader (texpack.h). The reader opens a shard for each read: the
   textures read are few next to what they save, and a read may come from any thread holding the renderer */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/texture/texpack.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#else
#define fseek64 fseeko
#endif

u64 texpack_hash(const u8* p, size_t n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ (u64)n, w;
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

/* ---- LZ4 blocks (the format of lz4's lz4_Block_format.md): a greedy match finder with one candidate per hash */
size_t texpack_lz4_bound(size_t n) { return n + n / 255 + 16; }

static u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static u8* put_len(u8* o, size_t v) { /* the bytes past a token's 15 */
    while (v >= 255) {
        *o++ = 255;
        v -= 255;
    }
    *o++ = (u8)v;
    return o;
}
size_t texpack_lz4_compress(const u8* src, size_t n, u8* dst) {
    enum { HBITS = 14, MINMATCH = 4, LASTLITERALS = 5, MFLIMIT = 12 };
    u32* table = calloc((size_t)1 << HBITS, sizeof *table); /* position + 1 of the last 4 bytes with that hash */
    size_t i = 0, anchor = 0;
    u8* o = dst;
    if (!table) { /* all literals */
        u8* t = o++;
        *t = (u8)((n < 15 ? n : 15) << 4);
        if (n >= 15) o = put_len(o, n - 15);
        memcpy(o, src, n);
        return (size_t)(o - dst) + n;
    }
    while (n >= MFLIMIT && i + MFLIMIT <= n) {
        u32 h = (rd32(src + i) * 2654435761u) >> (32 - HBITS), cand = table[h];
        size_t m = 0, lit, ref;
        table[h] = (u32)i + 1;
        if (cand && i - (cand - 1) <= 65535 && rd32(src + cand - 1) == rd32(src + i)) {
            ref = cand - 1;
            m = MINMATCH;
            while (i + m + LASTLITERALS < n && src[ref + m] == src[i + m]) m++;
        }
        if (!m) {
            i++;
            continue;
        }
        while (i > anchor && ref > 0 && src[i - 1] == src[ref - 1]) { /* backwards over the literals */
            i--;
            ref--;
            m++;
        }
        lit = i - anchor;
        {
            u8* t = o++;
            *t = (u8)((lit < 15 ? lit : 15) << 4 | (m - MINMATCH < 15 ? m - MINMATCH : 15));
            if (lit >= 15) o = put_len(o, lit - 15);
            memcpy(o, src + anchor, lit);
            o += lit;
            o[0] = (u8)(i - ref);
            o[1] = (u8)((i - ref) >> 8);
            o += 2;
            if (m - MINMATCH >= 15) o = put_len(o, m - MINMATCH - 15);
        }
        i += m;
        anchor = i;
    }
    { /* the last literals */
        size_t lit = n - anchor;
        u8* t = o++;
        *t = (u8)((lit < 15 ? lit : 15) << 4);
        if (lit >= 15) o = put_len(o, lit - 15);
        memcpy(o, src + anchor, lit);
        o += lit;
    }
    free(table);
    return (size_t)(o - dst);
}

int texpack_lz4_decompress(const u8* s, size_t sn, u8* out, size_t dn) {
    const u8* se = s + sn;
    size_t o = 0;
    while (s < se) {
        unsigned tok = *s++;
        size_t lit = tok >> 4, len = tok & 15, off, i;
        if (lit == 15) {
            unsigned b;
            do {
                if (s >= se) return 0;
                b = *s++;
                lit += b;
            } while (b == 255);
        }
        if (lit > (size_t)(se - s) || lit > dn - o) return 0;
        memcpy(out + o, s, lit);
        s += lit;
        o += lit;
        if (s == se) break; /* the last sequence has no match */
        if (se - s < 2) return 0;
        off = (size_t)s[0] | (size_t)s[1] << 8;
        s += 2;
        if (!off || off > o) return 0;
        if (len == 15) {
            unsigned b;
            do {
                if (s >= se) return 0;
                b = *s++;
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (len > dn - o) return 0;
        for (i = 0; i < len; i++) out[o + i] = out[o + i - off]; /* overlaps on purpose */
        o += len;
    }
    return o == dn;
}

/* ---- the reader */
static TexPackEntry* g_e;
static u32 g_n, g_shards;
static char (*g_name)[32];
static char g_dir[512];

u32 texpack_open(const char* dir) {
    char path[600];
    u32 head[4], i;
    FILE* f;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(path, sizeof path, "%s/index.b7i", dir);
    if (!(f = fopen(path, "rb"))) return 0;
    if (fread(head, 4, 4, f) == 4 && !memcmp(head, "B7PI", 4) && head[1] == TEXPACK_VERSION && head[2] <= 256 &&
        head[3] <= 1u << 24) {
        g_name = malloc((size_t)head[2] * 32 + 1);
        g_e = malloc((size_t)head[3] * sizeof *g_e + 1);
        if (g_name && g_e && fread(g_name, 32, head[2], f) == head[2] &&
            fread(g_e, sizeof *g_e, head[3], f) == head[3]) {
            g_shards = head[2];
            g_n = head[3];
        }
    }
    fclose(f);
    for (i = 0; i < g_shards; i++) g_name[i][31] = 0;
    return g_n;
}

const TexPackEntry* texpack_find(u64 hash, u64 data_size, u32 format, u32 width, u32 height, u32 levels, u32 layers) {
    u32 lo = 0, hi = g_n;
    while (lo < hi) {
        u32 m = lo + (hi - lo) / 2;
        if (g_e[m].hash < hash)
            lo = m + 1;
        else
            hi = m;
    }
    for (; lo < g_n && g_e[lo].hash == hash; lo++) {
        const TexPackEntry* e = &g_e[lo];
        if (e->data_size == data_size && e->format == format && e->width == width && e->height == height &&
            e->levels == levels && e->layers == layers)
            return e;
    }
    return NULL;
}

u32 texpack_sizes(u32 width, u32 height, u64* sizes, u32 max) {
    u32 i, k, n = 0;
    for (i = 0; i < g_n; i++) {
        if (g_e[i].width != width || g_e[i].height != height) continue;
        for (k = 0; k < n && sizes[k] != g_e[i].data_size; k++) {}
        if (k == n && n < max) sizes[n++] = g_e[i].data_size;
    }
    return n;
}

const TexPackEntry* texpack_find_any(u64 hash, u64 data_size, u32 width, u32 height) {
    u32 lo = 0, hi = g_n;
    while (lo < hi) {
        u32 m = lo + (hi - lo) / 2;
        if (g_e[m].hash < hash)
            lo = m + 1;
        else
            hi = m;
    }
    for (; lo < g_n && g_e[lo].hash == hash; lo++)
        if (g_e[lo].data_size == data_size && g_e[lo].width == width && g_e[lo].height == height) return &g_e[lo];
    return NULL;
}

u32 texpack_shards(void) { return g_shards; }

static FILE* open_shard(u32 shard) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", g_dir, g_name[shard]);
    return fopen(path, "rb");
}
static int read_entry(FILE* f, const TexPackEntry* e, u8* out) {
    u8* src = e->stored == e->raw ? out : malloc(e->stored ? e->stored : 1);
    int ok = src && !fseek64(f, (long long)e->offset, SEEK_SET) && fread(src, 1, e->stored, f) == e->stored;
    if (ok && src != out) ok = texpack_lz4_decompress(src, e->stored, out, e->raw);
    if (src != out) free(src);
    return ok;
}

int texpack_read(const TexPackEntry* e, u8* out) {
    FILE* f;
    int ok;
    if (e->shard >= g_shards || !(f = open_shard(e->shard))) return 0;
    ok = read_entry(f, e, out);
    fclose(f);
    return ok;
}

int texpack_read_with(void** files, const TexPackEntry* e, u8* out) {
    if (e->shard >= g_shards) return 0;
    if (!files[e->shard] && !(files[e->shard] = open_shard(e->shard))) return 0;
    return read_entry(files[e->shard], e, out);
}
