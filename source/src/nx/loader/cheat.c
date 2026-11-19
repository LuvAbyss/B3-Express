/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Cheat codes (cheat.h). Each instruction's first word holds its type in the top nibble (two nibbles for 0xC-0xF,
   three for 0xF0-0xFF) and its fields in the nibbles below, as Atmosphère's cheats.md lays them out */
#include "nx/loader/cheat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* instructions in one run at most, against a loop that never ends (a count of 0 runs 2^64 times) */
#define BUDGET 0x100000u

#define NIB(w, i) ((unsigned)((w) >> (4 * (i))) & 15u)

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int width_ok(unsigned w) { return w == 1 || w == 2 || w == 4 || w == 8; }
static uint64_t cut(unsigned width, uint64_t v) { return width >= 8 ? v : v & (((uint64_t)1 << (8 * width)) - 1); }
static uint64_t wide(const uint32_t* w) { return (uint64_t)w[0] << 32 | w[1]; }
/* an immediate value of width bytes: two words for 8, else one */
static uint64_t imm(const uint32_t* w, unsigned width) { return width == 8 ? wide(w) : w[0]; }
/* the 40-bit address of 0x0, 0x1 and 0x5: the low byte of the first word, then the second */
static uint64_t addr40(const uint32_t* w) { return (uint64_t)(w[0] & 0xffu) << 32 | w[1]; }
/* the 36-bit address of 0xA, 0xC0 and 0xFFF: the low nibble of the first word, then the second */
static uint64_t addr36(const uint32_t* w) { return (uint64_t)(w[0] & 15u) << 32 | w[1]; }

/* the words the instruction at op[at] takes, 0 when it is unknown or cut off */
static size_t op_size(const uint32_t* op, size_t at, size_t n) {
    uint32_t w = op[at];
    unsigned t = NIB(w, 7), width = NIB(w, 6);
    size_t size = 0;
    switch (t) {
        case 0x0:
        case 0x1: size = width == 8 ? 4 : 3; break;
        case 0x2: size = 1; break;
        case 0x3: size = NIB(w, 6) ? 1 : 2; break;
        case 0x4: size = 3; break;
        case 0x5: size = 2; break;
        case 0x6: size = 3; break;
        case 0x7: size = 2; break;
        case 0x8: size = 1; break;
        case 0x9: size = NIB(w, 2) ? (width == 8 ? 3 : 2) : 1; break;
        case 0xA: size = NIB(w, 2) == 2 || NIB(w, 2) == 4 || NIB(w, 2) == 5 ? 2 : 1; break;
        case 0xC:
            switch (NIB(w, 6)) {
                case 0x0:
                    switch (NIB(w, 2)) {
                        case 0:
                        case 2: size = 2; break;
                        case 4: size = NIB(w, 5) == 8 ? 3 : 2; break;
                        case 1:
                        case 3:
                        case 5: size = 1; break;
                        default: size = 0; break;
                    }
                    break;
                case 0x1:
                case 0x2:
                case 0x3: size = 1; break;
                case 0x4: size = 3; break;
                default: size = 0; break;
            }
            break;
        case 0xF:
            if (NIB(w, 6) != 15) break;
            if (NIB(w, 5) == 0 || NIB(w, 5) == 1)
                size = 1;
            else if (NIB(w, 5) == 15)
                size = NIB(w, 2) == 0 || NIB(w, 2) == 2 ? 2 : 1;
            break;
        default: break;
    }
    return size && size <= n - at ? size : 0;
}

/* the instruction opens a conditional block: 0x1, 0x8, 0xC0, 0xC4 */
static int opens(uint32_t w) {
    unsigned t = NIB(w, 7);
    return t == 1 || t == 8 || (t == 0xC && (NIB(w, 6) == 0 || NIB(w, 6) == 4));
}

/* past the block whose condition failed (is_if: stop after a matching Else, to run its other half) or whose first
   half ran (at an Else: on to its End). A block closed more often than opened ends the program */
