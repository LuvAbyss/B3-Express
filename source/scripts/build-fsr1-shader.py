#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Rebuild the embedded FSR 1 SPIR-V (src/nx/gpu/vk/fsr1_spv.h) from shaders/fsr1.comp.

Needs glslang: glslangValidator from .cache/tools/glslang-16.6.0/bin (an official KhronosGroup release, unpacked
there), from PATH, or given with --glslang. The checked-in header is the compiled shader, so normal and cross builds
need no shader compiler.
"""

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

GLSLANG_VERSION = '16.6.0'


def words(data):
    return struct.unpack(f'<{len(data) // 4}I', data)


def array(name, code):
    rows = [', '.join(f'0x{w:08x}u' for w in code[i : i + 9]) for i in range(0, len(code), 9)]
    return f'static const uint32_t {name}[] = {{\n    ' + ',\n    '.join(rows) + '};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--glslang', help='glslangValidator to use')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'src/nx/gpu/vk/shaders/fsr1.comp'
    tools = sorted((root / '.cache/tools').glob(f'glslang-{GLSLANG_VERSION}/bin/glslang*.exe'))
    glslang = args.glslang or (str(tools[0]) if tools else shutil.which('glslangValidator'))
    if not glslang:
        raise SystemExit(f'glslangValidator not found: unpack glslang {GLSLANG_VERSION} into .cache/tools')
    out = {}
    with tempfile.TemporaryDirectory() as temp:
        for name, defines in (('fsr1_easu_spv', ['-DPASS_EASU=1']), ('fsr1_rcas_spv', [])):
            binary = Path(temp) / f'{name}.spv'
            subprocess.run([glslang, '-V', '--target-env', 'vulkan1.3', *defines, '-o', str(binary), source.name],
                           cwd=source.parent, check=True)
            out[name] = words(binary.read_bytes())
    text = ('/* SPDX-License-Identifier: GPL-3.0-or-later\n   Copyright 2026 the ScarletNX contributors\n'
            '   Generated from shaders/fsr1.comp by scripts/build-fsr1-shader.py. AMD FSR 1, MIT, see shaders/ffx.\n'
            '   Do not edit. */\n#ifndef SNX_FSR1_SPV_H\n#define SNX_FSR1_SPV_H\n#include <stdint.h>\n'
            '#define FSR1_SPV_READY 1\n')
    text += ''.join(array(name, code) for name, code in out.items()) + '#endif\n'
    (source.parents[1] / 'fsr1_spv.h').write_text(text, newline='\n')


if __name__ == '__main__':
    main()
