/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c, vec_helper.c, helper-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro */
/* AdvSIMD data processing for snx_simd (cpu_helpers.c): every ARMv8.0 encoding of the vector and scalar groups,
   lane by lane in plain C. It is the interpreter's SIMD and the IR's fallback for the words its emitters don't
   inline (ir_simd.c), so allocation matters as much as results: an unallocated word returns 0 (UNDEFINED), as the
   emitters decide it. FP lanes go through the raw-bit FP of fp.c with the guest FPCR, and saturation sets FPSR.QC. */

#define FPSR_QC (1u << 27)

/* ---- lanes: size 0-3 is 8 << size bits ---- */
static u64 eget(const V128* v, u32 size, u32 i) {
    switch (size) {
        case 0: return v->b[i];
        case 1: return v->h[i];
        case 2: return v->s[i];
        default: return v->d[i];
    }
}
static void eset(V128* v, u32 size, u32 i, u64 x) {
    switch (size) {
        case 0: v->b[i] = (u8)x; break;
        case 1: v->h[i] = (u16)x; break;
        case 2: v->s[i] = (u32)x; break;
        default: v->d[i] = x; break;
    }
}
static u64 zx(u64 x, u32 size) { return x & bmask(8u << size); }
static i64 sx(u64 x, u32 size) {
    u32 bits = 8u << size;
    return bits >= 64 ? (i64)x : (i64)(x << (64 - bits)) >> (64 - bits);
}
static u64 ones(u32 size) { return bmask(8u << size); }
static void qc(CPU* c) { c->fpsr |= FPSR_QC; }
/* a value saturated to a signed or unsigned lane, QC when it didn't fit */
static u64 ssat(CPU* c, i64 v, u32 size) {
    i64 r = sat_s(v, 8 << size);
    if (r != v) qc(c);
    return zx((u64)r, size);
}
static u64 usat(CPU* c, i64 v, u32 size) {
    u64 r = sat_u(v, 8 << size);
    if (v < 0 || (u64)v != r) qc(c);
    return r;
}
/* the register written: the upper half zeroed for a 64-bit operation */
static void vwrite(CPU* c, u32 d, V128 r, int q) {
    if (!q) r.d[1] = 0;
    c->v[d] = r;
}
static void vzero(V128* r) {
    r->d[0] = 0;
    r->d[1] = 0;
}

/* ---- saturating arithmetic of whole lanes, 64-bit lanes included ---- */
static u64 sqadd_(CPU* c, u64 a, u64 b, u32 size) {
    u64 r;
    if (size < 3) return ssat(c, sx(a, size) + sx(b, size), size);
    r = a + b;
    if (((a ^ r) & (b ^ r)) >> 63) {
        qc(c);
        return (a >> 63) ? SNX_U64C(0x80000000u, 0) : SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu);
    }
    return r;
}
static u64 sqsub_(CPU* c, u64 a, u64 b, u32 size) {
    u64 r;
    if (size < 3) return ssat(c, sx(a, size) - sx(b, size), size);
    r = a - b;
    if (((a ^ b) & (a ^ r)) >> 63) {
        qc(c);
        return (a >> 63) ? SNX_U64C(0x80000000u, 0) : SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu);
    }
    return r;
}
static u64 uqadd_(CPU* c, u64 a, u64 b, u32 size) {
    u64 r;
    if (size < 3) return usat(c, (i64)(zx(a, size) + zx(b, size)), size);
    r = a + b;
    if (r < a) {
        qc(c);
        return ~(u64)0;
    }
    return r;
}
static u64 uqsub_(CPU* c, u64 a, u64 b, u32 size) {
    if (size < 3) return usat(c, (i64)zx(a, size) - (i64)zx(b, size), size);
    if (b > a) {
        qc(c);
        return 0;
    }
    return a - b;
}
/* SUQADD: signed accumulator d plus unsigned n, saturated signed */
static u64 suqadd_(CPU* c, u64 d, u64 n, u32 size) {
    if (size < 3) return ssat(c, sx(d, size) + (i64)zx(n, size), size);
    if (n > SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu) - d) { /* the bound doesn't wrap for any signed d */
        qc(c);
        return SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu);
    }
    return d + n;
}
/* USQADD: unsigned accumulator d plus signed n, saturated unsigned */
static u64 usqadd_(CPU* c, u64 d, u64 n, u32 size) {
    u64 r;
    if (size < 3) return usat(c, (i64)zx(d, size) + sx(n, size), size);
    if (n >> 63) {
        u64 down = (u64)0 - n;
        if (down > d) {
            qc(c);
            return 0;
        }
        return d - down;
    }
    r = d + n;
    if (r < d) {
        qc(c);
        return ~(u64)0;
    }
    return r;
}
/* SQDMULH, SQRDMULH (16 and 32-bit lanes): the high half of 2*a*b (+ rounding); only min*min saturates */
static u64 sqdmulh_(CPU* c, u64 a, u64 b, u32 size, int round) {
    u32 bits = 8u << size;
    i64 x = sx(a, size), y = sx(b, size), p;
    if (x == y && x == -((i64)1 << (bits - 1))) {
        qc(c);
        return ones(size) >> 1;
    }
    p = x * y * 2 + (round ? (i64)1 << (bits - 1) : 0);
    return zx((u64)(p >> bits), size);
}
/* 2*a*b saturated to the double-width lane (SQDMULL and the accumulating forms' product) */
static u64 sqdmull_(CPU* c, u64 a, u64 b, u32 size) {
    i64 x = sx(a, size), y = sx(b, size);
    if (size == 2 && x == y && x == -((i64)1 << 31)) {
        qc(c);
        return SNX_U64C(0x7FFFFFFFu, 0xFFFFFFFFu);
    }
    return ssat(c, x * y * 2, size + 1);
}

/* ---- shifts ---- */
static u64 lsr_(u64 x, u32 s) { return s >= 64 ? 0 : x >> s; }
static i64 asr_(i64 x, u32 s) { return s >= 64 ? (x < 0 ? -1 : 0) : x >> s; }
/* right shifts of a lane by 1..64 or more: the rounding forms add the last bit shifted out, which can't overflow
   where adding the rounding constant first would */
static u64 shr_lane(u64 x, u32 size, u32 s, int uns, int round) {
    if (uns) {
        u64 v = zx(x, size);
        return lsr_(v, s) + (round && s ? lsr_(v, s - 1) & 1 : 0);
    } else {
        i64 v = sx(x, size);
        return (u64)(asr_(v, s) + (round && s ? asr_(v, s - 1) & 1 : 0));
    }
}
/* SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL (register): the shift is the signed bottom byte of the
   second operand, negative for right; left shifts saturate in the saturating forms. A shift of 0 is the value. */