static void skip(const SnxCheatProgram* p, size_t* ip, unsigned* depth, int is_if) {
    unsigned want;
    if (!*depth) {
        *ip = p->n;
        return;
    }
    want = *depth - 1;
    while (*depth > want && *ip < p->n) {
        uint32_t w = p->op[*ip];
        size_t size = op_size(p->op, *ip, p->n);
        if (!size) {
            *ip = p->n;
            return;
        }
        *ip += size;
        if (opens(w))
            ++*depth;
        else if (NIB(w, 7) == 2) {
            if (!NIB(w, 6))
                --*depth;
            else if (is_if && *depth - 1 == want)
                return;
        }
    }
}

static int compare(unsigned c, uint64_t a, uint64_t b) {
    switch (c) {
        case 1: return a > b;
        case 2: return a >= b;
        case 3: return a < b;
        case 4: return a <= b;
        case 5: return a == b;
        case 6: return a != b;
        default: return 0;
    }
}

static uint64_t load(const SnxCheatEnv* e, uint64_t addr, unsigned width) {
    unsigned char b[8];
    uint64_t v = 0;
    unsigned i;
    if (!width_ok(width) || e->read(e->ctx, addr, b, width)) return 0;
    for (i = width; i--;) v = v << 8 | b[i];
    return v;
}

static void store(const SnxCheatEnv* e, uint64_t addr, unsigned width, uint64_t v) {
    unsigned char b[8];
    unsigned i;
    if (!width_ok(width)) return;
    for (i = 0; i < width; ++i) b[i] = (unsigned char)(v >> (8 * i));
    e->write(e->ctx, addr, b, width);
}

static uint64_t base(const SnxCheatEnv* e, unsigned region) { return region < SNX_CHEAT_REGIONS ? e->base[region] : 0; }

/* lhs op rhs as 0x7 and 0x9 compute it, cut to width */
static uint64_t arith(unsigned op, unsigned width, uint64_t a, uint64_t b) {
    uint64_t r = 0;
    if (op >= 10 && op <= 13 && (width == 4 || width == 8)) {
        if (width == 4) {
            float x, y, z;
            uint32_t ax = (uint32_t)a, by = (uint32_t)b, rz;
            memcpy(&x, &ax, 4);
            memcpy(&y, &by, 4);
            z = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
            memcpy(&rz, &z, 4);
            return rz;
        } else {
            double x, y, z;
            memcpy(&x, &a, 8);
            memcpy(&y, &b, 8);
            z = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
            memcpy(&r, &z, 8);
            return r;
        }
    }
    switch (op) {
        case 0: r = a + b; break;
        case 1: r = a - b; break;
        case 2: r = a * b; break;
        case 3: r = b >= 64 ? 0 : a << b; break;
        case 4: r = b >= 64 ? 0 : a >> b; break;
        case 5: r = a & b; break;
        case 6: r = a | b; break;
        case 7: r = ~a; break;
        case 8: r = a ^ b; break;
        case 9: r = a; break;
        default: r = a; break;
    }
    return cut(width, r);
}

/* 0xC1 and 0xC2's operations on register i: restore, save, clear the saved value, clear the register */
static void save_op(unsigned kind, unsigned dst, unsigned src, uint64_t* reg, uint64_t* saved) {
    switch (kind) {
        case 0: reg[dst] = saved[src]; break;
        case 1: saved[dst] = reg[src]; break;
        case 2: saved[dst] = 0; break;
        case 3: reg[dst] = 0; break;
        default: break;
    }
}

