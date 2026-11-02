/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated Maxwell decoder: every row's pattern, with its free bits all clear, all set and random, decodes to that
   row, or to a narrower row that the word also matches, and random words decode as a plain scan of the rows says. */
#include "nx/gpu/shader/translator.h"
#include "maxwell_decode.inc"

static int failures;
static u64 state = 0x9E3779B97F4A7C15ull;

static u64 next_random(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

/* the first row in priority order that matches w, the way the rows are meant to be read */
static const Op* scan(u64 w) {
    int i;
    for (i = 0; i < (int)(sizeof shaderdec_ops / sizeof *shaderdec_ops); i++)
        if ((w & shaderdec_ops[i].mask) == shaderdec_ops[i].val) return &shaderdec_ops[i];
    return NULL;
}

/* what decode() should give w: the scan's row, with EXIT.FCSM_TR read as NOP */
static const Op* expected(u64 w) {
    const Op* op = scan(w);
    if (op && op->id == O_EXIT && B(w, 0, 5) == 0x1C) return &shaderdec_ops[shaderdec_row[O_NOP]];
    return op;
}

/* decode() returns decode.c's copy of the rows: the same row of this one */
static const Op* local(const Op* op) {
    int i;
    for (i = 0; op && i < (int)(sizeof shaderdec_ops / sizeof *shaderdec_ops); i++)
        if (shaderdec_ops[i].id == op->id && shaderdec_ops[i].mask == op->mask && shaderdec_ops[i].val == op->val)
            return &shaderdec_ops[i];
    return NULL;
}

static void check_word(const Op* row, u64 w) {
    const Op* got = local(decode(w));
    const Op* want = expected(w);
    /* a narrower row may take the word: it fixes every bit the row does and more */
    int own = got == row || (got && (got->mask & row->mask) == row->mask && got->mask != row->mask) ||
              (row->id == O_EXIT && got && got->id == O_NOP);
    if (got != want || !own || shaderdec_match(w) != scan(w)) {
        fprintf(stderr, "FAIL: %s: %016llx decodes as %s\n", row->name, (unsigned long long)w,
                got ? got->name : "nothing");
        failures++;
    }
}

int main(void) {
    int i, k;
    for (i = 0; i < (int)(sizeof shaderdec_ops / sizeof *shaderdec_ops); i++) {
        const Op* row = &shaderdec_ops[i];
        if (shaderdec_row[row->id] > i || shaderdec_ops[shaderdec_row[row->id]].id != row->id) {
            fprintf(stderr, "FAIL: %s: its opcode's first row is wrong\n", row->name);
            failures++;
        }
        check_word(row, row->val);
        check_word(row, row->val | ~row->mask);
        for (k = 0; k < 64; k++) check_word(row, row->val | (next_random() & ~row->mask));
    }
    for (k = 0; k < 1000000; k++) {
        u64 w = next_random();
        if (local(decode(w)) != expected(w)) {
            fprintf(stderr, "FAIL: random word %016llx\n", (unsigned long long)w);
            failures++;
            break;
        }
    }
    if (decode(0xFFFFFFFFFFFFFFFFull)) {
        fprintf(stderr, "FAIL: an all-ones word decodes\n");
        failures++;
    }
    return failures ? 1 : 0;
}
