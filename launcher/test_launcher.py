"""Logic the launcher can get wrong without showing it: test_launcher.py. Runs on made-up data only, in a temporary folder."""
import os, sys, tempfile
from pathlib import Path
sys.path.insert(0, r'D:\Project-B3PC\B3-PC-Port-Portable\launcher')
os.environ['B3_GAME_DIR'] = tempfile.mkdtemp()
import bayonetta_launcher as L

INI = '; comment one\r\n[Display]\r\nfullscreen=0\r\n; about resolution\r\n[Graphics]\r\nresolution=1.6\r\nshadows=1\r\n'
t = L.ini_set(INI, 'resolution', '1.2', 'docked')
assert 'resolution=1.2\r\n' in t and '; about resolution\r\n' in t and t.count('resolution=') == 1
t = L.ini_set(t, 'fsr', '1.2', 'resolution')
assert 'resolution=1.2\r\nfsr=1.2\r\nshadows=1\r\n' in t, repr(t)
t2 = L.ini_set(t, 'fsr', '0', 'resolution')
assert t2.count('fsr=') == 1 and 'fsr=0\r\n' in t2
t3 = L.ini_set('a=1\nb=2\n', 'c', '3', 'a')
assert t3 == 'a=1\nc=3\nb=2\n', repr(t3)
assert L.ini_get(INI, 'resolution') == '1.6' and L.ini_get(INI, 'missing', 'x') == 'x'
try:
    L.ini_set('a=1\n', 'c', '3', 'nothere')
    raise SystemExit('a missing anchor must fail')
except ValueError:
    pass
# a key that only ends the same must not match
assert L.ini_get('xresolution=9\nresolution=2\n', 'resolution') == '2'
assert L.ini_set('xfsr=9\nfsr=1\n', 'fsr', '5', 'x') == 'xfsr=9\nfsr=5\n'

assert L.scale_settings(1.6, 1.0) == (1.6, 1.6)
assert L.scale_settings(1.2, 1.0) == (1.2, 1.2)
assert L.scale_settings(1.0, 1.0) == (1.0, 0)
assert L.scale_settings(1.2, 1.25) == (1.2, 0)
assert L.scale_settings(1.6, 1.25) == (1.6, 1.28)
assert L.scale_settings(1.6, None) == (1.6, 0)
assert L.quality_of(1.6, 1.6) == 'Smooth 60'
assert L.quality_of(1.6, 1.28) == 'Balanced'
assert L.quality_of(1.6, 0) == 'Sharp'
assert L.quality_of(1.0, 0) == 'Smooth 60'
assert L.quality_of(1.6, 2.0) == 'Custom'
for name, value in L.SCREENS:  # what is saved reads back as the same choice
    for q, scene in L.QUALITY:
        r, f = L.scale_settings(value, scene)
        assert L.screen_of(r) == name
        got = L.quality_of(r, f)
        assert got == q or value <= 1.25, (name, q, got)  # up to 1080p some choices are the same picture
assert L.screen_of(1.777).startswith('Custom')
assert L.best_screen(1920, 1080) == '1920 x 1080' and L.best_screen(2560, 1440) == '2560 x 1440'
assert L.best_screen(1366, 768) == '1600 x 900' and L.best_screen(3840, 2160) == '3840 x 2160'

folder = Path(os.environ['B3_GAME_DIR'])
log = folder / 'a.log'
log.write_text("[ 300.00] nvn-live: pace, a frame on average: before the first window 1 ms, windows 1 (vk_lock 0), after the last 1; held by the pacer 1; frames over 33.4 ms 6 of 600; schedule restarts 0; GPU busy at the first window 0\n"
               "[ 300.10] nvn-live: 600 frames: draws 1, drawn 1;\n[ 300.20] nvn-live: recording 1 ms a frame; worst frame 190.5 ms, over 50 ms 1;\n[ 360.00] Exit.\n")
s = L.session_summary(log)
assert s['over33'] == 1 and s['worst_ms'] == 190.5 and s['frames'] == 600 and abs(s['minutes'] - 6) < .01, s
assert 'Longest hitch: 190 ms' in L.summary_text(s) or 'Longest hitch: 191 ms' in L.summary_text(s)
log.write_text('nothing useful')
assert L.session_summary(log) is None and L.session_summary(folder / 'none.log') is None
assert 'No session' in L.summary_text(None)

