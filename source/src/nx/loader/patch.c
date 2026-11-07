/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Code-patch mods (patch.h). The formats as yuzu reads them (src/core/file_sys/ips_layer.cpp, GPL-2.0-or-later, read
   for the formats, not ported): IPS ("PATCH", 24-bit offsets, "EOF") and IPS32 ("IPS32", 32-bit offsets, "EEOF"),
   records of offset, 16-bit size and bytes, a size of 0 meaning a run of one byte; and IPSwitch's text format, lines
   of "@nsobid-<id>", "@flag offset_shift <n>", "@enabled", "@disabled", "@stop", and "<hex offset> <hex bytes>" or
   "<hex offset> \"string\"" (written with a NUL after it). Hex bytes are written in memory order. */
#include "nx/loader/patch.h"
#include <stdio.h>
#include <string.h>

#define NSO_HEADER 0x100u

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

/* the file name's extension is ext, in any case */
static int ends_with(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext), i;
    if (n <= e) return 0;
    for (i = 0; i < e; ++i)
        if (lower((unsigned char)name[n - e + i]) != ext[i]) return 0;
    return 1;
}

int snx_patch_kind(const char* file_name) { return ends_with(file_name, ".ips") || ends_with(file_name, ".pchtxt"); }

/* the n hex digits at id name this build: at least 16 of them, even, and the start of the build ID's hex */
static int same_build(const char* id, size_t n, const unsigned char* build_id) {
    size_t i;
    if (n < 16 || n > 64 || n % 2) return 0;
    for (i = 0; i < n; ++i) {
        int v = hexval((unsigned char)id[i]);
        if (v < 0 || v != (i % 2 ? build_id[i / 2] & 15 : build_id[i / 2] >> 4)) return 0;
    }
    return 1;
}

static uint64_t fnv(uint64_t h, const unsigned char* p, size_t n) {
    const uint64_t prime = (uint64_t)1 << 40 | 0x1b3u;
    while (n--) {
        h ^= *p++;
        h *= prime;
    }
    return h;
}

/* "<file>: <what> 0x<value>" into error, cut to fit (C90 has no snprintf: the parts are bounded instead) */
static int failf(char* error, size_t error_size, const char* file, const char* what, unsigned long value) {
    char message[200];
    size_t n;
    if (!error || !error_size) return -1;
    sprintf(message, "%.100s: %.70s 0x%lx", file, what, value);
    n = strlen(message);
    if (n >= error_size) n = error_size - 1;
    memcpy(error, message, n);
    error[n] = 0;
    return -1;
}

/* size bytes at file offset off (counted with the NSO header) written into the module, from bytes or, when bytes is
   NULL, size copies of fill */
static int put(const char* file, const SnxPatchTarget* t, SnxPatchStats* s, uint64_t off, const unsigned char* bytes,
               unsigned fill, size_t size, char* error, size_t error_size) {
    uint64_t at;
    unsigned char key[16];
    size_t i;
    if (!size) return 0;
    if (off < NSO_HEADER) return 0; /* the header isn't in memory: tools patch it to change nothing that runs */
    at = off - NSO_HEADER;
    if (at > t->image_size || size > t->image_size - at)
        return failf(error, error_size, file, "writes past the module's end at offset", (unsigned long)off);
    if (bytes)
        memcpy(t->image + at, bytes, size);
    else
        memset(t->image + at, (int)fill, size);
    s->records++;
    s->bytes += (unsigned long)size;
    if (at < t->text_off + t->text_size && at + size > t->text_off) {
        uint64_t lo = at > t->text_off ? at : t->text_off, end = at + size;
        if (end > t->text_off + t->text_size) end = t->text_off + t->text_size;
        s->text_bytes += (unsigned long)(end - lo);
    }
    /* every record, data too: the compiler reads rodata (jump tables, constants), so code built without a data patch
       may not match it either */
    for (i = 0; i < 8; ++i) key[i] = (unsigned char)(at >> (8 * i));
    for (i = 0; i < 8; ++i) key[8 + i] = (unsigned char)((uint64_t)size >> (8 * i));
    s->digest = fnv(s->digest, t->build_id, 32);
    s->digest = fnv(s->digest, key, sizeof key);
    s->digest = fnv(s->digest, t->image + at, size);
    return 0;
}