static u64 vshl(CPU* c, u64 x, i32 shift, u32 size, int uns, int round, int sat) {
    u32 bits = 8u << size;
    if (shift < 0) return zx(shr_lane(x, size, (u32)-shift, uns, round), size);
    if (shift == 0) return zx(x, size);
    if (!sat) return (u32)shift >= bits ? 0 : zx(x << shift, size);
    if (uns) {
        u64 v = zx(x, size);
        if (!v) return 0;
        if ((u32)shift >= bits || (v >> (bits - (u32)shift))) {
            qc(c);
            return ones(size);
        }
        return v << shift;
    } else {
        i64 v = sx(x, size), r;
        if (!v) return 0;
        r = (i64)((u64)v << ((u32)shift >= bits ? 0 : shift));
        if ((u32)shift >= bits || (r >> shift) != v) { /* lost bits: the end of the range on v's side */
            qc(c);
            return v < 0 ? (u64)1 << (bits - 1) : ones(size) >> 1;
        }
        return ssat(c, r, size);
    }
}
/* SQSHLU (immediate): a signed lane shifted left, saturated unsigned */
static u64 sqshlu_(CPU* c, u64 x, u32 s, u32 size) {
    u32 bits = 8u << size;
    i64 v = sx(x, size);
    u64 r;
    if (v < 0) {
        qc(c);
        return 0;
    }
    if (!v || !s) return (u64)v;
    if (s >= bits || ((u64)v >> (bits - s))) {
        qc(c);
        return ones(size);
    }
    r = (u64)v << s;
    return r;
}

/* ---- FP lanes ---- */
static int fwidth(u32 sz) { return sz ? 64 : 32; }
static u64 fsign(u32 sz) { return (u64)1 << (sz ? 63 : 31); }
/* FCMEQ (quiet), FCMGE, FCMGT (signalling), FACGE, FACGT (magnitudes): all ones or zero */
enum { FCMP_EQ, FCMP_GE, FCMP_GT, FCMP_LE, FCMP_LT };
static u64 fcmpmask(CPU* c, u64 a, u64 b, u32 sz, int kind, int absolute) {
    u32 f;
    int hit;
    if (absolute) {
        a &= ~fsign(sz);
        b &= ~fsign(sz);
    }
    f = snx_fp_compare(c, a, b, fwidth(sz), kind != FCMP_EQ);
    switch (kind) {
        case FCMP_EQ: hit = f == 6; break;
        case FCMP_GE: hit = f == 6 || f == 2; break;
        case FCMP_GT: hit = f == 2; break;
        case FCMP_LE: hit = f == 6 || f == 8; break;
        default: hit = f == 8; break;
    }
    return hit ? ones(sz ? 3 : 2) : 0;
}

/* ---- three same ---- */

/* one integer lane of the three-same group (op = opcode 15:11); d is the destination lane for the accumulating ops */
static u64 same_int(CPU* c, u32 op, u32 u, u32 size, u64 a, u64 b, u64 d) {
    i64 x = sx(a, size), y = sx(b, size);
    u64 ua = zx(a, size), ub = zx(b, size), r;
    switch (op) {
        case 0x00: return zx(u ? (ua + ub) >> 1 : (u64)((x + y) >> 1), size);
        case 0x01: return u ? uqadd_(c, a, b, size) : sqadd_(c, a, b, size);
        case 0x02: return zx(u ? (ua + ub + 1) >> 1 : (u64)((x + y + 1) >> 1), size);
        case 0x04: return zx(u ? (u64)(((i64)ua - (i64)ub) >> 1) : (u64)((x - y) >> 1), size);
        case 0x05: return u ? uqsub_(c, a, b, size) : sqsub_(c, a, b, size);
        case 0x06: return (u ? ua > ub : x > y) ? ones(size) : 0;
        case 0x07: return (u ? ua >= ub : x >= y) ? ones(size) : 0;
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: return vshl(c, a, (i8)(u8)b, size, (int)u, (op & 2) != 0, (op & 1) != 0);
        case 0x0C: return u ? (ua > ub ? ua : ub) : zx((u64)(x > y ? x : y), size);
        case 0x0D: return u ? (ua < ub ? ua : ub) : zx((u64)(x < y ? x : y), size);
        case 0x0E:
        case 0x0F:
            r = u ? (ua > ub ? ua - ub : ub - ua) : (u64)(x > y ? x - y : y - x);
            return zx(op == 0x0F ? d + r : r, size);
        case 0x10: return zx(u ? a - b : a + b, size);
        case 0x11: return (u ? ua == ub : (a & b & ones(size)) != 0) ? ones(size) : 0;
        case 0x12: return zx(u ? d - a * b : d + a * b, size);
        case 0x13:
            if (u) { /* PMUL: carry-less product of bytes, low 8 bits */
                u32 i, p = 0;
                for (i = 0; i < 8; ++i)
                    if ((ub >> i) & 1) p ^= (u32)ua << i;
                return p & 0xFF;
            }
            return zx(a * b, size);
        case 0x14: return u ? (ua > ub ? ua : ub) : zx((u64)(x > y ? x : y), size);
        case 0x15: return u ? (ua < ub ? ua : ub) : zx((u64)(x < y ? x : y), size);
        case 0x16: return sqdmulh_(c, a, b, size, (int)u);
        default: return zx(a + b, size); /* 0x17 ADDP */
    }
}

/* one FP lane of the three-same group: key = U:a:opcode<2:0> */
static u64 same_fp(CPU* c, u32 key, u32 sz, u64 a, u64 b, u64 d) {
    int w = fwidth(sz);
    switch (key) {
        case 0x00: return snx_fp_minmax(c, a, b, w, 0, 1);
        case 0x01: return snx_fp_fma(c, d, a, b, w);
        case 0x02: return snx_fp_binary(c, a, b, w, SNX_FP_ADD);
        case 0x03: return snx_fp_binary(c, a, b, w, SNX_FP_MULX);
        case 0x04: return fcmpmask(c, a, b, sz, FCMP_EQ, 0);
        case 0x06: return snx_fp_minmax(c, a, b, w, 0, 0);
        case 0x07: return snx_fp_binary(c, a, b, w, SNX_FP_RECPS);
        case 0x08: return snx_fp_minmax(c, a, b, w, 1, 1);
        case 0x09: return snx_fp_fma(c, d, a ^ fsign(sz), b, w); /* FMLS negates the multiplicand first */
        case 0x0A: return snx_fp_binary(c, a, b, w, SNX_FP_SUB);
        case 0x0E: return snx_fp_minmax(c, a, b, w, 1, 0);
        case 0x0F: return snx_fp_binary(c, a, b, w, SNX_FP_RSQRTS);
        case 0x10: return snx_fp_minmax(c, a, b, w, 0, 1); /* FMAXNMP */
        case 0x12: return snx_fp_binary(c, a, b, w, SNX_FP_ADD); /* FADDP */
        case 0x13: return snx_fp_binary(c, a, b, w, SNX_FP_MUL);
        case 0x14: return fcmpmask(c, a, b, sz, FCMP_GE, 0);
        case 0x15: return fcmpmask(c, a, b, sz, FCMP_GE, 1);
        case 0x16: return snx_fp_minmax(c, a, b, w, 0, 0); /* FMAXP */
        case 0x17: return snx_fp_binary(c, a, b, w, SNX_FP_DIV);
        case 0x18: return snx_fp_minmax(c, a, b, w, 1, 1); /* FMINNMP */
        case 0x1A: return snx_fp_binary(c, a, b, w, SNX_FP_ABD);
        case 0x1C: return fcmpmask(c, a, b, sz, FCMP_GT, 0);
        case 0x1D: return fcmpmask(c, a, b, sz, FCMP_GT, 1);
        default: return snx_fp_minmax(c, a, b, w, 1, 0); /* 0x1E FMINP */
    }
}