void snx_cheat_run(SnxCheatProgram* p, const SnxCheatEnv* e) {
    uint64_t reg[16], saved[16];
    size_t top[16], ip = 0;
    unsigned long budget = BUDGET;
    unsigned depth = 0;
    memset(reg, 0, sizeof reg);
    memset(saved, 0, sizeof saved);
    memset(top, 0, sizeof top);
    while (ip < p->n && budget--) {
        const uint32_t* w = p->op + ip;
        uint32_t w0 = w[0];
        size_t at = ip, size = op_size(p->op, ip, p->n);
        unsigned t = NIB(w0, 7), width = NIB(w0, 6);
        if (!size) return; /* unknown or cut off: the rest can't be read */
        ip += size;
        switch (t) {
            case 0x0: /* 0TMR00AA AAAAAAAA V: [region + R + A] = V */
                store(e, base(e, NIB(w0, 5)) + reg[NIB(w0, 4)] + addr40(w), width, imm(w + 2, width));
                break;
            case 0x1: { /* 1TMCXrAA AAAAAAAA V: if [region (+ r) + A] C V */
                uint64_t a = base(e, NIB(w0, 5)) + addr40(w) + (NIB(w0, 3) == 1 ? reg[NIB(w0, 2)] : 0);
                ++depth;
                if (!compare(NIB(w0, 4), load(e, a, width), cut(width, imm(w + 2, width)))) skip(p, &ip, &depth, 1);
                break;
            }
            case 0x2: /* 2X000000: End, or Else reached from the first half */
                if (NIB(w0, 6))
                    skip(p, &ip, &depth, 0);
                else if (depth)
                    --depth;
                break;
            case 0x3: { /* 300R0000 V: start a loop of V; 310R0000: its end */
                unsigned r = NIB(w0, 4);
                if (!NIB(w0, 6)) {
                    reg[r] = w[1];
                    top[r] = ip;
                } else if (--reg[r])
                    ip = top[r];
                break;
            }
            case 0x4: reg[NIB(w0, 4)] = wide(w + 1); break; /* 400R0000 VVVVVVVV VVVVVVVV */
            case 0x5: {                                     /* 5TMRIsAA AAAAAAAA: R = [...] */
                unsigned r = NIB(w0, 4), s = NIB(w0, 2), region = NIB(w0, 5);
                uint64_t a = addr40(w);
                switch (NIB(w0, 3)) {
                    case 0: a += base(e, region); break;
                    case 1: a += reg[r]; break;
                    case 2: a += reg[s]; break;
                    case 3: a += base(e, region) + reg[s]; break;
                    default: continue;
                }
                reg[r] = load(e, a, width);
                break;
            }
            case 0x6: { /* 6T0RIor0 V V: [R (+ r)] = V, R += T when I */
                unsigned r = NIB(w0, 4);
                store(e, reg[r] + (NIB(w0, 2) ? reg[NIB(w0, 1)] : 0), width, wide(w + 1));
                if (NIB(w0, 3)) reg[r] += width;
                break;
            }
            case 0x7: /* 7T0RC000 V: R = R op V */
                reg[NIB(w0, 4)] = arith(NIB(w0, 3), width, reg[NIB(w0, 4)], w[1]);
                break;
            case 0x8: { /* 8kkkkkkk: if those buttons are held */
                uint64_t keys = w0 & 0x0fffffffu;
                ++depth;
                if ((e->keys & keys) != keys) skip(p, &ip, &depth, 1);
                break;
            }
            case 0x9: { /* 9TCRS0s0 or 9TCRS100 V: R = S op (s or V) */
                uint64_t rhs = NIB(w0, 2) ? imm(w + 1, width) : reg[NIB(w0, 1)];
                reg[NIB(w0, 4)] = arith(NIB(w0, 5), width, reg[NIB(w0, 3)], rhs);
                break;
            }
            case 0xA: { /* ATSRIOxa (aaaaaaaa): [address] = S, R += T when I */
                unsigned s = NIB(w0, 5), r = NIB(w0, 4), x = NIB(w0, 1);
                uint64_t a;
                switch (NIB(w0, 2)) {
                    case 0: a = reg[r]; break;
                    case 1: a = reg[r] + reg[x]; break;
                    case 2: a = reg[r] + addr36(w); break;
                    case 3: a = base(e, x) + reg[r]; break;
                    case 4: a = base(e, x) + addr36(w); break;
                    case 5: a = base(e, x) + reg[r] + addr36(w); break;
                    default: continue;
                }
                store(e, a, width, reg[s]);
                if (NIB(w0, 3)) reg[r] += width;
                break;
            }
            case 0xC: {
                unsigned sub = NIB(w0, 6);
                if (sub == 0) { /* C0TcSX..: if S c operand */
                    unsigned wd = NIB(w0, 5), m = NIB(w0, 1), r = NIB(w0, 0);
                    uint64_t rhs;
                    switch (NIB(w0, 2)) {
                        case 0: rhs = load(e, base(e, m) + addr36(w), wd); break;
                        case 1: rhs = load(e, base(e, m) + reg[r], wd); break;
                        case 2: rhs = load(e, reg[m] + addr36(w), wd); break;
                        case 3: rhs = load(e, reg[m] + reg[r], wd); break;
                        case 4: rhs = imm(w + 1, wd); break;
                        default: rhs = reg[m]; break;
                    }
                    ++depth;
                    if (!compare(NIB(w0, 4), cut(wd, reg[NIB(w0, 3)]), cut(wd, rhs))) skip(p, &ip, &depth, 1);
                } else if (sub == 1) /* C10D0Sx0 */
                    save_op(NIB(w0, 1), NIB(w0, 4), NIB(w0, 2), reg, saved);
                else if (sub == 2) { /* C2x0XXXX */
                    unsigned i;
                    for (i = 0; i < 16; ++i)
                        if (w0 >> i & 1) save_op(NIB(w0, 5), i, i, reg, saved);
                } else if (sub == 3) { /* C3000XXx: static register XX (from 0x80: written) */
                    unsigned idx = (unsigned)(w0 >> 4) & 0xffu, r = NIB(w0, 0);
                    if (idx < 0x80)
                        reg[r] = p->statics[idx];
                    else
                        p->statics[idx] = reg[r];
                } else if (sub == 4) { /* C4r00000 k k: if those buttons are held; r = 0: once while they stay held */
                    uint64_t keys = wide(w + 1);
                    int held = (e->keys & keys) == keys;
                    ++depth;
                    if (!held) {
                        p->once[at] = 0;
                        skip(p, &ip, &depth, 1);
                    } else if (!NIB(w0, 5)) {
                        if (p->once[at]) skip(p, &ip, &depth, 1);
                        p->once[at] = 1;
                    }
                }
                break;
            }
            case 0xF: /* FFFTIX..: log a value as id I; 0xFF0 pause and 0xFF1 resume: nothing to do here */
                if (NIB(w0, 5) == 15 && e->log) {
                    unsigned wd = NIB(w0, 4), m = NIB(w0, 1), r = NIB(w0, 0);
                    uint64_t v;
                    switch (NIB(w0, 2)) {
                        case 0: v = load(e, base(e, m) + addr36(w), wd); break;
                        case 1: v = load(e, base(e, m) + reg[r], wd); break;
                        case 2: v = load(e, reg[m] + addr36(w), wd); break;
                        case 3: v = load(e, reg[m] + reg[r], wd); break;
                        default: v = cut(wd, reg[m]); break;
                    }
                    e->log(e->ctx, NIB(w0, 3), v);
                }
                break;
            default: break;
        }
    }
}

