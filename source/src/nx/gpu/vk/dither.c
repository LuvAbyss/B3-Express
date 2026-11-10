/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Bayonetta 3's screen-door fade, changed in the translated SPIR-V of a pixel stage.

   The game fades an object by discarding pixels against a 64 x 64 pattern: the stage fetches a texel at
   (int(FragCoord.x) & 63, int(FragCoord.y) & 63) and discards when the object's alpha (vertex alpha times material
   alpha times a fade value) is below it. 429 of the game's 3,755 pixel programs do it
   (diagnostics/dither-20261104/README.md, which has the Python this mirrors). Found by data flow, in program order:
   the translator keeps Maxwell registers in Function variables and reuses them, so a store replaces what a register
   held.

   DITHER_OFF:   the branch to that discard never fires: a fading object is drawn whole.
   DITHER_EVEN:  the pattern is read at the image's own pixels, not the game's (they differ when the scene is drawn
                 above the console's size): an even pattern at any scale, as Witch Time does it (nvn_vk_draw.c
                 g_fs_small, GPL-3.0-or-later).
   DITHER_BLEND: no discard, and the alpha it compared goes out as the second blend source (location 0, index 1),
                 so the pipeline (nvn_res.c pipe_key) blends the object by it instead. */
#include "nx/gpu/vk/dither.h"
#include <stdlib.h>
#include <string.h>

enum {
    OP_EXT_IMPORT = 11,
    OP_EXT_INST = 12,
    OP_MEMORY_MODEL = 14,
    OP_ENTRY_POINT = 15,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_POINTER = 32,
    OP_CONSTANT_FALSE = 42,
    OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44,
    OP_FUNCTION = 54,
    OP_VARIABLE = 59,
    OP_LOAD = 61,
    OP_STORE = 62,
    OP_ACCESS_CHAIN = 65,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_CONSTRUCT = 80,
    OP_EXTRACT = 81,
    OP_IMAGE_FETCH = 95,
    OP_CONVERT_F_TO_U = 109,
    OP_BITCAST = 124,
    OP_FMUL = 133,
    OP_LOGICAL_AND = 167,
    OP_LOGICAL_NOT = 168,
    OP_FORD_LT = 184,
    OP_FUNORD_LT = 185,
    OP_FORD_GT = 186,
    OP_FUNORD_GT = 187,
    OP_BITWISE_AND = 199,
    OP_LABEL = 248,
    OP_BRANCH = 249,
    OP_BRANCH_COND = 250,
    OP_KILL = 252,
    OP_SELECTION_MERGE = 247,
    OP_TERMINATE = 4416,
    OP_DEMOTE = 5380
};
#define MAX_MATCH 16 /* fade compares in one stage: the game's have one or two */
#define MAX_AND   8

typedef struct {
    const uint32_t* w;
    uint32_t bound;
    uint32_t* at; /* word offset of each instruction */
    uint32_t n, body;
} Mod;

static uint32_t op_of(const Mod* m, uint32_t i) { return m->w[m->at[i]] & 0xffffu; }
static uint32_t len_of(const Mod* m, uint32_t i) { return m->w[m->at[i]] >> 16; }
static const uint32_t* arg(const Mod* m, uint32_t i) { return m->w + m->at[i] + 1; }
static int has_result(uint32_t op) {
    switch (op) {
        case OP_STORE:
        case 63:
        case 64:
        case OP_DECORATE:
        case OP_MEMBER_DECORATE:
        case OP_BRANCH:
        case OP_BRANCH_COND:
        case 251:
        case OP_KILL:
        case 253:
        case 254:
        case 255:
        case 246:
        case OP_SELECTION_MERGE:
        case 56:
        case OP_TERMINATE:
        case OP_DEMOTE:
        case 99:
        case 224:
        case 225:
        case OP_LABEL:
        case OP_VARIABLE:
        case OP_FUNCTION: return 0;
        default: return 1;
    }
}
static int id_ok(const Mod* m, uint32_t id) { return id < m->bound; }