/* AdvSIMD three same and AdvSIMD scalar three same (scalar: bit 28) */
static int simd_3same(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, u = (w >> 29) & 1, scalar = (w >> 28) & 1, size = (w >> 22) & 3, op = (w >> 11) & 31;
    u32 rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, i, lanes;
    V128 n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    vzero(&r);
    if (op >= 0x18) { /* floating point */
        u32 sz = size & 1, key = (u << 4) | ((size >> 1) << 3) | (op & 7);
        static const u8 vector_ok[32] = {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,
                                         1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0};
        static const u8 scalar_ok[32] = {0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1,
                                         0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0};
        int pairwise = key == 0x10 || key == 0x12 || key == 0x16 || key == 0x18 || key == 0x1E;
        if (scalar ? !scalar_ok[key] : !vector_ok[key] || (sz && !q)) return 0;
        if (scalar) {
            u64 a = sz ? n.d[0] : n.s[0], b = sz ? m.d[0] : m.s[0];
            eset(&r, 2 + sz, 0, same_fp(c, key, sz, a, b, 0));
            vwrite(c, rd, r, 0);
            return 1;
        }
        lanes = (q ? 4 : 2) >> sz;
        for (i = 0; i < lanes; ++i) {
            u64 a, b;
            if (pairwise) { /* pairs of the concatenation n:m */
                const V128* src = i < lanes / 2 ? &n : &m;
                u32 k = (i % (lanes / 2)) * 2;
                a = eget(src, 2 + sz, k);
                b = eget(src, 2 + sz, k + 1);
            } else {
                a = eget(&n, 2 + sz, i);
                b = eget(&m, 2 + sz, i);
            }
            eset(&r, 2 + sz, i, same_fp(c, key, sz, a, b, eget(&d, 2 + sz, i)));
        }
        vwrite(c, rd, r, (int)q);
        return 1;
    }
    if (scalar) {
        switch (op) {
            case 0x01:
            case 0x05:
            case 0x09:
            case 0x0B: break;
            case 0x06:
            case 0x07:
            case 0x08:
            case 0x0A:
            case 0x10:
            case 0x11:
                if (size != 3) return 0;
                break;
            case 0x16:
                if (size != 1 && size != 2) return 0;
                break;
            default: return 0;
        }
        eset(&r, size, 0, same_int(c, op, u, size, eget(&n, size, 0), eget(&m, size, 0), 0));
        vwrite(c, rd, r, 0);
        return 1;
    }
    if (op == 0x03) { /* logical: AND BIC ORR ORN / EOR BSL BIT BIF, on whole registers */
        for (i = 0; i < 2; ++i) {
            u64 a = n.d[i], b = m.d[i], x = d.d[i];
            switch ((u << 2) | size) {
                case 0: r.d[i] = a & b; break;
                case 1: r.d[i] = a & ~b; break;
                case 2: r.d[i] = a | b; break;
                case 3: r.d[i] = a | ~b; break;
                case 4: r.d[i] = a ^ b; break;
                case 5: r.d[i] = (x & a) | (~x & b); break; /* BSL */
                case 6: r.d[i] = (x & ~b) | (a & b); break; /* BIT */
                default: r.d[i] = (x & b) | (a & ~b); break; /* BIF */
            }
        }
        vwrite(c, rd, r, (int)q);
        return 1;
    }
    switch (op) {
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x12:
        case 0x14:
        case 0x15:
            if (size == 3) return 0;
            break;
        case 0x13:
            if (size == 3 || (u && size != 0)) return 0;
            break;
        case 0x16:
            if (size != 1 && size != 2) return 0;
            break;
        case 0x17:
            if (u || (size == 3 && !q)) return 0;
            break;
        default: /* 0x01 0x05-0x0B 0x10 0x11 */
            if (size == 3 && !q) return 0;
            break;
    }
    lanes = (q ? 16 : 8) >> size;
    for (i = 0; i < lanes; ++i) {
        u64 a, b;
        if (op >= 0x14 && op != 0x16) { /* SMAXP UMAXP SMINP UMINP ADDP: pairs of n:m */
            const V128* src = i < lanes / 2 ? &n : &m;
            u32 k = (i % (lanes / 2)) * 2;
            a = eget(src, size, k);
            b = eget(src, size, k + 1);
        } else {
            a = eget(&n, size, i);
            b = eget(&m, size, i);
        }
        eset(&r, size, i, same_int(c, op, u, size, a, b, eget(&d, size, i)));
    }
    vwrite(c, rd, r, (int)q);
    return 1;
}