size_t snx_cheat_fixed(const SnxCheatProgram* p, SnxCheatWrite* out, size_t max) {
    size_t ip = 0, count = 0;
    unsigned depth = 0, loops = 0, set = 0; /* set: the registers some instruction before wrote */
    while (ip < p->n) {
        uint32_t w0 = p->op[ip];
        size_t size = op_size(p->op, ip, p->n);
        unsigned t = NIB(w0, 7), r = NIB(w0, 4);
        if (!size) break;
        if (opens(w0))
            ++depth;
        else if (t == 2 && !NIB(w0, 6) && depth)
            --depth;
        else if (t == 0 && !depth && !loops && !(set >> r & 1) && NIB(w0, 5) == SNX_CHEAT_MAIN &&
                 width_ok(NIB(w0, 6))) {
            if (count < max) {
                out[count].offset = addr40(p->op + ip);
                out[count].width = NIB(w0, 6);
                out[count].value = cut(NIB(w0, 6), imm(p->op + ip + 2, NIB(w0, 6)));
            }
            count++;
        } else if (t == 3) {
            set |= 1u << r;
            if (!NIB(w0, 6))
                loops++;
            else if (loops)
                loops--;
        } else if (t == 4 || t == 5 || t == 7 || t == 9 || ((t == 6 || t == 0xA) && NIB(w0, 3)))
            set |= 1u << r;
        else if (t == 0xC && NIB(w0, 6) == 1)
            set |= 1u << r;
        else if (t == 0xC && NIB(w0, 6) == 2)
            set |= w0 & 0xffffu;
        else if (t == 0xC && NIB(w0, 6) == 3)
            set |= 1u << NIB(w0, 0);
        ip += size;
    }
    return count;
}

