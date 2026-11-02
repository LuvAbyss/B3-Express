#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Thumb16/Thumb32 LLVM execution vs ARMv8 AArch32 (Unicorn 2.1.4).

Assemble each mnemonics line with llvm-mc -mcpu=cortex-a57, emit a Thumb block
per instruction/sequence, run it from random state and compare every GPR, CPSR
and guest memory. Exact raw-bit comparisons only. No game assets.
"""

import argparse
import ctypes as C
import pathlib
import random
import re
import subprocess
import tempfile

from arm_test import CPU, PC, SIZE


def catalogue():
    single = [
        # shift by immediate
        'lsl r0, r1, #1',
        'lsl r3, r4, #0',
        'lsr r0, r1, #32',
        'asr r2, r5, #31',
        # add/sub register and imm3
        'add r0, r1, r2',
        'sub r3, r4, r5',
        'add r0, r1, #7',
        'sub r2, r3, #0',
        # mov/cmp/add/sub imm8
        'mov r0, #0',
        'mov r7, #255',
        'cmp r1, #128',
        'add r2, #10',
        'sub r3, #10',
        # data-processing register
        'and r0, r1',
        'eor r0, r1',
        'lsl r0, r1',
        'lsr r0, r1',
        'asr r0, r1',
        'adc r0, r1',
        'sbc r0, r1',
        'ror r0, r1',
        'tst r0, r1',
        'neg r0, r1',
        'cmp r0, r1',
        'cmn r0, r1',
        'orr r0, r1',
        'mul r0, r1',
        'bic r0, r1',
        'mvn r0, r1',
        # high registers / branch exchange
        'add r0, r8',
        'add r8, r1',
        'add r9, r10',
        'cmp r1, r9',
        'mov r8, r2',
        'mov r0, r11',
        'bx r3',
        'blx r4',
        'bx lr',
        'mov pc, r2',
        # load/store register offset
        'str r0, [r1, r2]',
        'strh r0, [r1, r2]',
        'strb r0, [r1, r2]',
        'ldrsb r0, [r1, r2]',
        'ldr r0, [r1, r2]',
        'ldrh r0, [r1, r2]',
        'ldrb r0, [r1, r2]',
        'ldrsh r0, [r1, r2]',
        # load/store immediate
        'str r0, [r1, #0]',
        'str r0, [r1, #124]',
        'ldr r0, [r1, #124]',
        'ldr r0, [r1, #0]',
        'strb r0, [r1, #31]',
        'ldrb r0, [r1, #31]',
        'strh r0, [r1, #62]',
        'ldrh r0, [r1, #62]',
        # sp-relative
        'str r0, [sp, #252]',
        'ldr r0, [sp, #252]',
        'ldr r7, [sp, #0]',
        # load address
        'add r0, pc, #1020',
        'add r0, sp, #1020',
        'add r3, sp, #0',
        # add/sub sp
        'add sp, #128',
        'sub sp, #128',
        'add sp, #0',
        # extend
        'sxtb r0, r1',
        'sxth r0, r1',
        'uxtb r0, r1',
        'uxth r0, r1',
        # push/pop
        'push {r0, r1}',
        'push {lr}',
        'push {r0-r7, lr}',
        'pop {r0, r1}',
        'pop {r0-r7, pc}',
        # ldm/stm
        'stmia r0!, {r1, r2}',
        'ldmia r0!, {r1, r2}',
        'ldmia r0!, {r1-r7}',
        # branches and svc
        'beq #254',
        'bne #0',
        'b #2046',
        'b #-2048',
        'svc #255',
        'cbz r0, #4',
        'cbnz r1, #64',
        # Thumb32 data-processing / bitfield / wide immediate
        'movw r0, #0',
        'movw r3, #65535',
        'movt r0, #0',
        'movt r3, #65535',
        'addw r0, r1, #0',
        'addw r3, r4, #4095',
        'subw r0, r1, #4095',
        'subw r3, r4, #0',
        'sbfx r0, r1, #0, #1',
        'sbfx r3, r4, #31, #1',
        'ubfx r0, r1, #4, #8',
        'ubfx r3, r4, #0, #32',
        'bfi r0, r1, #4, #8',
        'bfc r0, #4, #8',
        'bfc r3, #0, #32',
        'sxtb.w r0, r1',
        'uxth.w r3, r4',
        # Thumb32 wide data-processing (shifted register)
        'and.w r0, r1, r2',
        'orr.w r0, r1, r2',
        'eor.w r0, r1, r2',
        'bic.w r0, r1, r2',
        'orn r0, r1, r2',
        'mvn.w r0, r1',
        'add.w r0, r1, r2',
        'adc.w r0, r1, r2',
        'sbc.w r0, r1, r2',
        'sub.w r0, r1, r2',
        'rsb.w r0, r1, #0',
        'mov.w r0, r1',
        'lsl.w r0, r1, r2',
        'lsr.w r0, r1, r2',
        'asr.w r0, r1, r2',
        'ror.w r0, r1, r2',
        'tst.w r0, r1',
        'teq r0, r1',
        'cmn.w r0, r1',
        'cmp.w r0, r1',
        # Thumb32 multiply
        'mul r0, r1',
        'mla r0, r1, r2, r3',
        'mls r0, r1, r2, r3',
        # Thumb32 wide load/store
        'ldr.w r0, [r1, #255]',
        'ldr.w r0, [r1, #2304]',  # imm12 with bits 11 and 8 set: not the imm8 form
        'ldr r0, [r1, #-8]',
        'ldrb r0, [r1, #-1]',
        'ldrsh r0, [r1, #-2]',
        'strb r0, [r1, #-1]!',
        'ldrt r0, [r1, #4]',
        'ldr.w r0, [pc, #16]',
        'ldr.w r0, [pc, #-16]',
        'pld [r1, #64]',
        'ldrd r0, r1, [r2, #8]',
        'ldrd r3, r10, [r2, #-1020]',
        'ldrd r4, r5, [r2], #-8',
        'ldrd r0, r1, [pc, #8]',
        'strd r3, r9, [r2, #16]!',
        'strd r0, r1, [r2]',
        # Thumb32 LDM/STM (A32 twins)
        'ldm.w r0!, {r1, r2, r8}',
        'ldmdb r0, {r1, r9}',
        'stmdb sp!, {r1, r2, lr}',
        'stm.w r0, {r1, r10}',
        'push.w {r4-r11, lr}',
        'pop.w {r4-r11}',
        'pop.w {r4, r5, pc}',
        # Thumb32 branches and miscellaneous control
        'beq.w #4096',
        'blt.w #-4092',
        'b.w #-2',
        'dmb ish',
        'dsb sy',
        'isb sy',
        'clrex',
        'nop.w',
        'mrs r0, apsr',
        'msr apsr_nzcvq, r1',
        # Thumb32 register data processing (REV, CLZ, extends with add: A32 twins)
        'clz r0, r1',
        'rbit r0, r1',
        'rev.w r0, r1',
        'rev16.w r0, r1',
        'revsh.w r0, r1',
        'rev r0, r1',
        'rev16 r0, r1',
        'revsh r0, r1',
        'sxtab r0, r1, r2',
        'uxtah r0, r1, r2, ror #8',
        'uxtb.w r0, r1, ror #16',
        'sxth.w r3, r4, ror #24',
        'lsls.w r0, r1, r2',
        'ands.w r0, r1, #0xff000000',  # a rotated immediate: C is its bit 31
        'orrs.w r0, r1, #255',  # an unrotated one: C unchanged
        'tst.w r0, #0x80000000',
        'movs.w r0, r1, lsr #1',
        'eors.w r0, r1, r2, ror #4',
        'mvns.w r0, r1, asr #32',
        'rors.w r0, r1, r2',
        # Thumb32 long multiply and divide (A32 twins)
        'smull r0, r1, r2, r3',
        'umull r0, r1, r2, r3',
        'smlal r0, r1, r2, r3',
        'umlal r0, r1, r2, r3',
        'sdiv r0, r1, r2',
        'udiv r0, r1, r2',
        # Thumb32 exclusives and acquire/release (A32 twins, a doubleword one's pair any two registers)
        'ldrex r0, [r1]',
        'strex r2, r3, [r1]',
        'lda r0, [r1]',
        'stl r0, [r1]',
        'ldaexd r0, r1, [r12]',
        # Thumb32 coprocessor, VFP and Advanced SIMD (A32 twins)
        'vldr d0, [r1, #8]',
        'vldr d1, [pc, #8]',
        'vstr s2, [r1, #4]',
        'vadd.f64 d0, d1, d2',
        'vmul.f32 s0, s1, s2',
        'vadd.i32 q8, q8, q9',
        'vld1.32 {d16, d17}, [r0]',
        'vst1.8 {d0}, [r1]!',
        'vmov r0, s1',
        'vmov s3, r2',
        'vmrs r0, fpscr',
        'vpush {d8, d9}',
        'vpop {d8}',
        'vcvt.f32.s32 s0, s1',
        'vselgt.f32 s0, s1, s2',
        'vrintm.f64 d0, d1',
        'aese.8 q0, q1',
        'str.w r0, [r1, #255]',
        'ldr.w r0, [r1, r2]',
        'str.w r0, [r1, r2]',
        'ldrb.w r0, [r1, #255]',
        'ldrh.w r0, [r1, #255]',
        'ldrsb.w r0, [r1, r2]',
        'ldrsh.w r0, [r1, r2]',
        'ldr.w r0, [r1, #4]!',
        'ldr.w r0, [r1], #4',
        'str.w r0, [r1, #4]!',
    ]
    # short IT-guarded sequences (branches are unpredictable inside IT blocks)
    it_seq = [
        # Guards use hi-reg MOV/ADD, which do not set flags, keeping the IT
        # condition stable. llvm-mc still wants matching condition suffixes.
        'it eq; moveq r8, r1',
        'itt ne; movne r8, r1; addne r0, r8',
        'ittt eq; moveq r8, r1; addeq r0, r8; moveq r9, r2',
        'itttt ne; movne r8, r1; addne r0, r8; movne r9, r2; addne r3, r8',
        'ite eq; moveq r8, r1; movne r8, r2',
        'itte eq; moveq r8, r1; addeq r0, r8; movne r8, r3',
        'itee eq; moveq r8, r1; movne r8, r2; movne r8, r3',
        # PC reads at an address 2 mod 4: ADD from the PC is PC + 4, literals and ADR use Align(PC, 4)
        'nop; add r0, pc',
        'nop; mov r0, pc',
        'nop; ldr.w r0, [pc, #12]',
        'nop; adr.w r0, #12',
        'nop; vldr d1, [pc, #8]',
        # an exclusive pair, and the doubleword ones with any second register
        'ldrex r0, [r1]; strex r2, r3, [r1]',
        'ldrexb r0, [r1]; strexb r2, r3, [r1]',
        'ldrexh r0, [r1]; strexh r2, r3, [r1]',
        'ldrexd r4, r5, [r1]; strexd r2, r6, r7, [r1]',
        'ldrexd r0, r8, [r2]; strexd r3, r9, r4, [r2]',
        'ldaex r0, [r1]; stlex r2, r3, [r1]',
        'ldaexd r0, r1, [r12]; stlexd r3, lr, r2, [r12]',
    ]
    lines = []
    for m in single:
        lines.append((m, 1))
    for seq in it_seq:
        lines.append((seq, seq.count(';') + 1))
    return lines


def assemble(mc, entries):
    out = []
    for text, n in entries:
        source = '.syntax unified\n.thumb\n' + '\n'.join(p.strip() for p in text.split(';')) + '\n'
        proc = subprocess.run(
            [mc, '-triple=armv8-linux-gnueabi', '-mcpu=cortex-a57', '-show-encoding'],
            input=source,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise RuntimeError(proc.stderr + '\n' + source)
        hw = []
        count = 0
        for line in proc.stdout.splitlines():
            if 'encoding:' not in line:
                continue
            data = re.findall(r'0x[0-9a-f]+', line.split('encoding:')[1])
            w = int.from_bytes(bytes(int(v, 16) for v in data), 'little')
            count += 1
            if len(data) == 2:
                hw.append(w & 0xFFFF)
            else:
                hw.append(w & 0xFFFF)
                hw.append((w >> 16) & 0xFFFF)
        assert count == n, f'{text}: assembled {count} instructions, expected {n}'
        out.append(hw)
    return out


EDGE = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0x0000FFFF, 0xFFFF0000)


def initial(seed, n, is_mem):
    rng = random.Random(seed)
    cpu = CPU()
    mem = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu.mem = mem
    cpu.mem_size = SIZE
    cpu.cpsr = ((seed & 15) << 28) | (1 << 5) | 0x10  # Thumb state (T bit)
    for i in range(16):
        if is_mem:
            cpu.r[i] = 0x1000 + (rng.getrandbits(32) % 0x7000 & ~15)
        elif seed < len(EDGE):
            cpu.r[i] = EDGE[(seed + i) % len(EDGE)]
        else:
            cpu.r[i] = rng.getrandbits(32)
    cpu.r[15] = PC
    for i in range(32):
        cpu.d[i] = rng.getrandbits(64)
    mem[PC : PC + 4 * n] = bytes(4 * n)
    return cpu, mem


def compare(functions, entries, halfwords, states):
    import unicorn as U
    from unicorn import arm_const as A

    regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]
    total = 0
    for index, (text, n) in enumerate(entries):
        fn = functions[index]
        hws = halfwords[index]
        u = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_THUMB)
        u.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
        u.mem_map(0, SIZE)
        is_mem = '[' in text or any(op in text for op in ('push', 'pop', 'ldm', 'stm', 'vst1', 'vld1'))
        for seed in range(states):
            cpu, mem = initial(seed, len(hws) // 2, is_mem)
            for i, h in enumerate(hws):
                mem[PC + 2 * i : PC + 2 * i + 2] = h.to_bytes(2, 'little')
            u.mem_write(0, bytes(mem))
            u.ctl_remove_cache(0, SIZE)
            u.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
            u.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
            u.reg_write(A.UC_ARM_REG_FPSCR, cpu.fpscr)
            for r, value in zip(regs, cpu.r):
                u.reg_write(r, value)
            for i, value in enumerate(cpu.d):
                u.reg_write(A.UC_ARM_REG_D0 + i, value)
            try:
                u.emu_start(PC | 1, PC + 2 * len(hws), count=n)
                fetched = True
            except U.UcError as exc:
                # Register-indirect branches jump to a random target; Unicorn
                # fetch-faults there. The branch itself completed (PC is set).
                if 'FETCH_UNMAPPED' in str(exc):
                    fetched = False
                elif 'EXCEPTION' not in str(exc):
                    raise AssertionError(f'Unicorn rejected {text}: {exc}') from exc
                fetched = False
            fn(C.byref(cpu))
            if not fetched and text.startswith('svc'):
                # SVC raises on real hardware too; check the reported exit.
                assert cpu.exit == 4, f'{text} seed={seed} expected SVC exit, got {cpu.exit}'
                total += 1
                continue
            actual = list(cpu.r) + [cpu.cpsr] + list(cpu.d) + [cpu.fpscr]
            expected = (
                [u.reg_read(r) for r in regs]
                + [u.reg_read(A.UC_ARM_REG_CPSR)]
                + [u.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)]
                + [u.reg_read(A.UC_ARM_REG_FPSCR)]
            )
            label = f'{text} seed={seed}'
            assert cpu.exit == 0, f'{label} exit={cpu.exit} syndrome={cpu.syndrome:x}'
            assert actual == expected, (
                label
                + ': '
                + ', '.join(f'r{i}: {x:08x} != {y:08x}' for i, (x, y) in enumerate(zip(actual, expected)) if x != y)
            )
            if fetched:
                assert bytes(mem) == bytes(u.mem_read(0, SIZE)), label + ': memory differs'
            total += 1
    print(f'  {total} exact comparisons passed ({len(entries)} sequences)')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--emitter', required=True)
    p.add_argument('--mc', required=True)
    p.add_argument('--cc', default='clang')
    p.add_argument('--runtime', required=True, help='libsnx_arm_rt.a')
    p.add_argument('--states', type=int, default=24)
    p.add_argument('--opt', choices=('0', '2'), action='append')
    args = p.parse_args()
    entries = catalogue()
    halfwords = assemble(args.mc, entries)
    with tempfile.TemporaryDirectory(prefix='snx-thumb-') as path:
        directory = pathlib.Path(path)
        source = directory / 'blocks.txt'
        source.write_text(''.join(f'{PC:x} ' + ' '.join(f'{h:04x}' for h in hw) + '\n' for hw in halfwords))
        for opt in args.opt or ('0', '2'):
            print(f'Thumb16/32 LLVM O{opt}', flush=True)
            obj, dll = directory / f'test{opt}.o', directory / f'test{opt}.so'
            subprocess.run([args.emitter, str(source), str(obj), opt], check=True)
            subprocess.run([args.cc, '-shared', str(obj), args.runtime, '-o', str(dll)], check=True)
            lib = C.CDLL(str(dll))
            functions = [getattr(lib, f'block_{i}') for i in range(len(entries))]
            for fn in functions:
                fn.argtypes = [C.POINTER(CPU)]
                fn.restype = None
            compare(functions, entries, halfwords, args.states)


if __name__ == '__main__':
    main()