/* ---- three different (long, wide and narrow) ---- */
static int simd_3diff(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, u = (w >> 29) & 1, scalar = (w >> 28) & 1, size = (w >> 22) & 3, op = (w >> 12) & 15;
    u32 rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, i, lanes, half;
    V128 n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    vzero(&r);
    if (scalar) { /* SQDMLAL, SQDMLSL, SQDMULL */
        u64 p;
        if (u || (size != 1 && size != 2) || (op != 9 && op != 11 && op != 13)) return 0;
        p = sqdmull_(c, eget(&n, size, 0), eget(&m, size, 0), size);
        if (op == 9) p = sqadd_(c, eget(&d, size + 1, 0), p, size + 1);
        if (op == 11) p = sqsub_(c, eget(&d, size + 1, 0), p, size + 1);
        eset(&r, size + 1, 0, p);
        vwrite(c, rd, r, 0);
        return 1;
    }
    if (op == 15) return 0;
    if (op == 14) { /* PMULL, PMULL2 */
        if (u || size == 1 || size == 2) return 0;
        if (size == 3) {
            v_pmull(&r, &n, &m, (int)q);
        } else {
            for (i = 0; i < 8; ++i) {
                u32 a = n.b[i + 8 * q], b = m.b[i + 8 * q], k, p = 0;
                for (k = 0; k < 8; ++k)
                    if ((b >> k) & 1) p ^= a << k;
                r.h[i] = (u16)p;
            }
        }
        vwrite(c, rd, r, 1);
        return 1;
    }
    if (size == 3) return 0;
    if ((op == 9 || op == 11 || op == 13) && (u || size == 0)) return 0;
    half = q * (8u >> size); /* the "2" forms take the upper halves of the narrow operands */
    if (op == 4 || op == 6) { /* ADDHN SUBHN (RADDHN RSUBHN): the high halves, into the low or high half of Vd */
        lanes = 8u >> size;
        if (q) r.d[0] = d.d[0];
        for (i = 0; i < lanes; ++i) {
            u64 a = eget(&n, size + 1, i), b = eget(&m, size + 1, i), s = op == 4 ? a + b : a - b;
            u32 bits = 8u << size;
            if (u) s += (u64)1 << (bits - 1);
            eset(&r, size, i + half, zx(s >> bits, size));
        }
        vwrite(c, rd, r, 1);
        return 1;
    }
    lanes = 8u >> size;
    for (i = 0; i < lanes; ++i) {
        u64 b = eget(&m, size, i + half), acc = eget(&d, size + 1, i), a, res;
        i64 x, y;
        if (op == 1 || op == 3) /* the wide forms: n is double width */
            a = eget(&n, size + 1, i);
        else
            a = eget(&n, size, i + half);
        x = (op == 1 || op == 3) ? sx(a, size + 1) : sx(a, size);
        y = sx(b, size);
        if (u) {
            x = (op == 1 || op == 3) ? (i64)zx(a, size + 1) : (i64)zx(a, size);
            y = (i64)zx(b, size);
        }
        switch (op) {
            case 0:
            case 1: res = (u64)(x + y); break;
            case 2:
            case 3: res = (u64)(x - y); break;
            case 5: res = acc + (u64)(x > y ? x - y : y - x); break;
            case 7: res = (u64)(x > y ? x - y : y - x); break;
            case 8: res = acc + (u64)(x * y); break;
            case 10: res = acc - (u64)(x * y); break;
            case 12: res = (u64)(x * y); break;
            case 9: res = sqadd_(c, acc, sqdmull_(c, a, b, size), size + 1); break;
            case 11: res = sqsub_(c, acc, sqdmull_(c, a, b, size), size + 1); break;
            default: res = sqdmull_(c, a, b, size); break; /* 13 SQDMULL */
        }
        eset(&r, size + 1, i, zx(res, size + 1));
    }
    vwrite(c, rd, r, 1);
    return 1;
}

/* ---- two-register misc ---- */

/* SQXTN, UQXTN, SQXTUN, XTN of a double-width lane x to size */
static u64 narrow_(CPU* c, u64 x, u32 size, int kind) {
    switch (kind) {
        case 0: return zx(x, size);                     /* XTN */
        case 1: return ssat(c, sx(x, size + 1), size);  /* SQXTN */
        case 2:                                         /* UQXTN */
            if (size + 1 == 3 && (x >> 63)) {
                qc(c);
                return ones(size);
            }
            return usat(c, (i64)zx(x, size + 1), size);
        default: return usat(c, sx(x, size + 1), size); /* SQXTUN */
    }
}

/* FRINT*, FCVT*, SCVTF/UCVTF, FRECPE, FRSQRTE, URECPE, URSQRTE, FSQRT, FRECPX, the compares against zero, FABS,
   FNEG: one lane; key = U:a:opcode */
static u64 misc_fp(CPU* c, u32 key, u32 sz, u64 x) {
    int wd = fwidth(sz), rm = FPRM;
    switch (key) {
        case 0x018: return snx_fp_round(c, x, wd, 0, 0);
        case 0x019: return snx_fp_round(c, x, wd, 2, 0);
        case 0x01A: return snx_fp_to_int(c, x, wd, wd, 0, 0, 0);
        case 0x01B: return snx_fp_to_int(c, x, wd, wd, 0, 0, 2);
        case 0x01C: return snx_fp_to_int(c, x, wd, wd, 0, 0, 4);
        case 0x01D: return snx_fp_from_int(c, x, wd, wd, 0, 0);
        case 0x02C: return fcmpmask(c, x, 0, sz, FCMP_GT, 0);
        case 0x02D: return fcmpmask(c, x, 0, sz, FCMP_EQ, 0);
        case 0x02E: return fcmpmask(c, x, 0, sz, FCMP_LT, 0);
        case 0x02F: return x & ~fsign(sz);
        case 0x038: return snx_fp_round(c, x, wd, 1, 0);
        case 0x039: return snx_fp_round(c, x, wd, 3, 0);
        case 0x03A: return snx_fp_to_int(c, x, wd, wd, 0, 0, 1);
        case 0x03B: return snx_fp_to_int(c, x, wd, wd, 0, 0, 3);
        case 0x03C: return urecpe32((u32)x);
        case 0x03D: return snx_fp_unary(c, x, wd, SNX_FP_RECPE);
        case 0x03F: return snx_fp_unary(c, x, wd, SNX_FP_RECPX);
        case 0x058: return snx_fp_round(c, x, wd, 4, 0);
        case 0x059: return snx_fp_round(c, x, wd, rm, 1);
        case 0x05A: return snx_fp_to_int(c, x, wd, wd, 1, 0, 0);
        case 0x05B: return snx_fp_to_int(c, x, wd, wd, 1, 0, 2);
        case 0x05C: return snx_fp_to_int(c, x, wd, wd, 1, 0, 4);
        case 0x05D: return snx_fp_from_int(c, x, wd, wd, 1, 0);
        case 0x06C: return fcmpmask(c, x, 0, sz, FCMP_GE, 0);
        case 0x06D: return fcmpmask(c, x, 0, sz, FCMP_LE, 0);
        case 0x06F: return x ^ fsign(sz);
        case 0x079: return snx_fp_round(c, x, wd, rm, 0);
        case 0x07A: return snx_fp_to_int(c, x, wd, wd, 1, 0, 1);
        case 0x07B: return snx_fp_to_int(c, x, wd, wd, 1, 0, 3);
        case 0x07C: return ursqrte32((u32)x);
        case 0x07D: return snx_fp_unary(c, x, wd, SNX_FP_RSQRTE);
        default: return snx_fp_unary(c, x, wd, SNX_FP_SQRT); /* 0x07F */
    }
}
/* whether an FP misc key is allocated in the vector (scalar 0) or the scalar group */
static int misc_fp_allocated(u32 key, u32 sz, int scalar) {
    switch (key) {
        case 0x01A:
        case 0x01B:
        case 0x01C:
        case 0x01D:
        case 0x02C:
        case 0x02D:
        case 0x02E:
        case 0x03A:
        case 0x03B:
        case 0x03D:
        case 0x05A:
        case 0x05B:
        case 0x05C:
        case 0x05D:
        case 0x06C:
        case 0x06D:
        case 0x07A:
        case 0x07B:
        case 0x07D: return 1;
        case 0x018:
        case 0x019:
        case 0x02F:
        case 0x038:
        case 0x039:
        case 0x058:
        case 0x059:
        case 0x06F:
        case 0x079:
        case 0x07F: return !scalar;
        case 0x03C:
        case 0x07C: return !scalar && !sz;
        case 0x03F: return scalar;
        default: return 0;
    }
}