/* t holds the seeds, and gets every value derived from them, in program order (a register store replaces) */
static void taint(const Mod* m, uint8_t* t, uint8_t* held, uint32_t* root) {
    uint32_t i, j;
    memset(held, 0, m->bound);
    for (i = 0; i < m->bound; i++) root[i] = i;
    for (i = m->body; i < m->n; i++) {
        uint32_t op = op_of(m, i), na = len_of(m, i) - 1;
        const uint32_t* a = arg(m, i);
        if (op == OP_ACCESS_CHAIN && na >= 3 && id_ok(m, a[1]) && id_ok(m, a[2])) {
            root[a[1]] = root[a[2]];
            if (t[a[2]] || held[root[a[1]]]) t[a[1]] = 1;
        } else if (op == OP_STORE && na >= 2 && id_ok(m, a[0]) && id_ok(m, a[1])) {
            uint32_t var = root[a[0]];
            if (t[a[1]])
                held[var] = 1;
            else if (var == a[0])
                held[var] = 0;
        } else if (op == OP_LOAD && na >= 3 && id_ok(m, a[1]) && id_ok(m, a[2])) {
            if (t[a[2]] || held[root[a[2]]]) t[a[1]] = 1;
        } else if (has_result(op) && na >= 3 && id_ok(m, a[1]) && !t[a[1]]) {
            for (j = 2; j < na; j++)
                if (id_ok(m, a[j]) && t[a[j]]) {
                    t[a[1]] = 1;
                    break;
                }
        }
    }
}

typedef struct {
    uint32_t cmp, fade_slot, fade; /* the compare's instruction, which operand is the alpha, and its id */
    uint32_t branch[8], nbranch;   /* branches to a discard on its result */
} Match;
typedef struct {
    uint32_t ins, slot, comp; /* the And, its operand that came from FragCoord, and which axis */
    uint32_t nid;             /* DITHER_EVEN: the id that replaces that operand */
} AndUse;

/* the inserted words: before or after an instruction, kept in the order added */
typedef struct {
    uint32_t ins, after, off, len;
} Insert;
typedef struct {
    uint32_t *w, n, cap;
    Insert ins[256];
    uint32_t nins;
    uint32_t next_id;
    int bad;
} Patch;
static void put(Patch* p, uint32_t word) {
    if (p->n == p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 256;
        uint32_t* w = realloc(p->w, cap * sizeof *w);
        if (!w) {
            p->bad = 1;
            return;
        }
        p->w = w;
        p->cap = cap;
    }
    p->w[p->n++] = word;
}
static void begin(Patch* p, uint32_t ins, uint32_t after) {
    if (p->nins == sizeof p->ins / sizeof p->ins[0]) {
        p->bad = 1;
        return;
    }
    p->ins[p->nins].ins = ins;
    p->ins[p->nins].after = after;
    p->ins[p->nins].off = p->n;
    p->ins[p->nins].len = 0;
}
static void end(Patch* p) {
    if (p->bad) return;
    p->ins[p->nins].len = p->n - p->ins[p->nins].off;
    p->nins++;
}
static void inst(Patch* p, uint32_t op, const uint32_t* a, uint32_t na) {
    uint32_t i;
    put(p, (na + 1) << 16 | op);
    for (i = 0; i < na; i++) put(p, a[i]);
}
#define INST(p, op, ...)                                                                                               \
    do {                                                                                                               \
        const uint32_t a_[] = {__VA_ARGS__};                                                                           \
        inst(p, op, a_, sizeof a_ / sizeof a_[0]);                                                                     \
    } while (0)

static uint32_t find_type(const Mod* m, uint32_t op, uint32_t x, uint32_t y, int nargs) {
    uint32_t i;
    for (i = 0; i < m->body; i++) {
        const uint32_t* a = arg(m, i);
        if (op_of(m, i) != op || len_of(m, i) - 1 < (uint32_t)nargs + 1) continue;
        if (nargs < 1 || a[1] == x)
            if (nargs < 2 || a[2] == y) return a[0];
    }
    return 0;
}

