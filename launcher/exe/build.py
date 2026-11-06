# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds B3-Express.exe (b3express.c with its icon and version details) with the llvm-mingw in source/deps/tools.
   launcher/python/python.exe -I launcher/exe/build.py [version]   -> launcher/exe/B3-Express.exe
The icon is drawn here too (the launcher's clock-ring: gold rings on its night colour, a violet hand), at 16 to 256 px."""
import math
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = HERE.parents[1] / 'source/deps/tools/llvm-mingw-20261006-ucrt-x86_64/bin'
VERSION = sys.argv[1] if len(sys.argv) > 1 else '0.2.0'


def png(size):
    """One square RGBA PNG of the icon, 4x4 supersampled"""
    S = 4
    night, gold, violet = (14, 11, 21), (226, 194, 126), (139, 95, 196)
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(S):
                for sx in range(S):
                    u = ((x + (sx + .5) / S) / size - .5) * 2  # -1..1
                    v = ((y + (sy + .5) / S) / size - .5) * 2
                    r = math.hypot(u, v)
                    if r > .96:
                        continue
                    c, a = night, 1.0
                    ang = math.atan2(v, u)
                    w = max(.022, 1.5 / size)  # lines stay a pixel wide at the small sizes
                    for ring, ticks in ((.86, 12), (.62, 0)):
                        if abs(r - ring) < w:
                            c = gold
                        if ticks and ring - .14 < r < ring and abs(((ang / (2 * math.pi) * ticks) % 1) - .5) > .5 - .045 * 12 / ticks:
                            c = gold
                    hand = (-.35 * math.pi)  # the hand, pointing up-right
                    hx, hy = math.cos(hand), math.sin(hand)
                    t = u * hx + v * hy
                    if 0 < t < .78 and abs(-u * hy + v * hx) < w * 1.6:
                        c = violet
                    if r < .09:
                        c = gold
                    for i in range(3):
                        acc[i] += c[i]
                    acc[3] += a
            n = S * S
            alpha = acc[3] / n
            row += bytes((round(acc[0] / max(acc[3], 1e-9)), round(acc[1] / max(acc[3], 1e-9)), round(acc[2] / max(acc[3], 1e-9)), round(255 * alpha)))
        rows.append(b'\0' + bytes(row))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))


def ico(sizes):
    images = [png(s) for s in sizes]
    head = struct.pack('<HHH', 0, 1, len(images))
    offset, entries = 6 + 16 * len(images), b''
    for s, data in zip(sizes, images):
        entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return head + entries + b''.join(images)


ICON = HERE.parent / 'ui' / 'b3express.ico'  # beside the page: the window takes it too (web_launcher.py)
ICON.write_bytes(ico([16, 24, 32, 48, 64, 128, 256]))
nums = [int(x) for x in VERSION.split('.')[:3]] + [0] * 3
(HERE / 'b3express.rc').write_text(f'''1 ICON "../ui/b3express.ico"
1 VERSIONINFO
FILEVERSION {nums[0]},{nums[1]},{nums[2]},0
PRODUCTVERSION {nums[0]},{nums[1]},{nums[2]},0
FILEOS 0x40004
FILETYPE 1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "B3-Express contributors"
      VALUE "FileDescription", "B3-Express launcher"
      VALUE "FileVersion", "{VERSION}"
      VALUE "InternalName", "B3-Express"
      VALUE "LegalCopyright", "GPL-3.0-or-later"
      VALUE "OriginalFilename", "B3-Express.exe"
      VALUE "ProductName", "B3-Express"
      VALUE "ProductVersion", "{VERSION}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
''', encoding='ascii')
run = lambda *a: subprocess.run([str(x) for x in a], check=True, cwd=HERE)
run(BIN / 'x86_64-w64-mingw32-windres.exe', 'b3express.rc', '-O', 'coff', '-o', 'b3express.res')
run(BIN / 'x86_64-w64-mingw32-clang.exe', '-O2', '-municode', '-mwindows', '-static', '-s', '-Wall', '-Wextra',
    'b3express.c', 'b3express.res', '-o', 'B3-Express.exe')
for f in ('b3express.res',):
    (HERE / f).unlink()
print('B3-Express.exe', (HERE / 'B3-Express.exe').stat().st_size, 'bytes, version', VERSION)
