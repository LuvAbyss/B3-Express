#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Disassembles game code at its runtime addresses, with symbol names.

    tools/disasm.py work/Game cf34240 cf343c0     # an address range (hex, as crash reports print them)
    tools/disasm.py work/Game NvDdkVicExecuteSync  # a whole function, by symbol name (substring match)

The game folder is the extracted dump (exefs/ + romfs/) or an NRO, laid out at the same bases the runtime uses. Its
modules are read by the recompiler's loader through `scarletnx --dump-modules` (build/scarletnx, or the program
SNX_SCARLETNX names), cached per game folder (tools/moduledump.py). A 32-bit program's code is shown as A32, or as
Thumb when the symbol it starts in is a Thumb function. Needs llvm-objcopy and llvm-objdump.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..')]
import moduledump  # noqa: E402

from symbols import Symbols  # noqa: E402


def disassemble(mods, syms, start, end, is64=True):
    mod = next((m for m in mods if m.base <= start < m.base + m.size), None)
    if mod is None:
        raise SystemExit('%x is not inside any module' % start)
    off = start - mod.base
    code = bytes(mod.image[off : off + (end - start)])
    triple = 'aarch64' if is64 else 'thumbv8a' if syms.is_thumb(start) else 'armv8a'
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'code.bin')
        elf = os.path.join(tmp, 'code.elf')
        with open(raw, 'wb') as f:
            f.write(code)
        subprocess.run(
            [
                'llvm-objcopy',
                '-I',
                'binary',
                '-O',
                'elf64-littleaarch64' if is64 else 'elf32-littlearm',
                '--rename-section',
                '.data=.text,alloc,load,readonly,code',
                raw,
                elf,
            ],
            check=True,
        )
        out = subprocess.run(
            ['llvm-objdump', '-d', '--no-show-raw-insn', '--triple=' + triple, '--adjust-vma=%#x' % start, elf],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    for line in out.splitlines():
        head, _, rest = line.partition(':')
        try:
            addr = int(head.strip(), 16)
        except ValueError:
            continue
        name = syms.exact(addr)
        if name:
            print('%s:' % name)
        # Name branch targets
        parts = rest.split()
        note = ''
        if parts and (parts[0] in ('bl', 'b', 'cbz', 'cbnz', 'tbz', 'tbnz') or parts[0].startswith('b.')):
            target = next((p for p in parts[1:] if p.startswith('0x')), None)
            if target:
                note = '  ; ' + syms.describe(int(target, 16))
        # llvm-objdump names addresses after the temporary file; the note above has the real name
        print('  %x:%s%s' % (addr, rest.split(' <_binary_')[0].rstrip(), note))


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    mods, is64 = moduledump.load_program(sys.argv[1])
    syms = Symbols(mods, is64)
    if len(sys.argv) == 4:
        start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
    else:
        start, end = syms.function_range(sys.argv[2])
    align = 3 if is64 or not syms.is_thumb(start) else 1
    disassemble(mods, syms, start & ~align, (end + align) & ~align, is64)


if __name__ == '__main__':
    main()
