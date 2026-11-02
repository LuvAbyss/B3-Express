/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* AdvSIMD loads and stores for snx_simd: LD1-LD4/ST1-ST4 of multiple structures, of a single structure, and
   LD1R-LD4R, with no offset or post-indexed. Lanes are little-endian in guest memory as in the registers. */

static u64 mem_get(u8* M, u64 a, u32 size) {
    switch (size) {
        case 0: return RD8(a);
        case 1: return RD16(a);
        case 2: return RD32(a);
        default: return RD64(a);
    }
}
static void mem_put(u8* M, u64 a, u32 size, u64 v) {
    switch (size) {
        case 0: WR8(a, v); break;
        case 1: WR16(a, v); break;
        case 2: WR32(a, v); break;
        default: WR64(a, v); break;
    }
}

/* the post-index form's writeback: the register's value, or the bytes transferred for Rm = 31 */
static void ldst_writeback(CPU* c, u32 w, u32 rn, u64 bytes) {
    u32 rm = (w >> 16) & 31;
    if (!((w >> 23) & 1)) return;
    c->x[rn] += rm == 31 ? bytes : c->x[rm];
}

static int simd_ldst(CPU* c, u8* M, u32 w) {
    u32 q = (w >> 30) & 1, load = (w >> 22) & 1, rn = (w >> 5) & 31, rt = w & 31;
    u64 base = c->x[rn], offset = 0; /* Rn = 31 is SP */
    u32 i, s, r;
    /* bits 20:16 of the forms without offset, and bit 21 of the multiple-structure ones, are not decoded: the
       emitters take those words as the encodings with them zero, and an unallocated word must be one for both */
    if (!((w >> 24) & 1)) { /* multiple structures */
        u32 size = (w >> 10) & 3, opcode = (w >> 12) & 15, rpt, selem, elements = (q ? 16u : 8u) >> size;
        switch (opcode) {
            case 0x0: rpt = 1, selem = 4; break;
            case 0x2: rpt = 4, selem = 1; break;
            case 0x4: rpt = 1, selem = 3; break;
            case 0x6: rpt = 3, selem = 1; break;
            case 0x7: rpt = 1, selem = 1; break;
            case 0x8: rpt = 1, selem = 2; break;
            case 0xA: rpt = 2, selem = 1; break;
            default: return 0;
        }
        if (size == 3 && !q && selem != 1) return 0;
        if (load && !q)
            for (r = 0; r < rpt * selem; ++r) c->v[(rt + r) & 31].d[1] = 0;
        for (r = 0; r < rpt; ++r)
            for (i = 0; i < elements; ++i)
                for (s = 0; s < selem; ++s) {
                    V128* v = &c->v[(rt + r + s) & 31];
                    if (load)
                        eset(v, size, i, mem_get(M, base + offset, size));
                    else
                        mem_put(M, base + offset, size, eget(v, size, i));
                    offset += (u64)1 << size;
                }
        ldst_writeback(c, w, rn, offset);
        return 1;
    } else { /* single structure */
        u32 R = (w >> 21) & 1, opcode = (w >> 13) & 7, S = (w >> 12) & 1, size = (w >> 10) & 3;
        u32 selem = (((opcode & 1) << 1) | R) + 1, scale = opcode >> 1, index = 0;
        switch (scale) {
            case 3: /* LD1R-LD4R */
                if (!load || S) return 0;
                scale = size;
                for (s = 0; s < selem; ++s) {
                    V128 v;
                    u64 x = mem_get(M, base + offset, scale);
                    vzero(&v);
                    for (i = 0; i < ((q ? 16u : 8u) >> scale); ++i) eset(&v, scale, i, x);
                    c->v[(rt + s) & 31] = v;
                    offset += (u64)1 << scale;
                }
                ldst_writeback(c, w, rn, offset);
                return 1;
            case 0: index = (q << 3) | (S << 2) | size; break;
            case 1:
                if (size & 1) return 0;
                index = (q << 2) | (S << 1) | (size >> 1);
                break;
            default:
                if (size & 2) return 0;
                if (size & 1) {
                    if (S) return 0;
                    scale = 3;
                    index = q;
                } else {
                    index = (q << 1) | S;
                }
                break;
        }
        for (s = 0; s < selem; ++s) {
            V128* v = &c->v[(rt + s) & 31];
            if (load)
                eset(v, scale, index, mem_get(M, base + offset, scale));
            else
                mem_put(M, base + offset, scale, eget(v, scale, index));
            offset += (u64)1 << scale;
        }
        ldst_writeback(c, w, rn, offset);
        return 1;
    }
}