cache = folder / 'shader_cache'
cache.mkdir(exist_ok=True)
assert not L.shaders_ready(cache, True)
(cache / 'gpu.txt').write_text('9b404000 1\n')
assert not L.shaders_ready(cache, True)
(cache / 'prepared.txt').write_text('9b404000 1\n')
assert L.shaders_ready(cache, True) and not L.shaders_ready(cache, False)
(cache / 'gpu.txt').write_text('9c000000 1\n')
assert not L.shaders_ready(cache, True)
assert L.prepare_progress(cache) is None
(cache / 'prepare_progress.txt').write_text('25 75\n')
assert L.prepare_progress(cache) == (25, 75)
assert L.scaled_mode('resolution=1.6\nfsr=0\n') and not L.scaled_mode('resolution=1\nfsr=0\n')
assert L.scaled_mode('resolution=1.2\nfsr=1.2\n') and L.scaled_mode('resolution=1\nfsr=1.2\n')

assert L.build_jobs(16) == 14 and L.build_jobs(2) == 1 and L.build_jobs(1) == 1 and L.build_jobs(None) == 1
assert L.block_progress('  compiled block 12/83: code_000, 0x8000000-0x805E000, 488 functions (47.1 s)') == (12, 83)
assert L.block_progress('Copying RomFS...') is None
a, b = folder / 'a.hints', folder / 'b.hints'
a.write_text('80016D0\n800138c\n\nnot-hex\n')
b.write_text('800138c\n9326220\n')
assert L.merged_hints(a, b, folder / 'none') == ['80016d0', '800138c', '9326220']
seed = L.merged_hints(L.SEED_HINTS)
assert len(seed) >= 351 and all(int(x, 16) % 4 == 0 for x in seed)  # the compiler drops an unaligned one


# the build, with a stand-in for the compiler: a fresh folder of its own, as the module's paths are fixed at import
class Fake:
    """What Popen gives build_code. Writes what the compiler would, or fails"""
    def __init__(self, args, fail):
        self.stdout = iter(['Recompiling\n', '  compiled block 1/2: x\n', '  compiled block 2/2: y\n'])
        self.args, self.fail = args, fail

    def wait(self):
        out = Path(self.args[self.args.index('--outdir') + 1]) / L.NAME
        self.__class__.hints = (out / (L.NAME + '.hints')).read_text().split()
        self.__class__.beside = sorted(p.name for p in Path(self.args[0]).parent.iterdir())
        self.__class__.env_portable = 'SNX_RECOMPILE_PORTABLE' in self.env
        if self.fail:
            return 3
        (out / L.CODE.name).write_text('new code')
        (out / L.STAMP.name).write_text('8\n')
        return 0


def fake_popen(fail):
    def make(args, **kw):
        Fake.env = kw['env']
        return Fake(args, fail)
    return make


L.LOGS = folder / 'logs'
(L.DATA / 'exefs').mkdir(parents=True)
(L.DATA / 'exefs' / 'main').write_text('x')
(L.DATA / 'romfs').mkdir()
(L.DATA / 'romfs' / 'big').write_text('x')
(folder / (L.NAME + '.exe')).write_text('exe')
(folder / 'libLLVM-23.dll').write_text('dll')
L.CODE.write_text('old code')  # beside the runtime it would be copied over the new one
(folder / (L.NAME + '.hints')).write_text('abc0\n')
os.environ['SNX_RECOMPILE_PORTABLE'] = '1'
assert not L.module_ready()  # no stamp: a stopped build doesn't count
real = L.subprocess.Popen
seen = []
try:
    L.subprocess.Popen = fake_popen(True)
    problem = L.build_code(seen.append)
    assert problem and 'code 3' in problem and L.CODE.read_text() == 'old code' and not L.STAMP.exists(), problem
    L.subprocess.Popen = fake_popen(False)
    seen.clear()
    assert L.build_code(seen.append) is None
finally:
    L.subprocess.Popen = real
    del os.environ['SNX_RECOMPILE_PORTABLE']
assert seen == [(1, 2), (2, 2)] and L.module_ready() and L.CODE.read_text() == 'new code' and not L.WORK.exists()
assert Fake.beside == ['libLLVM-23.dll', 'scarletnx.exe'] and not Fake.env_portable, (Fake.beside, Fake.env_portable)
assert Fake.hints[:len(seed)] == seed and Fake.hints[-1] == 'abc0'
assert (L.LOGS / 'build-code.log').read_text().startswith('Recompiling')

source = folder / 'extracted'
(source / 'exefs').mkdir(parents=True)
(source / 'exefs' / 'main').write_text('x')
(source / 'romfs' / 'a').mkdir(parents=True)
(source / 'romfs' / 'a' / 'f').write_text('yy')
assert L.has_game(source) and not L.has_game(folder)
(L.DATA / 'exefs' / 'main').unlink()
(L.DATA / 'exefs').rmdir()
(L.DATA / 'romfs' / 'big').unlink()
(L.DATA / 'romfs').rmdir()
percents = []
L.copy_game(source, percents.append)
assert percents[-1] == 100 and (L.DATA / 'romfs' / 'a' / 'f').read_text() == 'yy' and L.installed()
assert not (folder / '_copy').exists() and (source / 'exefs' / 'main').is_file()  # the player's folder stays
(L.DATA / 'romfs' / 'big').write_text('x')

