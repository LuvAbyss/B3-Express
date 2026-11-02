#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""tools/shaderdec: the real spec generates the same files twice, and bad specs fail with their reason."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROW = 'FADD_R "FADD (reg)" "0101 1100 0101 1---"\n'
BAD = {
    'a bad character': ('FADD_R "FADD (reg)" "0101 1100 0101 1--x"\n', 'may only hold 0, 1, -'),
    'too few bits': ('FADD_R "FADD (reg)" "0101 1100 0101 1--"\n', 'fewer than 16 bits'),
    'too many bits': ('FADD_R "FADD (reg)" "0101 1100 0101 1---0"\n', 'more than 16 bits'),
    'no pattern': ('FADD_R "FADD (reg)"\n', 'expected the pattern'),
    'an open quote': ('FADD_R "FADD (reg)" "0101 1100 0101 1---\n', 'closing quote'),
    'a lower-case name': ('fadd "FADD (reg)" "0101 1100 0101 1---"\n', 'capital letter'),
    'the reserved name': ('NONE "none" "0101 1100 0101 1---"\n', 'NONE'),
    'a duplicate name': (ROW + 'FADD_R "FADD (other)" "0100 1100 0101 1---"\n', 'already a row at line 1'),
    'a duplicate display name': (ROW + 'FADD_C "FADD (reg)" "0100 1100 0101 1---"\n', 'is also line 1'),
    'an alias of nothing': ('FADD_R "FADD (reg)" "0101 1100 0101 1---" alias\n', 'names no opcode'),
    'an unknown attribute': ('FADD_R "FADD (reg)" "0101 1100 0101 1---" fast\n', "unknown attribute 'fast'"),
    'the same pattern twice': (ROW + 'FADD_C "FADD (c)" "0101 1100 0101 1---"\n', 'neither pattern narrows'),
    'patterns crossing': ('A "a" "0101 1100 0101 ----"\nB "b" "0101 1100 ---- 1---"\n', 'neither pattern narrows'),
    'no rows': ('# nothing\n', 'has no rows'),
}


def main():
    tool, spec = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='snx-shaderdec-') as directory:
        root = Path(directory)

        def run(spec_path, name):
            return subprocess.run(
                [tool, '--enum', str(root / f'{name}.h'), '--decoder', str(root / f'{name}.inc'), spec_path],
                text=True,
                capture_output=True,
                timeout=30,
            )

        failures = 0
        for name in ('first', 'second'):
            result = run(spec, name)
            if result.returncode:
                print(f'FAIL: the spec: {result.stderr.strip()}')
                return 1
        for extension in ('h', 'inc'):
            if (root / f'first.{extension}').read_bytes() != (root / f'second.{extension}').read_bytes():
                print(f'FAIL: two runs give different .{extension} files')
                failures += 1
        # a narrower pattern beside a broader one is fine, and comes first
        (root / 'nested.def').write_text('WIDE "wide" "0101 ---- ---- ----"\nNARROW "narrow" "0101 1100 ---- ----"\n')
        result = run(str(root / 'nested.def'), 'nested')
        if result.returncode:
            print(f'FAIL: nested patterns refused: {result.stderr.strip()}')
            failures += 1
        else:
            text = (root / 'nested.inc').read_text()
            if text.index('"narrow"') > text.index('"wide"'):
                print('FAIL: the narrower pattern is not tried first')
                failures += 1
        for what, (text, reason) in BAD.items():
            (root / 'bad.def').write_text(text)
            result = run(str(root / 'bad.def'), 'bad')
            if result.returncode != 1 or reason not in result.stderr:
                print(f'FAIL: {what}: exit {result.returncode}, {result.stderr.strip()!r}')
                failures += 1
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
