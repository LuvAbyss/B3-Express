/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The out-of-line helpers of the generated-code ABI (cpu.h), compiled into every game module's runtime library
   (llvm/rtlib.c), into the gnuabi execution path's generated C and into the tests, after cpu.h. Generated code calls
   snx_fp_*, snx_simd, snx_cas128, snx_mrs/snx_msr and snx_crc32 by name; the templates' C calls the rest. They are
   plain portable C: the library is built for no particular host CPU. */
#include "cpu.h"

#include "helpers/fp.c"
#include "helpers/integer.c"
#include "helpers/crypto.c"
#include "helpers/simd.c"
#include "helpers/simd_mem.c"

/* AdvSIMD, crypto and SIMD load/store words, by the encoding groups of the Arm ARM's top-level A64 decode */
int snx_simd(CPU* c, u8* M, u32 w) {
    if ((w & 0xBF800000UL) == 0x0C000000UL || (w & 0xBF800000UL) == 0x0C800000UL ||
        (w & 0xBF000000UL) == 0x0D000000UL)
        return simd_ldst(c, M, w);
    if ((w & 0xFFFE0C00UL) == 0x4E280800UL) { /* AESE AESD AESMC AESIMC */
        u32 op = (w >> 12) & 31, rd = w & 31, rn = (w >> 5) & 31;
        V128 s = c->v[rn];
        if (op < 4 || op > 7) return 0;
        if (op < 6)
            v_aes(&c->v[rd], &s, op == 5);
        else
            v_aesmc(&c->v[rd], &s, op == 7);
        return 1;
    }
    if ((w & 0xFFE08C00UL) == 0x5E000000UL) { /* SHA1C/P/M, SHA1SU0, SHA256H/H2, SHA256SU1 */
        u32 op = (w >> 12) & 7, rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        if (op == 7) return 0;
        if (op < 4)
            v_sha1(c, op, rd, rn, rm);
        else
            v_sha256(c, op, rd, rn, rm);
        return 1;
    }
    if ((w & 0xFFFE0C00UL) == 0x5E280800UL) { /* SHA1H, SHA1SU1, SHA256SU0 */
        u32 op = (w >> 12) & 31, rd = w & 31, rn = (w >> 5) & 31;
        if (op > 2) return 0;
        if (op < 2)
            v_sha1(c, 0x10 + op, rd, rn, rn);
        else
            v_sha256(c, 0x12, rd, rn, rn);
        return 1;
    }
    if ((w & 0x9F200400UL) == 0x0E200400UL || (w & 0xDF200400UL) == 0x5E200400UL) return simd_3same(c, w);
    if ((w & 0x9F200C00UL) == 0x0E200000UL || (w & 0xDF200C00UL) == 0x5E200000UL) return simd_3diff(c, w);
    if ((w & 0x9F3E0C00UL) == 0x0E200800UL || (w & 0xDF3E0C00UL) == 0x5E200800UL) return simd_2misc(c, w);
    if ((w & 0x9F3E0C00UL) == 0x0E300800UL) return simd_across(c, w);
    if ((w & 0xDF3E0C00UL) == 0x5E300800UL) return simd_scalar_pairwise(c, w);
    if ((w & 0x9FE08400UL) == 0x0E000400UL || (w & 0xDFE08400UL) == 0x5E000400UL) return simd_copy(c, w);
    if ((w & 0x9FF80400UL) == 0x0F000400UL) return simd_modimm(c, w);
    if ((w & 0x9F800400UL) == 0x0F000400UL || (w & 0xDF800400UL) == 0x5F000400UL) return simd_shimm(c, w);
    if ((w & 0x9F000400UL) == 0x0F000000UL || (w & 0xDF000400UL) == 0x5F000000UL) return simd_elem(c, w);
    if ((w & 0xBF208C00UL) == 0x0E000800UL) return simd_perm(c, w);
    if ((w & 0xBF208400UL) == 0x2E000000UL) return simd_ext(c, w);
    if ((w & 0xBF208C00UL) == 0x0E000000UL) { /* TBL, TBX */
        if ((w >> 22) & 3) return 0;
        v_tbl(c, w & 31, (w >> 5) & 31, (w >> 13) & 3, (w >> 16) & 31, (int)((w >> 30) & 1), (int)((w >> 12) & 1));
        return 1;
    }
    return 0;
}