/* "<file>: <what> <n>" into error */
static int failf(char* error, size_t error_size, const char* what, unsigned long n) {
    char message[160];
    size_t len;
    if (!error || !error_size) return -1;
    sprintf(message, "%.120s %lu", what, n);
    len = strlen(message);
    if (len >= error_size) len = error_size - 1;
    memcpy(error, message, len);
    error[len] = 0;
    return -1;
}

/* the name from to end is one of enabled's lines (each trimmed of spaces and a CR) */
static int turned_on(const char* name, size_t n, const char* enabled, size_t size) {
    const char *p = enabled, *stop = enabled + size;
    while (enabled && p < stop) {
        const char *line = p, *end = memchr(p, '\n', (size_t)(stop - p));
        if (!end) end = stop;
        p = end + 1;
        while (line < end && (*line == ' ' || *line == '\t')) line++;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - line) == n && !memcmp(line, name, n)) return 1;
    }
    return 0;
}

/* one pass over the file: the words of the codes on into op (when not NULL), their count into *n */
static int scan(const char* text, size_t size, const char* enabled, size_t enabled_size, SnxCheatProgram* p,
                char* error, size_t error_size) {
    const char* stop = text + size;
    unsigned long line_no = 0;
    int named = 0, on = 0;
    p->n = 0;
    p->cheats = p->masters = 0;
    while (text < stop) {
        const char *l = text, *end = memchr(text, '\n', (size_t)(stop - text));
        if (!end) end = stop;
        text = end + 1;
        line_no++;
        while (l < end && (*l == ' ' || *l == '\t' || *l == '\r')) l++;
        while (end > l && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
        if (l == end) continue;
        if (*l == '[' || *l == '{') {
            char close = *l == '[' ? ']' : '}';
            const char* e = end;
            while (e > l + 1 && e[-1] != close) e--;
            if (e == l + 1) return failf(error, error_size, "a name without its closing bracket on line", line_no);
            named = 1;
            on = close == '}' || turned_on(l + 1, (size_t)(e - 1 - (l + 1)), enabled, enabled_size);
            if (on && close == '}') p->masters++;
            if (on && close == ']') p->cheats++;
            continue;
        }
        while (l < end) {
            uint32_t v = 0;
            int i;
            for (i = 0; i < 8; ++i) {
                int h = l + i < end ? hexval((unsigned char)l[i]) : -1;
                if (h < 0) return failf(error, error_size, "a code that isn't 8 hex digits on line", line_no);
                v = v << 4 | (uint32_t)h;
            }
            l += 8;
            if (l < end && *l != ' ' && *l != '\t')
                return failf(error, error_size, "a code that isn't 8 hex digits on line", line_no);
            while (l < end && (*l == ' ' || *l == '\t')) l++;
            if (!named) return failf(error, error_size, "codes before the first cheat's name on line", line_no);
            if (on) {
                if (p->op) p->op[p->n] = v;
                p->n++;
            }
        }
    }
    return 0;
}

int snx_cheat_parse(const char* text, size_t size, const char* enabled, size_t enabled_size, SnxCheatProgram* p,
                    char* error, size_t error_size) {
    memset(p, 0, sizeof *p);
    if (scan(text, size, enabled, enabled_size, p, error, error_size)) return -1;
    p->op = (uint32_t*)malloc(p->n * (sizeof *p->op + 1) + 1);
    if (!p->op) return failf(error, error_size, "out of memory for codes:", (unsigned long)p->n);
    p->once = (unsigned char*)(p->op + p->n);
    memset(p->once, 0, p->n + 1);
    scan(text, size, enabled, enabled_size, p, error, error_size);
    return 0;
}

void snx_cheat_free(SnxCheatProgram* p) {
    free(p->op);
    memset(p, 0, sizeof *p);
}