/* one integer lane of two-register misc (not the narrowing, widening or pairwise ops) */
static u64 misc_int(CPU* c, u32 op, u32 u, u32 size, u64 x, u64 d) {
    i64 v = sx(x, size);
    u64 min = (u64)1 << ((8u << size) - 1);
    switch (op) {
        case 0x03: return u ? usqadd_(c, d, x, size) : suqadd_(c, d, x, size);
        case 0x04:
            if (u) return clz64(zx(x, size)) - (64 - (8u << size));
            return clz64(zx(x ^ (u64)(v >> 1), size)) - (64 - (8u << size)) - 1;
        case 0x05:
            if (!u) return popc8((u8)x);
            return size ? (u64)(rbit32((u32)x) >> 24) : ~x & 0xFF; /* RBIT, NOT */
        case 0x07:
            if (zx(x, size) == min) { /* SQABS, SQNEG of the minimum */
                qc(c);
                return min - 1;
            }
            return zx(u || v < 0 ? (u64)0 - x : x, size);
        case 0x08: return (u ? v >= 0 : v > 0) ? ones(size) : 0;
        case 0x09: return (u ? v <= 0 : v == 0) ? ones(size) : 0;
        case 0x0A: return v < 0 ? ones(size) : 0;
        default: return zx(u || v < 0 ? (u64)0 - x : x, size); /* 0x0B ABS, NEG */
    }
}

/* AdvSIMD two-register misc and AdvSIMD scalar two-register misc */
static int simd_2misc(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, u = (w >> 29) & 1, scalar = (w >> 28) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31;
    u32 rn = (w >> 5) & 31, rd = w & 31, i, lanes, sz = size & 1, key = (u << 6) | ((size >> 1) << 5) | op;
    V128 n = c->v[rn], d = c->v[rd], r;
    vzero(&r);
    if (op >= 0x0C && op != 0x12 && op != 0x13 && op != 0x14 && op != 0x16 && op != 0x17) { /* FP */
        if (!misc_fp_allocated(key, sz, (int)scalar)) return 0;
        if (scalar) {
            eset(&r, 2 + sz, 0, misc_fp(c, key, sz, eget(&n, 2 + sz, 0)));
            vwrite(c, rd, r, 0);
            return 1;
        }
        if (sz && !q) return 0;
        lanes = (q ? 4 : 2) >> sz;
        for (i = 0; i < lanes; ++i) eset(&r, 2 + sz, i, misc_fp(c, key, sz, eget(&n, 2 + sz, i)));
        vwrite(c, rd, r, (int)q);
        return 1;
    }
    if (op == 0x16 || op == 0x17) { /* FCVTN, FCVTXN, FCVTL (a = 0) */
        if (size >> 1) return 0;
        if (op == 0x17) {
            if (u || scalar) return 0;
            for (i = 0; i < (sz ? 2u : 4u); ++i) {
                u64 x = sz ? n.s[i + 2 * q] : n.h[i + 4 * q];
                eset(&r, 2 + sz, i, snx_fp_convert(c, x, sz ? 32 : 16, sz ? 64 : 32, FPRM));
            }
            vwrite(c, rd, r, 1);
            return 1;
        }
        if (u && !sz) return 0;
        if (scalar) {
            if (!u) return 0;
            r.s[0] = (u32)snx_fp_convert(c, n.d[0], 64, 32, 5);
            vwrite(c, rd, r, 0);
            return 1;
        }
        if (q) r.d[0] = d.d[0];
        for (i = 0; i < (sz ? 2u : 4u); ++i) {
            if (sz)
                r.s[i + 2 * q] = (u32)snx_fp_convert(c, n.d[i], 64, 32, u ? 5 : FPRM);
            else
                r.h[i + 4 * q] = (u16)snx_fp_convert(c, n.s[i], 32, 16, FPRM);
        }
        vwrite(c, rd, r, 1);
        return 1;
    }
    if (op == 0x12 || op == 0x14) { /* XTN, SQXTUN (0x12), SQXTN, UQXTN (0x14) */
        int kind = op == 0x12 ? (u ? 3 : 0) : (u ? 2 : 1);
        if (size == 3 || (scalar && kind == 0)) return 0;
        if (scalar) {
            eset(&r, size, 0, narrow_(c, eget(&n, size + 1, 0), size, kind));
            vwrite(c, rd, r, 0);
            return 1;
        }
        lanes = 8u >> size;
        if (q) r.d[0] = d.d[0];
        for (i = 0; i < lanes; ++i) eset(&r, size, i + q * lanes, narrow_(c, eget(&n, size + 1, i), size, kind));
        vwrite(c, rd, r, 1);
        return 1;
    }
    if (scalar) {
        switch (op) {
            case 0x03:
            case 0x07: break;
            case 0x08:
            case 0x09:
            case 0x0B:
                if (size != 3) return 0;
                break;
            case 0x0A:
                if (u || size != 3) return 0;
                break;
            default: return 0;
        }
        eset(&r, size, 0, misc_int(c, op, u, size, eget(&n, size, 0), eget(&d, size, 0)));
        vwrite(c, rd, r, 0);
        return 1;
    }
    switch (op) {
        case 0x00: /* REV64, REV32 */
            if (size + u >= 3) return 0;
            lanes = (q ? 16 : 8) >> size;
            for (i = 0; i < lanes; ++i) eset(&r, size, i, eget(&n, size, i ^ (((u ? 4u : 8u) >> size) - 1)));
            break;
        case 0x01: /* REV16 */
            if (u || size) return 0;
            for (i = 0; i < 16; ++i) r.b[i] = n.b[i ^ 1];
            break;
        case 0x02:
        case 0x06: /* SADDLP UADDLP SADALP UADALP */
            if (size == 3) return 0;
            lanes = (q ? 8 : 4) >> size;
            for (i = 0; i < lanes; ++i) {
                u64 a = eget(&n, size, 2 * i), b = eget(&n, size, 2 * i + 1);
                u64 s = u ? zx(a, size) + zx(b, size) : (u64)(sx(a, size) + sx(b, size));
                if (op == 0x06) s += eget(&d, size + 1, i);
                eset(&r, size + 1, i, zx(s, size + 1));
            }
            break;
        case 0x13: /* SHLL, SHLL2 */
            if (!u || size == 3) return 0;
            lanes = 8u >> size;
            for (i = 0; i < lanes; ++i) eset(&r, size + 1, i, eget(&n, size, i + q * lanes) << (8u << size));
            vwrite(c, rd, r, 1);
            return 1;
        case 0x04: /* CLS CLZ */
            if (size == 3) return 0;
            goto lanewise;
        case 0x05: /* CNT; NOT, RBIT */
            if (u ? size > 1 : size != 0) return 0;
            for (i = 0; i < 16; ++i) r.b[i] = (u8)misc_int(c, op, u, size, n.b[i], 0);
            break;
        case 0x0A:
            if (u) return 0;
            /* fall through */
        case 0x03:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0B:
            if (size == 3 && !q) return 0;
            goto lanewise;
        default: return 0;
    }
    vwrite(c, rd, r, (int)q);
    return 1;
lanewise:
    lanes = (q ? 16 : 8) >> size;
    for (i = 0; i < lanes; ++i) eset(&r, size, i, misc_int(c, op, u, size, eget(&n, size, i), eget(&d, size, i)));
    vwrite(c, rd, r, (int)q);
    return 1;
}