int dither_patch(uint32_t** spv_io, size_t* words_io, int mode, float fade_test, int* blend) {
    const uint32_t* w = *spv_io;
    size_t words = *words_io;
    Mod m;
    Patch p;
    uint8_t *t = NULL, *ta = NULL, *tf = NULL, *held = NULL, *cur = NULL, *regs = NULL, *whole = NULL, *kill = NULL;
    uint32_t* root = NULL;
    uint32_t i, j, k, fc = 0, last_decor = 0, mem_model = 0, entry_ins = 0, entry_fn = 0, label = 0;
    Match match[MAX_MATCH];
    AndUse ands[MAX_AND];
    uint32_t nmatch = 0, nand = 0, found = 0;
    *blend = 0;
    if (words < 5 || w[0] != 0x07230203u) return 0;
    memset(&m, 0, sizeof m);
    memset(&p, 0, sizeof p);
    m.w = w;
    m.bound = w[3];
    m.at = malloc(words * sizeof *m.at);
    if (!m.at) return 0;
    for (i = 5; i < words;) {
        uint32_t n = w[i] >> 16;
        if (!n || i + n > words) goto done;
        m.at[m.n++] = i;
        i += n;
    }
    m.body = m.n;
    for (i = 0; i < m.n; i++) {
        uint32_t op = op_of(&m, i);
        const uint32_t* a = arg(&m, i);
        if (op == OP_FUNCTION && m.body == m.n) m.body = i;
        if (op == OP_DECORATE && len_of(&m, i) >= 4 && a[1] == 11 && a[2] == 15) fc = a[0];
        if (op == OP_DECORATE || op == OP_MEMBER_DECORATE) last_decor = i;
        if (op == OP_MEMORY_MODEL) mem_model = i;
        if (op == OP_ENTRY_POINT && !entry_ins) entry_ins = i, entry_fn = a[1];
    }
    if (!fc || m.body == m.n || !entry_ins) goto done;
    t = calloc(m.bound, 1), ta = calloc(m.bound, 1), tf = calloc(m.bound, 1), held = malloc(m.bound);
    cur = calloc(m.bound, 1), regs = calloc(m.bound, 1), whole = calloc(m.bound, 1), kill = calloc(m.bound, 1);
    root = malloc(m.bound * sizeof *root);
    if (!t || !ta || !tf || !held || !cur || !regs || !whole || !kill || !root) goto done;
    /* the pattern's coordinate: And(x, 63) with x from FragCoord.x or .y */
    for (i = m.body; i < m.n; i++)
        if (op_of(&m, i) == OP_LOAD && arg(&m, i)[2] == fc && id_ok(&m, arg(&m, i)[1])) whole[arg(&m, i)[1]] = 1;
    for (k = 0; k < 2; k++) {
        memset(t, 0, m.bound);
        for (i = m.body; i < m.n; i++) {
            const uint32_t* a = arg(&m, i);
            if (op_of(&m, i) == OP_EXTRACT && len_of(&m, i) == 5 && id_ok(&m, a[2]) && whole[a[2]] && a[3] == k)
                t[a[1]] = 1;
        }
        taint(&m, t, held, root);
        for (i = m.body; i < m.n; i++) {
            const uint32_t* a = arg(&m, i);
            if (op_of(&m, i) != OP_BITWISE_AND || len_of(&m, i) != 5 || !t[a[1]]) continue;
            for (j = 2; j < 4; j++) {
                uint32_t c = a[j], q;
                for (q = 0; q < m.body; q++) /* a constant 63 */
                    if (op_of(&m, q) == OP_CONSTANT && len_of(&m, q) == 4 && arg(&m, q)[1] == c && arg(&m, q)[2] == 63)
                        break;
                if (q < m.body && nand < MAX_AND && id_ok(&m, a[5 - j]) && t[a[5 - j]]) {
                    ands[nand].ins = i, ands[nand].slot = 5 - j, ands[nand].comp = k;
                    nand++;
                    ta[a[1]] = 1;
                }
            }
        }
    }
    if (!nand) goto done;
    taint(&m, ta, held, root);
    for (i = m.body; i < m.n; i++) { /* the pattern's texel */
        const uint32_t* a = arg(&m, i);
        if (op_of(&m, i) == OP_IMAGE_FETCH && id_ok(&m, a[3]) && ta[a[3]]) tf[a[1]] = 1;
    }
    taint(&m, tf, held, root);
    for (i = m.body; i < m.n; i++) { /* the blocks that discard */
        uint32_t op = op_of(&m, i);
        if (op == OP_LABEL) label = arg(&m, i)[0];
        if ((op == OP_KILL || op == OP_TERMINATE || op == OP_DEMOTE) && id_ok(&m, label)) kill[label] = 1;
    }
    /* compares of the texel with a value not from it, whose result reaches a branch to a discard */
    for (i = m.body; i < m.n; i++) {
        uint32_t op = op_of(&m, i), x, y, q;
        const uint32_t* a = arg(&m, i);
        int fade_first, lt;
        Match mt;
        if ((op < OP_FORD_LT || op > OP_FUNORD_GT) || len_of(&m, i) != 5) continue;
        x = a[2], y = a[3];
        if (!id_ok(&m, x) || !id_ok(&m, y) || tf[x] == tf[y]) continue;
        fade_first = tf[y];
        lt = op == OP_FORD_LT || op == OP_FUNORD_LT;
        memset(&mt, 0, sizeof mt);
        memset(cur, 0, m.bound), memset(regs, 0, m.bound);
        cur[a[1]] = 1;
        for (q = i + 1; q < m.n; q++) {
            uint32_t o = op_of(&m, q), nb = len_of(&m, q) - 1;
            const uint32_t* b = arg(&m, q);
            if (o == OP_STORE && nb >= 2 && id_ok(&m, b[0]) && id_ok(&m, b[1])) {
                if (cur[b[1]])
                    regs[b[0]] = 1;
                else
                    regs[b[0]] = 0;
            } else if (o == OP_LOAD && nb >= 3 && id_ok(&m, b[2]) && regs[b[2]])
                cur[b[1]] = 1;
            else if ((o == OP_LOGICAL_AND || o == OP_BITCAST) && nb >= 3) {
                int in = 0, other_f = 0;
                for (j = 2; j < nb; j++)
                    if (id_ok(&m, b[j])) in |= cur[b[j]], other_f |= !cur[b[j]] && tf[b[j]];
                if (in && other_f) break;
                if (in) cur[b[1]] = 1;
            } else if (o == OP_LOGICAL_NOT) /* FSETP's complement predicate */
                continue;
            else if (o == OP_BRANCH_COND && nb >= 3 && id_ok(&m, b[0]) && cur[b[0]]) {
                if (id_ok(&m, b[1]) && kill[b[1]] && mt.nbranch < 8) mt.branch[mt.nbranch++] = q;
            } else if (has_result(o) && nb >= 3) {
                int in = 0;
                for (j = 2; j < nb; j++) in |= id_ok(&m, b[j]) && cur[b[j]];
                if (in) break; /* used another way: not the shape this knows */
            }
        }
        if (!mt.nbranch) continue;
        found++;
        if (lt != fade_first || nmatch == MAX_MATCH) { /* the other direction, or too many: leave the stage alone */
            nmatch = 0;
            goto done;
        }
        mt.cmp = i, mt.fade_slot = fade_first ? 2 : 3, mt.fade = a[mt.fade_slot];
        match[nmatch++] = mt;
    }
    /* DITHER_BLEND is for fades: an alpha from the vertices and the material's constants that a scene fades over
       time. An alpha from a sampled texture (other than the pattern) is a cut-out the discard shapes: the Kraken's
       goo in chapter 14 is a box carved to its shape by two noise textures and the scene's depth, and blended it
       showed as a flat black slab. Such a stage keeps the game's discard */
    if (nmatch && mode == DITHER_BLEND) {
        memset(t, 0, m.bound);
        for (i = m.body; i < m.n; i++) {
            uint32_t op = op_of(&m, i);
            const uint32_t* a = arg(&m, i);
            if (op >= 87 && op <= 97 && len_of(&m, i) >= 4 && id_ok(&m, a[1]) && !tf[a[1]]) t[a[1]] = 1;
        }
        taint(&m, t, held, root);
        for (k = 0; k < nmatch; k++)
            if (id_ok(&m, match[k].fade) && t[match[k].fade]) {
                nmatch = 0, found = 0;
                goto done;
            }
    }
    if (!nmatch || (mode == DITHER_GAME && fade_test < 0.0f)) {
        if (!nmatch) found = 0;
        goto done;
    }
    {
        uint32_t t_bool = find_type(&m, OP_TYPE_BOOL, 0, 0, 0), t_f32 = find_type(&m, OP_TYPE_FLOAT, 32, 0, 1);
        uint32_t t_u32 = find_type(&m, OP_TYPE_INT, 32, 0, 2),
                 t_v4 = t_f32 ? find_type(&m, OP_TYPE_VECTOR, t_f32, 4, 2) : 0;
        uint32_t c_false = 0, c_fade = 0, c0 = 0, c1 = 0, v1 = 0, outv = 0, glsl = 0, t_out = 0;
        uint32_t first_fn = m.body, init_at = 0;
        if (!t_bool || !t_f32 || !t_v4) goto done;
        p.next_id = m.bound;
        /* constants and the output, before the first function */
        begin(&p, first_fn, 0);
        if (mode == DITHER_OFF || mode == DITHER_BLEND) INST(&p, OP_CONSTANT_FALSE, t_bool, c_false = p.next_id++);
        if (fade_test >= 0.0f) {
            uint32_t bits;
            memcpy(&bits, &fade_test, 4);
            INST(&p, OP_CONSTANT, t_f32, c_fade = p.next_id++, bits);
        }
        if (mode == DITHER_EVEN && !t_u32) { /* a type before anything uses it: at the end of the globals is fine */
            INST(&p, OP_TYPE_INT, t_u32 = p.next_id++, 32, 0);
        }
        if (mode == DITHER_BLEND) {
            for (i = 0; i < m.body; i++) /* GLSL.std.450 */
                if (op_of(&m, i) == OP_EXT_IMPORT && len_of(&m, i) >= 5 && !memcmp(arg(&m, i) + 1, "GLSL.std.450", 12))
                    glsl = arg(&m, i)[0];
            t_out = find_type(&m, OP_TYPE_POINTER, 3, t_v4, 2);
            if (!t_out) INST(&p, OP_TYPE_POINTER, t_out = p.next_id++, 3, t_v4);
            INST(&p, OP_CONSTANT, t_f32, c0 = p.next_id++, 0);
            INST(&p, OP_CONSTANT, t_f32, c1 = p.next_id++, 0x3f800000u);
            INST(&p, OP_CONSTANT_COMPOSITE, t_v4, v1 = p.next_id++, c1, c1, c1, c1);
            INST(&p, OP_VARIABLE, t_out, outv = p.next_id++, 3);
        }
        end(&p);
        if (mode == DITHER_BLEND) {
            if (!glsl) { /* the import goes before the memory model */
                static const char name[16] = "GLSL.std.450";
                uint32_t nw[4];
                memcpy(nw, name, 16);
                begin(&p, mem_model, 0);
                INST(&p, OP_EXT_IMPORT, glsl = p.next_id++, nw[0], nw[1], nw[2], nw[3]);
                end(&p);
            }
            begin(&p, last_decor, 1); /* location 0, index 1 */
            INST(&p, OP_DECORATE, outv, 30, 0);
            INST(&p, OP_DECORATE, outv, 32, 1);
            end(&p);
            /* 1 until a fade says otherwise: after the entry function's first label and its variables */
            for (i = m.body; i < m.n; i++)
                if (op_of(&m, i) == OP_FUNCTION && arg(&m, i)[1] == entry_fn) break;
            for (i++; i < m.n && op_of(&m, i) != OP_LABEL; i++) {}
            for (init_at = i; init_at + 1 < m.n && op_of(&m, init_at + 1) == OP_VARIABLE; init_at++) {}
            if (i >= m.n) goto done;
            begin(&p, init_at, 1);
            INST(&p, OP_STORE, outv, v1);
            end(&p);
        }
        for (k = 0; k < nmatch; k++) {
            uint32_t fade = c_fade ? c_fade : match[k].fade;
            if (mode == DITHER_BLEND) { /* out *= clamp(alpha, 0, 1), after the compare: the alpha is defined there */
                uint32_t cl = p.next_id++, v = p.next_id++, o = p.next_id++, r = p.next_id++;
                begin(&p, match[k].cmp, 1);
                INST(&p, OP_EXT_INST, t_f32, cl, glsl, 43, fade, c0, c1);
                INST(&p, OP_CONSTRUCT, t_v4, v, cl, cl, cl, cl);
                INST(&p, OP_LOAD, t_v4, o, outv);
                INST(&p, OP_FMUL, t_v4, r, o, v);
                INST(&p, OP_STORE, outv, r);
                end(&p);
            }
        }
        if (mode == DITHER_EVEN) /* the pattern at the image's pixels: FragCoord as it is, not in the game's pixels */
            for (k = 0; k < nand; k++) {
                uint32_t l = p.next_id++, e = p.next_id++, ptr = 0, fct = 0;
                for (i = 0; i < m.body; i++) /* FragCoord's pointer type, then what it points to */
                    if (op_of(&m, i) == OP_VARIABLE && arg(&m, i)[1] == fc) ptr = arg(&m, i)[0];
                for (i = 0; i < m.body; i++)
                    if (op_of(&m, i) == OP_TYPE_POINTER && arg(&m, i)[0] == ptr) fct = arg(&m, i)[2];
                if (!fct) goto done;
                begin(&p, ands[k].ins, 0);
                INST(&p, OP_LOAD, fct, l, fc);
                INST(&p, OP_EXTRACT, t_f32, e, l, ands[k].comp);
                INST(&p, OP_CONVERT_F_TO_U, t_u32, ands[k].nid = p.next_id++, e);
                end(&p);
            }
        if (p.bad) goto done;
        /* the module again, with the inserts and the changed words */
        {
            Patch o;
            memset(&o, 0, sizeof o);
            for (i = 0; i < 5; i++) put(&o, i == 3 ? p.next_id : w[i]);
            for (i = 0; i < m.n; i++) {
                uint32_t n = len_of(&m, i), q, s;
                for (q = 0; q < p.nins; q++)
                    if (p.ins[q].ins == i && !p.ins[q].after)
                        for (j = 0; j < p.ins[q].len; j++) put(&o, p.w[p.ins[q].off + j]);
                s = o.n; /* where this instruction starts, after what goes before it */
                for (j = 0; j < n; j++) put(&o, w[m.at[i] + j]);
                if (o.bad) break;
                if (i == entry_ins && outv) { /* the new output in the entry point's interface */
                    put(&o, outv);
                    o.w[s] += 1u << 16;
                }
                for (k = 0; k < nmatch; k++) {
                    if (c_fade && match[k].cmp == i) o.w[s + 1 + match[k].fade_slot] = c_fade;
                    if (c_false)
                        for (q = 0; q < match[k].nbranch; q++)
                            if (match[k].branch[q] == i) o.w[s + 1] = c_false; /* the discard's branch: never */
                }
                if (mode == DITHER_EVEN)
                    for (k = 0; k < nand; k++)
                        if (ands[k].ins == i) o.w[s + 1 + ands[k].slot] = ands[k].nid;
                for (q = 0; q < p.nins; q++)
                    if (p.ins[q].ins == i && p.ins[q].after)
                        for (j = 0; j < p.ins[q].len; j++) put(&o, p.w[p.ins[q].off + j]);
            }
            if (o.bad) {
                free(o.w);
                goto done;
            }
            free(*spv_io);
            *spv_io = o.w;
            *words_io = o.n;
            *blend = mode == DITHER_BLEND;
        }
    }
done:
    free(m.at), free(p.w), free(t), free(ta), free(tf), free(held), free(cur), free(regs), free(whole), free(kill);
    free(root);
    return (int)(nmatch ? found : 0);
}
