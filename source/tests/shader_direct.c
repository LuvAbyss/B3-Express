/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED   0x001F8000FC0007E0ull
#define EXIT    0xE30000000007000Full
#define FMUL    0x5C68000000070000ull
#define IPA     0xE000000000070000ull
#define MUFU    0x5080000000070000ull
#define FFMA    0x5980000000070000ull
#define MOV_C   0x4C98000000070000ull
#define TEXS    0xD800000000070000ull
#define FSETP   0x5BB0000000070000ull
#define FMNMX   0x5C60000000070000ull
#define BRA     0xE24000000007000Full
#define AST     0xEFF0000000070000ull
#define FADD32I 0x0800000000070000ull
#define SYNC    0xF0F800000007000Full
#define VOTE    0x50D8000000070000ull
#define SHFL    0xEF10000000070000ull

static u64 mov32i(u32 reg, u32 value) { return 0x0100000000000000ull | ((u64)value << 20) | 0x71000ull | reg; }

static int save(const char* path, const u32* spv, size_t words) {
    FILE* out = fopen(path, "wb");
    if (!out) return 0;
    int ok = fwrite(spv, sizeof *spv, words, out) == words;
    if (fclose(out)) ok = 0;
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 6) return 2;
    int check_only = argc == 2 && !strcmp(argv[1], "--check-only");
    if (argc == 2 && !check_only) return 2;
    u64 code[64] = {0};
    ((u32*)code)[18] = 15; /* one RGBA fragment output */
    ((u8*)code)[0x18] = 3; /* input location 0 is screen linear */
    code[10] = SCHED;
    code[11] = IPA | (0x20ull << 30);    /* r0 = location 0, x */
    code[12] = mov32i(1, 0x3f000000);    /* 0.5 */
    code[13] = MOV_C | (1ull << 39) | 1; /* r1 = c[0][0] */
    code[14] = SCHED;
    code[15] = FMUL | (1ull << 20) | 2;                /* r2 = r0 * r1 */
    code[16] = FFMA | (1ull << 39) | (1ull << 20) | 2; /* r2 = fma(r0, r1, r1) */
    code[17] = MUFU | (2ull << 20) | (1ull << 8) | 3;  /* r3 = exp2(r1) */
    code[18] = SCHED;
    code[19] = FSETP | (9ull << 48) | (7ull << 39) | (1ull << 8) | 7;   /* p0 = r1 < r0 */
    code[20] = TEXS | (1ull << 53) | (255ull << 28) | (1ull << 20) | 3; /* 2D sample to r3 */
    code[21] = (TEXS & ~(7ull << 16)) | (4ull << 53) | (1ull << 36) | (255ull << 28) | (1ull << 20) | 3;
    code[22] = SCHED;
    code[23] = FMNMX | (7ull << 39) | (1ull << 20) | 3; /* min(r0, r1) with NaN handling */
    code[24] = BRA | (16ull << 20);                     /* skip an instruction across a schedule word */
    code[25] = mov32i(3, 0);
    code[26] = SCHED;
    code[27] = EXIT;
    ShaderInfo info = {0};
    size_t words = 0;
    u32* spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    if (!spv || info.stage != ST_FS || info.sph[18] != 15 || info.cbuf_mask != 1 || info.cbuf_max[0] != 16 ||
        info.ntex != 2 || !info.tex[1].shadow) {
        free(spv);
        return 1;
    }
    int ok = check_only || save(argv[1], spv, words);
    free(spv);
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = AST | (0x70ull << 20) | (255ull << 8); /* position.x = r0; W retains its default 1 */
    code[12] = EXIT;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_VS, &info, &words, 1, 1, 1, 1);
    if (!spv || info.stage != ST_VS || (!check_only && !save(argv[2], spv, words))) ok = 0;
    free(spv);
    memset(code, 0, sizeof code);
    ((u32*)code)[18] = 15;
    code[10] = SCHED;
    code[11] = FADD32I | (1ull << 57); /* right-operand absolute value needs GLSL.std.450 */
    code[12] = EXIT;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    if (!spv || (!check_only && !save(argv[3], spv, words))) ok = 0;
    free(spv);
    code[11] = SYNC;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    if (!spv || (!check_only && !save(argv[4], spv, words))) ok = 0;
    free(spv);
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = VOTE | (7ull << 39); /* ballot P7 and vote all */
    code[12] = SHFL | (1ull << 28) | (1ull << 29) | (31ull << 34) | (1ull << 20) | 1;
    code[13] = EXIT;
    info = (ShaderInfo){0};
    info.cs_local[0] = 32;
    info.cs_local[1] = info.cs_local[2] = 1;
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_CS, &info, &words, 1, 1, 1, 1);
    if (!spv || (!check_only && !save(argv[5], spv, words))) ok = 0;
    free(spv);
    /* a pixel stage's SHFL reads its quad through fine derivatives (issue 17): no shuffle, quad swap or subgroup lane */
    memset(code, 0, sizeof code);
    ((u32*)code)[18] = 15;
    code[10] = SCHED;
    code[11] = SHFL | (3ull << 30) | (1ull << 28) | (1ull << 29) | (0x1c03ull << 34) | (1ull << 20) | (1ull << 8) | 2;
    code[12] = SHFL | (1ull << 29) | (0x1c03ull << 34) | (3ull << 20) | (1ull << 8) | 4; /* index in r3 */
    code[13] = EXIT;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    int fine = 0, lane_ops = 0;
    for (size_t i = 5; spv && i < words && spv[i] >> 16; i += spv[i] >> 16) {
        u32 op = spv[i] & 0xffff;
        if (op == 210 || op == 211) fine++;                               /* OpDPdxFine, OpDPdyFine */
        if (op == 345 || op == 366) lane_ops++;                           /* OpGroupNonUniformShuffle, QuadSwap */
        if (op == 71 && spv[i + 2] == 11 && spv[i + 3] == 41) lane_ops++; /* BuiltIn SubgroupLocalInvocationId */
    }
    if (!spv || !fine || lane_ops) ok = 0;
    free(spv);
    /* a pixel stage with flow kills only after it (upstream PR 512, issue 17): KIL marks the lane, EXIT doesn't
       return, so there is no demote, one OpKill and one OpReturn, the function's last */
    memset(code, 0, sizeof code);
    ((u32*)code)[18] = 15;
    code[10] = SCHED;
    code[11] = BRA | (8ull << 20);    /* over the next instruction */
    code[12] = 0xE33000000007000Full; /* KIL */
    code[13] = EXIT;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    int kills = 0, demotes = 0, returns = 0;
    for (size_t i = 5; spv && i < words && spv[i] >> 16; i += spv[i] >> 16) {
        u32 op = spv[i] & 0xffff;
        if (op == 252) kills++;    /* OpKill */
        if (op == 5380) demotes++; /* OpDemoteToHelperInvocation */
        if (op == 253) returns++;  /* OpReturn */
    }
    if (!spv || kills != 1 || demotes || returns != 1) ok = 0;
    free(spv);
    /* a pixel stage whose loop never ends leaves it as a helper after 4096 passes (helper_loop_exit, upstream d48364cc,
       issue 17's hang): the flow loop asks IsHelperInvocation and compares with 4096 */
    memset(code, 0, sizeof code);
    ((u32*)code)[18] = 15;
    code[10] = SCHED;
    code[11] = (BRA & ~(7ull << 16)) | (0xFFFFF8ull << 20); /* @P0 to itself */
    code[12] = EXIT;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    /* and, with no quad read in the loop, it runs as a structured loop around its blocks in order: no OpSwitch
       dispatcher (Witch Time PR 272, issue 17) */
    int helper_asks = 0, loops = 0, cap = 0, switches = 0;
    for (size_t i = 5; spv && i < words && spv[i] >> 16; i += spv[i] >> 16) {
        u32 op = spv[i] & 0xffff;
        if (op == 251) switches++;                                        /* OpSwitch */
        if (op == 5381) helper_asks++;                                    /* OpIsHelperInvocationEXT */
        if (op == 246) loops++;                                           /* OpLoopMerge */
        if (op == 43 && (spv[i] >> 16) == 4 && spv[i + 3] == 4096) cap++; /* OpConstant 4096 */
    }
    if (!spv || !helper_asks || !loops || !cap || switches) ok = 0;
    free(spv);
    /* a converging pixel stage leaves its flow loop only when the whole subgroup has finished (issue 17): one UMin
       picks the pass's pc, a second asks whether every lane but the helpers is done. The quad read inside the loop
       keeps it out of the in-order blocks */
    memset(code, 0, sizeof code);
    ((u32*)code)[18] = 15;
    code[10] = SCHED;
    code[11] = SHFL | (3ull << 30) | (1ull << 28) | (1ull << 29) | (0x1c03ull << 34) | (1ull << 20) | (1ull << 8) | 2;
    code[12] = (BRA & ~(7ull << 16)) | (0xFFFFF0ull << 20); /* @P0 back to the SHFL */
    code[13] = EXIT;
    info = (ShaderInfo){0};
    spv = shader_to_spirv((const u8*)code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
    int umins = 0;
    for (size_t i = 5; spv && i < words && spv[i] >> 16; i += spv[i] >> 16)
        if ((spv[i] & 0xffff) == 354) umins++; /* OpGroupNonUniformUMin */
    if (!spv || umins != 2) ok = 0;
    free(spv);
    return ok ? 0 : 1;
}