/* ---- across lanes and scalar pairwise ---- */
static u64 fp_reduce(CPU* c, const V128* n, u32 key) {
    int minimum = (key & 8) != 0, numeric = (key & 7) == 4;
    u64 lo = snx_fp_minmax(c, n->s[0], n->s[1], 32, minimum, numeric);
    u64 hi = snx_fp_minmax(c, n->s[2], n->s[3], 32, minimum, numeric);
    return snx_fp_minmax(c, lo, hi, 32, minimum, numeric); /* the architecture's tree, not a running reduction */
}

static int simd_across(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31;
    u32 rn = (w >> 5) & 31, rd = w & 31, i, lanes;
    V128 n = c->v[rn], r;
    vzero(&r);
    if (op == 0x0C || op == 0x0F) { /* FMAXNMV FMINNMV FMAXV FMINV */
        if (!u || (size & 1) || !q) return 0;
        r.s[0] = (u32)fp_reduce(c, &n, ((size >> 1) << 3) | (op & 7));
        vwrite(c, rd, r, 0);
        return 1;
    }
    if (op != 0x03 && op != 0x0A && op != 0x1A && !(op == 0x1B && !u)) return 0;
    if (size == 3 || (size == 2 && !q)) return 0;
    lanes = (q ? 16 : 8) >> size;
    if (op == 0x03) { /* SADDLV UADDLV */
        u64 s = 0;
        for (i = 0; i < lanes; ++i) s += u ? zx(eget(&n, size, i), size) : (u64)sx(eget(&n, size, i), size);
        eset(&r, size + 1, 0, zx(s, size + 1));
    } else if (op == 0x1B) { /* ADDV */
        u64 s = 0;
        for (i = 0; i < lanes; ++i) s += eget(&n, size, i);
        eset(&r, size, 0, zx(s, size));
    } else { /* SMAXV UMAXV SMINV UMINV */
        u64 best = eget(&n, size, 0);
        for (i = 1; i < lanes; ++i) {
            u64 x = eget(&n, size, i);
            int greater = u ? x > best : sx(x, size) > sx(best, size);
            if (op == 0x0A ? greater : (x != best && !greater)) best = x;
        }
        eset(&r, size, 0, best);
    }
    vwrite(c, rd, r, 0);
    return 1;
}

static int simd_scalar_pairwise(CPU* c, u32 w) {
    u32 u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31, sz = size & 1;
    V128 n = c->v[rn], r;
    u64 a, b;
    vzero(&r);
    if (!u) {
        if (op != 0x1B || size != 3) return 0; /* ADDP */
        r.d[0] = n.d[0] + n.d[1];
        vwrite(c, rd, r, 0);
        return 1;
    }
    a = sz ? n.d[0] : n.s[0];
    b = sz ? n.d[1] : n.s[1];
    switch (((size >> 1) << 5) | op) {
        case 0x0C: a = snx_fp_minmax(c, a, b, fwidth(sz), 0, 1); break;
        case 0x2C: a = snx_fp_minmax(c, a, b, fwidth(sz), 1, 1); break;
        case 0x0D: a = snx_fp_binary(c, a, b, fwidth(sz), SNX_FP_ADD); break;
        case 0x0F: a = snx_fp_minmax(c, a, b, fwidth(sz), 0, 0); break;
        case 0x2F: a = snx_fp_minmax(c, a, b, fwidth(sz), 1, 0); break;
        default: return 0;
    }
    eset(&r, 2 + sz, 0, a);
    vwrite(c, rd, r, 0);
    return 1;
}

/* ---- copy: DUP, INS, SMOV, UMOV, and scalar DUP ---- */
static int simd_copy(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, op = (w >> 29) & 1, scalar = (w >> 28) & 1, imm5 = (w >> 16) & 31, imm4 = (w >> 11) & 15;
    u32 rn = (w >> 5) & 31, rd = w & 31, size, index, i;
    V128 n = c->v[rn], r;
    if (!(imm5 & 15)) return 0;
    for (size = 0; !((imm5 >> size) & 1); ++size) {
    }
    index = imm5 >> (size + 1);
    vzero(&r);
    if (scalar) { /* DUP (element), scalar */
        if (op || imm4) return 0;
        eset(&r, size, 0, eget(&n, size, index));
        vwrite(c, rd, r, 0);
        return 1;
    }
    if (op) { /* INS (element) */
        if (!q) return 0;
        r = c->v[rd];
        eset(&r, size, index, eget(&n, size, imm4 >> size));
        c->v[rd] = r;
        return 1;
    }
    switch (imm4) {
        case 0: /* DUP (element) */
        case 1: /* DUP (general) */
        {
            u64 x = imm4 ? RX(rn) : eget(&n, size, index);
            if (size == 3 && !q) return 0;
            for (i = 0; i < ((q ? 16u : 8u) >> size); ++i) eset(&r, size, i, x);
            vwrite(c, rd, r, (int)q);
            return 1;
        }
        case 3: /* INS (general) */
            if (!q) return 0;
            r = c->v[rd];
            eset(&r, size, index, RX(rn));
            c->v[rd] = r;
            return 1;
        case 5: /* SMOV */
            if (size == 3 || (size == 2 && !q)) return 0;
            SETX(rd, q ? (u64)sx(eget(&n, size, index), size) : (u64)(u32)sx(eget(&n, size, index), size));
            return 1;
        case 7: /* UMOV */
            if (q ? size != 3 : size == 3) return 0;
            SETX(rd, eget(&n, size, index));
            return 1;
        default: return 0;
    }
}

/* ---- modified immediate ---- */