static unsigned long be(const unsigned char* p, unsigned n) {
    unsigned long v = 0;
    while (n--) v = v << 8 | *p++;
    return v;
}

static int apply_ips(const char* file, const unsigned char* d, size_t size, const SnxPatchTarget* t, SnxPatchStats* s,
                     char* error, size_t error_size) {
    int wide = size >= 5 && !memcmp(d, "IPS32", 5);
    unsigned width = wide ? 4 : 3;
    size_t at = 5;
    if (!wide && (size < 5 || memcmp(d, "PATCH", 5))) return failf(error, error_size, file, "is not an IPS patch", 0);
    for (;;) {
        unsigned long off, n;
        if (wide ? at + 4 <= size && !memcmp(d + at, "EEOF", 4) : at + 3 <= size && !memcmp(d + at, "EOF", 3)) return 1;
        if (at + width + 2 > size) return failf(error, error_size, file, "ends inside a record at", (unsigned long)at);
        off = be(d + at, width);
        n = be(d + at + width, 2);
        at += width + 2;
        if (n) {
            if (n > size - at) return failf(error, error_size, file, "ends inside a record at", (unsigned long)at);
            if (put(file, t, s, off, d + at, 0, n, error, error_size)) return -1;
            at += n;
        } else { /* a run: 16-bit count, then the byte */
            if (at + 3 > size) return failf(error, error_size, file, "ends inside a record at", (unsigned long)at);
            if (put(file, t, s, off, NULL, d[at + 2], be(d + at, 2), error, error_size)) return -1;
            at += 3;
        }
    }
}

/* one line of an IPSwitch text patch, from p to end */
typedef struct {
    const char *p, *end;
} Line;

static void skip_space(Line* l) {
    while (l->p < l->end && (*l->p == ' ' || *l->p == '\t')) l->p++;
}

static int word(Line* l, const char* w) {
    size_t n = strlen(w);
    if ((size_t)(l->end - l->p) < n || memcmp(l->p, w, n)) return 0;
    l->p += n;
    return 1;
}

/* a number: hex with or without 0x, as offsets and offset_shift are written */
static int number(Line* l, uint64_t* out) {
    int any = 0;
    *out = 0;
    if (l->end - l->p > 2 && l->p[0] == '0' && (l->p[1] == 'x' || l->p[1] == 'X')) l->p += 2;
    while (l->p < l->end && hexval((unsigned char)*l->p) >= 0) {
        if (*out >> 60) return 0;
        *out = *out << 4 | (uint64_t)hexval((unsigned char)*l->p++);
        any = 1;
    }
    return any;
}