L.ROOT = folder
assert L.version_problem() is None  # no list: only the folders can be checked
digest = L.hashlib.sha256(b'x').hexdigest()
(folder / 'game-files.txt').write_text(f'1 {digest} exefs/main\n1 {digest} romfs/big\n')
assert L.version_problem() is None
(L.DATA / 'exefs' / 'main').write_text('y')
assert 'Different: exefs/main' in L.version_problem()
(L.DATA / 'romfs' / 'big').unlink()
assert 'Missing: romfs/big' in L.version_problem()

# what the review found
L.ROOT = folder / 'no-list'
err = L.shutil.Error([('a', 'b', 'first reason'), ('c', 'd', 'second')])
assert L.error_text(err) == 'first reason' and L.error_text(OSError('plain')) == 'plain'

L.shutil.rmtree(L.DATA / 'exefs')
L.shutil.rmtree(L.DATA / 'romfs')
locked = source / 'exefs' / 'main'
os.chmod(locked, 0o444)  # a read-only dump: the attribute must not come along
L.copy_game(source, lambda p: None)
os.chmod(locked, 0o666)
assert os.access(L.DATA / 'exefs' / 'main', os.W_OK)

L.shutil.rmtree(L.DATA / 'exefs')
L.shutil.rmtree(L.DATA / 'romfs')
real_replace = os.replace


def refuse_romfs(a, b):
    if Path(a).name == 'romfs':
        raise PermissionError('held by a scanner')
    real_replace(a, b)


os.replace = refuse_romfs
try:
    L.copy_game(source, lambda p: None)
    raise SystemExit('the refused move must fail')
except PermissionError:
    pass
finally:
    os.replace = real_replace
assert not (L.DATA / 'exefs').exists() and not (L.DATA / 'romfs').exists()  # never half a game
L.shutil.rmtree(folder / '_copy')
L.copy_game(source, lambda p: None)
assert L.installed()


class Page:
    events = L.queue.Queue()
    builder = 'old'


real_build = L.build_code


def raises(*_):
    raise ValueError('something unforeseen')


L.build_code = raises
L.Launcher.build_run(Page)
L.build_code = real_build
kinds = []
while not Page.events.empty():
    kinds.append(Page.events.get())
assert kinds[-1][0] == 'installed' and 'something unforeseen' in kinds[-1][1] and Page.builder is None, kinds

# a compile whose work folder can't be removed afterwards is still a finished game
L.subprocess.Popen = fake_popen(False)
real_rmtree = L.shutil.rmtree


def stubborn(path, ignore_errors=False):
    if ignore_errors:
        return
    if Path(path).exists():
        raise PermissionError('locked')


try:
    L.shutil.rmtree = stubborn
    assert L.build_code(lambda got: None) is None and L.module_ready() and L.WORK.exists()
    assert 'locked' in L.build_code(lambda got: None)  # can't clear the old one: said, not a dead thread
finally:
    L.subprocess.Popen = real
    L.shutil.rmtree = real_rmtree

# tasklist answers in the PC's code page: German cp850 "ausgeführt" is not UTF-8 (issue 1: Install did nothing)
class Answer:
    def __init__(self, stdout):
        self.stdout = stdout


real_run = L.subprocess.run
try:
    L.subprocess.run = lambda *a, **k: Answer(b'INFORMATION: Es werden keine Tasks mit den angegebenen Kriterien ausgef\x81hrt.\r\n')
    assert L.game_running() is False
    L.subprocess.run = lambda *a, **k: Answer(b'Bayo3_120.exe   1234 Console   1   2.000.000 K\r\n')
    assert L.game_running() is True
    L.subprocess.run = lambda *a, **k: Answer(None)  # what a decode failure in run's reader thread leaves
    assert L.game_running() is False
    L.subprocess.run = lambda *a, **k: Answer(b'NVIDIA GeForce RTX 3080, 10240, 621.01\r\n')
    assert L.gpu_info() == 'NVIDIA GeForce RTX 3080, 10 GB, driver 621.01', L.gpu_info()
    L.subprocess.run = lambda *a, **k: Answer(None)
    assert 'No NVIDIA' in L.gpu_info()
finally:
    L.subprocess.run = real_run
L.LOGS = Path(tempfile.mkdtemp())
try:
    raise KeyError('x')
except KeyError:
    shown = L.log_error(*sys.exc_info())
assert 'KeyError' in shown and 'launcher-errors.log' in shown
assert 'KeyError' in (L.LOGS / 'launcher-errors.log').read_text(encoding='utf8')
print('launcher logic: all passed')