/* AdvSIMDExpandImm */
u64 v_expand_imm(u32 op, u32 cmode, u32 imm8) {
    u64 imm = imm8 & 0xFF, r = 0;
    u32 i;
    switch (cmode >> 1) {
        case 0: return imm | imm << 32;
        case 1: return (imm << 8) | (imm << 40);
        case 2: return (imm << 16) | (imm << 48);
        case 3: return (imm << 24) | (imm << 56);
        case 4: return (imm | imm << 16) * SNX_U64C(0x00000001u, 0x00000001u);
        case 5: return ((imm << 8) | (imm << 24)) * SNX_U64C(0x00000001u, 0x00000001u);
        case 6:
            r = (cmode & 1) ? (imm << 16) | 0xFFFF : (imm << 8) | 0xFF;
            return r | r << 32;
        default:
            if (!(cmode & 1)) {
                if (!op) return imm * SNX_U64C(0x01010101u, 0x01010101u);
                for (i = 0; i < 8; ++i)
                    if ((imm >> i) & 1) r |= (u64)0xFF << (8 * i);
                return r;
            }
            if (!op) return (u64)vfp_imm32(imm8) | (u64)vfp_imm32(imm8) << 32;
            return vfp_imm64(imm8);
    }
}

static int simd_modimm(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, op = (w >> 29) & 1, cmode = (w >> 12) & 15, rd = w & 31;
    u32 imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
    u64 imm;
    V128 r;
    if ((w >> 11) & 1) return 0; /* o2: FMOV (half) is ARMv8.2 */
    if (cmode == 15 && op && !q) return 0;
    imm = v_expand_imm(op, cmode, imm8);
    r = c->v[rd];
    if (cmode < 12 && (cmode & 1)) { /* ORR, BIC */
        r.d[0] = op ? r.d[0] & ~imm : r.d[0] | imm;
        r.d[1] = op ? r.d[1] & ~imm : r.d[1] | imm;
    } else { /* MOVI, MVNI, FMOV */
        if (op && cmode < 14) imm = ~imm;
        r.d[0] = imm;
        r.d[1] = imm;
    }
    vwrite(c, rd, r, (int)q);
    return 1;
}

/* ---- shift by immediate, vector and scalar ---- */
static int simd_shimm(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, u = (w >> 29) & 1, scalar = (w >> 28) & 1, immh = (w >> 19) & 15, op = (w >> 11) & 31;
    u32 rn = (w >> 5) & 31, rd = w & 31, immhb = (w >> 16) & 127, size, bits, lanes, i, rs, ls;
    int narrow = op >= 0x10 && op <= 0x13, wide = op == 0x14;
    V128 n = c->v[rn], d = c->v[rd], r;
    if (!immh) return 0;
    for (size = 3; !((immh >> size) & 1); --size) {
    }
    bits = 8u << size;
    rs = 2 * bits - immhb; /* right shifts: 1..bits */
    ls = immhb - bits;     /* left shifts: 0..bits-1 */
    vzero(&r);
    switch (op) {
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x06:
        case 0x0A: break;
        case 0x08:
        case 0x0C:
            if (!u) return 0;
            break;
        case 0x0E: break;
        case 0x10:
        case 0x11:
            if (scalar && !u) return 0;
            break;
        case 0x12:
        case 0x13: break;
        case 0x14:
            if (scalar) return 0;
            break;
        case 0x1C:
        case 0x1F:
            if (size < 2) return 0;
            break;
        default: return 0;
    }
    if (narrow || wide) {
        if (size == 3) return 0;
    } else if (scalar) {
        if (size != 3 && op <= 0x0A) return 0;
    } else if (size == 3 && !q) {
        return 0;
    }
    if (op == 0x1C || op == 0x1F) { /* SCVTF UCVTF FCVTZS FCVTZU, fixed point: fbits = rs */
        lanes = scalar ? 1 : (q ? 16u : 8u) >> size;
        for (i = 0; i < lanes; ++i) {
            u64 x = eget(&n, size, i);
            eset(&r, size, i,
                 op == 0x1C ? snx_fp_from_int(c, x, (int)bits, (int)bits, (int)u, (int)rs)
                            : snx_fp_to_int(c, x, (int)bits, (int)bits, (int)u, (int)rs, 3));
        }
        vwrite(c, rd, r, scalar ? 0 : (int)q);
        return 1;
    }
    if (wide) { /* SSHLL USHLL (SXTL UXTL) */
        lanes = 8u >> size;
        for (i = 0; i < lanes; ++i) {
            u64 x = eget(&n, size, i + q * lanes);
            eset(&r, size + 1, i, zx((u ? zx(x, size) : (u64)sx(x, size)) << ls, size + 1));
        }
        vwrite(c, rd, r, 1);
        return 1;
    }
    if (narrow) { /* SHRN RSHRN SQSHRUN SQRSHRUN SQSHRN UQSHRN SQRSHRN UQRSHRN: double-width lanes to size */
        int round = op & 1, kind;
        lanes = scalar ? 1 : 8u >> size;
        if (q && !scalar) r.d[0] = d.d[0];
        kind = op <= 0x11 ? (u ? 3 : 0) : (u ? 2 : 1);
        for (i = 0; i < lanes; ++i) {
            u64 x = eget(&n, size + 1, i), s, out;
            s = shr_lane(x, size + 1, rs, kind == 0 || kind == 2, round);
            switch (kind) {
                case 0: out = zx(s, size); break;
                case 1: out = ssat(c, (i64)s, size); break;
                default: out = usat(c, (i64)s, size); break; /* a shifted 64-bit source fits i64 */
            }
            eset(&r, size, i + (scalar ? 0 : q * lanes), out);
        }
        vwrite(c, rd, r, scalar ? 0 : 1);
        return 1;
    }
    lanes = scalar ? 1 : (q ? 16u : 8u) >> size;
    for (i = 0; i < lanes; ++i) {
        u64 x = eget(&n, size, i), acc = eget(&d, size, i), out;
        switch (op) {
            case 0x00: out = shr_lane(x, size, rs, (int)u, 0); break;
            case 0x02: out = acc + shr_lane(x, size, rs, (int)u, 0); break;
            case 0x04: out = shr_lane(x, size, rs, (int)u, 1); break;
            case 0x06: out = acc + shr_lane(x, size, rs, (int)u, 1); break;
            case 0x08: { /* SRI: the shifted bits inserted, the top rs bits of the destination kept */
                u64 mask = rs >= bits ? 0 : ones(size) >> rs;
                out = (acc & ~mask) | (lsr_(zx(x, size), rs) & mask);
                break;
            }
            case 0x0A:
                if (u) { /* SLI */
                    u64 mask = (ones(size) << ls) & ones(size);
                    out = (acc & ~mask) | ((x << ls) & mask);
                } else {
                    out = x << ls; /* SHL */
                }
                break;
            case 0x0C: out = sqshlu_(c, x, ls, size); break;
            default: out = vshl(c, x, (i32)ls, size, (int)u, 0, 1); break; /* 0x0E SQSHL UQSHL */
        }
        eset(&r, size, i, zx(out, size));
    }
    vwrite(c, rd, r, scalar ? 0 : (int)q);
    return 1;
}

