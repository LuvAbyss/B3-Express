"""Build the player's zip from a list of what goes in: nothing of the game, no compiled game code, no saves. Run from the project root."""
import hashlib
import shutil
import zipfile
from pathlib import Path

VERSION = '0.1.7'
root = Path.cwd()
name = f'B3-Express-{VERSION}-windows-x64'
out = root / 'release' / name
exe = root / 'diagnostics/release-0.1/Bayo3_120-0.1.7-release.exe'  # 0.1.6 + present thread on even slots + compute barriers outside the flow loop (issue 2), built from fbf7bf6
assert hashlib.sha256(exe.read_bytes()).hexdigest().startswith('30e053b5')
assert not (root / 'release' / (name + '.zip')).exists(), 'this version is already packaged: raise VERSION'
if out.exists():
    shutil.rmtree(out)
out.mkdir(parents=True)
game = out / 'Bayo3_120'
game.mkdir()
shutil.copyfile(exe, game / 'Bayo3_120.exe')
for dll in ('libLLVM-23', 'libc++', 'libffi-8', 'libiconv-2', 'libxml2-16', 'libzstd', 'zlib1'):
    shutil.copyfile(root / 'output/Bayo3_120' / (dll + '.dll'), game / (dll + '.dll'))
for f in ('game-files.txt', 'LICENSE'):
    shutil.copyfile(root / f, out / f)
# the launcher's starter, built with this version in its details (launcher/exe/build.py)
import subprocess, sys
subprocess.run([sys.executable, '-I', str(root / 'launcher/exe/build.py'), VERSION], check=True)
shutil.copyfile(root / 'launcher/exe/B3-Express.exe', out / 'B3-Express.exe')
shutil.copytree(root / 'licenses', out / 'licenses')
shutil.copyfile(root / 'docs/INSTALL.md', out / 'INSTALL.txt')
launcher = out / 'launcher'
launcher.mkdir()
for f in ('bayonetta_launcher.py', 'web_launcher.py', 'theme.py', 'code-hints.txt', 'code-tablecalls.txt', 'README.md', 'ATTRIBUTION.md', 'LICENSE'):
    shutil.copyfile(root / 'launcher' / f, launcher / f)
shutil.copytree(root / 'launcher/ui', launcher / 'ui', ignore=shutil.ignore_patterns('prototype.html'))
(out / 'VERSION').write_text(VERSION + '
', encoding='ascii')  # the launcher's update check reads it
shutil.copytree(root / 'launcher/python', launcher / 'python', ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))

BANNED = ('_code.dll', '.abi', '.hints', '.ini', '.log', '.nso', '.nca', '.nsp', '.xci', '.keys', '.ppm', '.snxs', '.bnk', '.webm')
names = [p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file()]
low = [n.lower() for n in names]
bad = [n for n in low if not n.startswith('launcher/python/') and (n.endswith(BANNED) or 'save' in n.split('/') or 'romfs' in n or 'exefs' in n or 'hactool' in n)]
assert not bad, bad[:10]
target = root / 'release' / (name + '.zip')
target.unlink(missing_ok=True)
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in sorted(out.rglob('*')):
        if p.is_file():
            z.write(p, Path(name) / p.relative_to(out))
print(target.name, len(names), 'files', f'{target.stat().st_size / 1048576:.1f} MB', hashlib.sha256(target.read_bytes()).hexdigest())