static int apply_pchtxt(const char* file, const unsigned char* d, size_t size, const SnxPatchTarget* t,
                        SnxPatchStats* s, char* error, size_t error_size) {
    const char *text = (const char*)d, *stop = text + size;
    uint64_t shift = 0;
    int mine = -1, enabled = 1;
    unsigned long line_no = 0;
    while (text < stop) {
        Line l;
        const char* nl = memchr(text, '\n', (size_t)(stop - text));
        l.p = text;
        l.end = nl ? nl : stop;
        text = nl ? nl + 1 : stop;
        line_no++;
        while (l.end > l.p && (l.end[-1] == '\r' || l.end[-1] == ' ' || l.end[-1] == '\t')) l.end--;
        skip_space(&l);
        if (l.p == l.end || *l.p == '#' || (l.end - l.p >= 2 && l.p[0] == '/' && l.p[1] == '/')) continue;
        if (*l.p == '@') {
            if (word(&l, "@nsobid-")) {
                const char* id = l.p;
                while (l.p < l.end && hexval((unsigned char)*l.p) >= 0) l.p++;
                mine = same_build(id, (size_t)(l.p - id), t->build_id);
                if (!mine) return 0;
            } else if (word(&l, "@flag")) {
                skip_space(&l);
                if (word(&l, "offset_shift")) {
                    skip_space(&l);
                    if (!number(&l, &shift))
                        return failf(error, error_size, file, "has a bad offset_shift on line", line_no);
                }
            } else if (word(&l, "@enabled"))
                enabled = 1;
            else if (word(&l, "@disabled"))
                enabled = 0;
            else if (word(&l, "@stop"))
                break;
            continue;
        }
        {
            uint64_t off;
            unsigned char bytes[512];
            size_t n = 0;
            if (!number(&l, &off))
                return failf(error, error_size, file, "has a line that isn't a patch: line", line_no);
            skip_space(&l);
            if (l.p < l.end && *l.p == '"') { /* a string, written with a NUL after it */
                for (l.p++; l.p < l.end && *l.p != '"'; l.p++) {
                    int c = (unsigned char)*l.p;
                    if (c == '\\' && l.p + 1 < l.end) {
                        c = (unsigned char)*++l.p;
                        c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == '0' ? 0 : c;
                    }
                    if (n + 1 >= sizeof bytes)
                        return failf(error, error_size, file, "has too long a string on line", line_no);
                    bytes[n++] = (unsigned char)c;
                }
                if (l.p >= l.end) return failf(error, error_size, file, "has an unclosed string on line", line_no);
                bytes[n++] = 0;
            } else {
                while (l.p + 1 < l.end && hexval((unsigned char)l.p[0]) >= 0 && hexval((unsigned char)l.p[1]) >= 0) {
                    if (n >= sizeof bytes)
                        return failf(error, error_size, file, "has too long a value on line", line_no);
                    bytes[n++] = (unsigned char)(hexval((unsigned char)l.p[0]) << 4 | hexval((unsigned char)l.p[1]));
                    l.p += 2;
                }
                skip_space(&l);
                if (!n || (l.p < l.end && !(*l.p == '/' || *l.p == '#')))
                    return failf(error, error_size, file, "has a bad value on line", line_no);
            }
            if (mine < 0) return failf(error, error_size, file, "has a patch before its @nsobid line: line", line_no);
            if (enabled && put(file, t, s, off + shift, bytes, 0, n, error, error_size)) return -1;
        }
    }
    if (mine < 0) return failf(error, error_size, file, "has no @nsobid line", 0);
    return 1;
}

int snx_patch_apply(const char* file_name, const unsigned char* data, size_t size, const SnxPatchTarget* t,
                    SnxPatchStats* stats, char* error, size_t error_size) {
    const char* base = file_name;
    const char* p;
    size_t n;
    for (p = file_name; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (ends_with(base, ".pchtxt")) return apply_pchtxt(base, data, size, t, stats, error, error_size);
    if (!ends_with(base, ".ips")) return 0;
    n = strlen(base) - 4;
    if (!same_build(base, n, t->build_id)) return 0;
    return apply_ips(base, data, size, t, stats, error, error_size);
}

size_t snx_patch_mod_names(char* text, char** names, size_t max) {
    size_t count = 0, i;
    char* p = text;
    while (p && *p && count < max) {
        char *line = p, *end = strchr(p, '\n'), *hash, *s;
        int bad = 0;
        p = end ? end + 1 : NULL;
        if (end) *end = 0;
        if ((hash = strchr(line, '#')) != NULL) *hash = 0;
        while (*line == ' ' || *line == '\t') line++;
        end = line + strlen(line);
        while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
        if (!*line || !strcmp(line, ".") || !strcmp(line, "..")) continue;
        for (s = line; *s; ++s)
            if (*s == '/' || *s == '\\' || *s == ':') bad = 1;
        if (bad) continue;
        for (i = 0; i < count && strcmp(names[i], line); ++i) {}
        if (i == count) names[count++] = line;
    }
    return count;
}