/* ---- by element ---- */
static int simd_elem(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, u = (w >> 29) & 1, scalar = (w >> 28) & 1, size = (w >> 22) & 3, op = (w >> 12) & 15;
    u32 L = (w >> 21) & 1, M = (w >> 20) & 1, H = (w >> 11) & 1, rn = (w >> 5) & 31, rd = w & 31, rm, index;
    u32 i, lanes;
    V128 n = c->v[rn], d = c->v[rd], r;
    u64 e;
    vzero(&r);
    if (op == 1 || op == 5 || op == 9) { /* FMLA FMLS FMUL, FMULX (U, op 9) */
        u32 sz = size & 1;
        if ((u && op != 9) || size < 2 || (sz && L) || (sz && !q && !scalar)) return 0;
        rm = (w >> 16) & 31;
        index = sz ? H : (H << 1) | L;
        e = eget(&c->v[rm], 2 + sz, index);
        lanes = scalar ? 1 : (q ? 4u : 2u) >> sz;
        for (i = 0; i < lanes; ++i) {
            u64 a = eget(&n, 2 + sz, i), out;
            if (op == 9)
                out = snx_fp_binary(c, a, e, fwidth(sz), u ? SNX_FP_MULX : SNX_FP_MUL);
            else
                out = snx_fp_fma(c, eget(&d, 2 + sz, i), op == 5 ? a ^ fsign(sz) : a, e, fwidth(sz));
            eset(&r, 2 + sz, i, out);
        }
        vwrite(c, rd, r, scalar ? 0 : (int)q);
        return 1;
    }
    if (size != 1 && size != 2) return 0;
    rm = size == 1 ? (w >> 16) & 15 : (w >> 16) & 31;
    index = size == 1 ? (H << 2) | (L << 1) | M : (H << 1) | L;
    e = eget(&c->v[rm], size, index);
    switch (op) {
        case 0x0: /* MLA, MLS (U) */
        case 0x4:
        case 0x8: /* MUL */
            if (scalar || (op == 8 ? u : !u)) return 0;
            lanes = (q ? 16u : 8u) >> size;
            for (i = 0; i < lanes; ++i) {
                u64 p = eget(&n, size, i) * e, acc = eget(&d, size, i);
                eset(&r, size, i, zx(op == 0 ? acc + p : op == 4 ? acc - p : p, size));
            }
            vwrite(c, rd, r, (int)q);
            return 1;
        case 0xC:
        case 0xD: /* SQDMULH SQRDMULH */
            if (u) return 0;
            lanes = scalar ? 1 : (q ? 16u : 8u) >> size;
            for (i = 0; i < lanes; ++i) eset(&r, size, i, sqdmulh_(c, eget(&n, size, i), e, size, op == 0xD));
            vwrite(c, rd, r, scalar ? 0 : (int)q);
            return 1;
        case 0x2:
        case 0x6:
        case 0xA: /* SMLAL SMLSL SMULL, UMLAL UMLSL UMULL */
        case 0x3:
        case 0x7:
        case 0xB: /* SQDMLAL SQDMLSL SQDMULL */
            if ((op & 1) ? u : scalar) return 0;
            lanes = scalar ? 1 : 8u >> size;
            for (i = 0; i < lanes; ++i) {
                u64 a = eget(&n, size, i + (scalar ? 0 : q * lanes)), acc = eget(&d, size + 1, i), out;
                if (op & 1) {
                    u64 p = sqdmull_(c, a, e, size);
                    out = op == 3 ? sqadd_(c, acc, p, size + 1) : op == 7 ? sqsub_(c, acc, p, size + 1) : p;
                } else {
                    u64 p = u ? zx(a, size) * zx(e, size) : (u64)(sx(a, size) * sx(e, size));
                    out = op == 2 ? acc + p : op == 6 ? acc - p : p;
                }
                eset(&r, size + 1, i, zx(out, size + 1));
            }
            vwrite(c, rd, r, scalar ? 0 : 1);
            return 1;
        default: return 0;
    }
}

/* ---- permute, extract, table lookup ---- */
static int simd_perm(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, size = (w >> 22) & 3, op = (w >> 12) & 7, rm = (w >> 16) & 31, rn = (w >> 5) & 31;
    u32 rd = w & 31, lanes = (q ? 16u : 8u) >> size, half = lanes / 2, part = op >> 2, i;
    V128 n = c->v[rn], m = c->v[rm], r;
    if (!(op & 3) || (size == 3 && !q)) return 0;
    vzero(&r);
    for (i = 0; i < lanes; ++i) {
        u64 x;
        switch (op & 3) {
            case 1: { /* UZP1 UZP2: the even (odd) lanes of n:m */
                u32 k = 2 * i + part;
                x = k < lanes ? eget(&n, size, k) : eget(&m, size, k - lanes);
                break;
            }
            case 2: /* TRN1 TRN2 */
                x = eget((i & 1) ? &m : &n, size, (i & ~1u) + part);
                break;
            default: /* ZIP1 ZIP2 */
                x = eget((i & 1) ? &m : &n, size, i / 2 + part * half);
                break;
        }
        eset(&r, size, i, x);
    }
    vwrite(c, rd, r, (int)q);
    return 1;
}

static int simd_ext(CPU* c, u32 w) {
    u32 q = (w >> 30) & 1, imm4 = (w >> 11) & 15, rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, i;
    u32 bytes = q ? 16 : 8;
    V128 n = c->v[rn], m = c->v[rm], r;
    if ((w >> 22) & 3) return 0;
    if (!q && (imm4 & 8)) return 0;
    vzero(&r);
    for (i = 0; i < bytes; ++i) {
        u32 k = imm4 + i;
        r.b[i] = k < bytes ? n.b[k] : m.b[k - bytes];
    }
    vwrite(c, rd, r, (int)q);
    return 1;
}

/* TBL, TBX: bytes of the table n..n+len (registers wrap at 32); an index past it gives 0, or keeps Vd's byte */
void v_tbl(CPU* c, u32 d, u32 n, u32 len, u32 m, int q, int tbx) {
    V128 table[4], idx = c->v[m], r = c->v[d];
    u32 i, count = len + 1;
    for (i = 0; i < count; ++i) table[i] = c->v[(n + i) & 31];
    for (i = 0; i < (q ? 16u : 8u); ++i) {
        u32 k = idx.b[i];
        if (k < 16 * count)
            r.b[i] = table[k >> 4].b[k & 15];
        else if (!tbx)
            r.b[i] = 0;
    }
    vwrite(c, d, r, q);
}
