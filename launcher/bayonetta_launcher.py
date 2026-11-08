# SPDX-License-Identifier: GPL-2.0-or-later
"""Bayonetta 3 launcher. Settings live in the game's INI, so this and the game's own menu always agree."""
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from theme import LauncherTheme, BG, PANEL, CARD, TEXT, MUTED, GOLD

ROOT = Path(__file__).resolve().parent.parent
NAME = 'Bayo3_120'
HIDDEN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
ENV = {'SNX_NVN_FRONTEND': '2', 'SNX_NVN_RELEASE_ONLY': '1'}  # the fast renderer: without these it is the old one
FREE_GB = 20  # the game's files, and the compile's room
KEEP_LOGS = 8
PREPARE_MIN = 1000  # stages known before a percentage means anything
BUILD_FREE_GB = 3  # what compiling the game code needs while it runs
CODE_LEVEL = '1'  # O2 gave nothing over O1 in the game
# name, the 3D scene's size against the console's. None draws at the screen's size
QUALITY = (('Smooth 60', 1.0), ('Balanced', 1.25), ('Sharp', None))
QUALITY_HELP = {'Smooth 60': 'Recommended. The 3D is drawn at the console\'s size and upscaled with FSR 1. The HUD stays sharp.',
                'Balanced': 'A sharper picture. Busy scenes can drop below 60 FPS.',
                'Sharp': 'Native resolution with no upscaling. The most demanding, and it can drop well below 60 FPS.'}
SCREENS = (('1600 x 900', 1.0), ('1920 x 1080', 1.2), ('2560 x 1440', 1.6), ('3840 x 2160', 2.4))


def game_dir():
    forced = os.environ.get('B3_GAME_DIR')
    if forced:
        return Path(forced)
    for path in (ROOT / 'output' / NAME, ROOT / NAME):
        if (path / (NAME + '.exe')).is_file():
            return path
    return ROOT / NAME


GAME = game_dir()
INI = GAME / (NAME + '.ini')
DATA = GAME / (NAME + '_data')
CODE = GAME / (NAME + '_code.dll')
STAMP = GAME / (NAME + '.abi')
PATCHES = GAME / (NAME + '.patches')  # the code patches the code was compiled with, as the game checks them
CODEMODS = GAME / (NAME + '.codemods')  # the same, as the launcher checks them (code_mods_print)
WORK = GAME / '_build'
LOGS = ROOT / 'logs'
SEED_HINTS = Path(__file__).resolve().parent / 'code-hints.txt'  # addresses only: code past runs found that the compiler can't
TABLE_CALLS = Path(__file__).resolve().parent / 'code-tablecalls.txt'  # addresses of the functions the runtime replaces, for cutscene_fps=60
HOOK_POINTS = Path(__file__).resolve().parent / 'code-insnhooks.txt'  # instructions the runtime runs a hook before (widescreen)


def ini_get(text, key, default=''):
    m = re.search(rf'^{re.escape(key)}=(.*?)\r?$', text, re.M)
    return m.group(1).strip() if m else default


def ini_set(text, key, value, after):
    """key=value in place, keeping every comment. A key the file lacks goes on the line after the key named by after"""
    pattern = rf'^{re.escape(key)}=.*?(?=\r?$)'
    if re.search(pattern, text, re.M):
        return re.sub(pattern, lambda m: f'{key}={value}', text, count=1, flags=re.M)
    new, n = re.subn(rf'^({re.escape(after)}=.*?)(\r?)$', lambda m: f'{m.group(1)}{m.group(2)}\n{key}={value}{m.group(2)}',
                     text, count=1, flags=re.M)
    if not n:
        raise ValueError(f'{after} is not in the settings file')
    return new


def scale_settings(resolution, scene):
    """resolution and fsr for a screen and a quality: FSR 1 only where the scene is smaller than the screen"""
    if scene is None or scene >= resolution:
        return resolution, 0
    return resolution, round(resolution / scene, 3)


def quality_of(resolution, fsr):
    scene = resolution / fsr if fsr > 1 else resolution
    for name, value in QUALITY:
        if value is not None and abs(scene - value) < 0.01:
            return name
    return 'Sharp' if fsr <= 1 else 'Custom'


def screen_of(resolution):
    for name, value in SCREENS:
        if abs(value - resolution) < 0.01:
            return name
    return f'Custom ({resolution:g}x)'


def best_screen(width, height):
    fit = [name for name, value in SCREENS if round(1600 * value) <= width and round(900 * value) <= height]
    return fit[-1] if fit else SCREENS[0][0]


def to_float(text, default):
    try:
        return float(text)
    except ValueError:
        return default


def error_text(e):
    """An OSError for the player. copytree's own is a list of every file that failed: the first says enough"""
    return str(e.args[0][0][2]) if isinstance(e, shutil.Error) and e.args and isinstance(e.args[0], list) and e.args[0] else str(e)


def has_game(data):
    return (data / 'exefs' / 'main').is_file() and (data / 'romfs').is_dir() and any((data / 'romfs').iterdir())


def installed():
    return has_game(DATA)


def copy_game(source, report):
    """exefs and romfs of source into the game's folder, whole: a copy that stopped is cleared by the next one.
    copyfile, as a read-only dump's attribute must not come along: it stops Windows deleting the work folders"""
    temp = GAME / '_copy'
    if temp.exists():
        shutil.rmtree(temp)
    total = sum(p.stat().st_size for name in ('exefs', 'romfs') for p in (source / name).rglob('*') if p.is_file())
    done = [0, -1]

    def copy(src, dst):
        shutil.copyfile(src, dst)
        done[0] += os.path.getsize(src)
        percent = 100 * done[0] // max(total, 1)
        if percent != done[1]:
            done[1] = percent
            report(percent)
    for name in ('exefs', 'romfs'):
        shutil.copytree(source / name, temp / name, copy_function=copy)
    DATA.mkdir(exist_ok=True)
    try:
        for name in ('exefs', 'romfs'):
            os.replace(temp / name, DATA / name)
    except OSError:
        if (DATA / 'exefs').exists() and not (temp / 'exefs').exists():  # never half a game
            os.replace(DATA / 'exefs', temp / 'exefs')
        raise
    shutil.rmtree(temp, ignore_errors=True)


def module_ready():
    """The game code is built, with the code mods on now. The stamp goes in last, so a build that stopped never
    counts"""
    return CODE.is_file() and STAMP.is_file() and not code_mods_changed()


def build_jobs(cpus):
    """All threads but two, so the PC stays usable"""
    return max(1, (cpus or 1) - 2)


def merged_hints(*paths):
    """The addresses of these hint files, each once and in order. A missing file adds nothing"""
    seen = {}
    for path in paths:
        try:
            lines = path.read_text(encoding='ascii', errors='ignore').splitlines()
        except OSError:
            continue
        for line in lines:
            line = line.strip().lower()
            if re.fullmatch(r'[0-9a-f]{1,16}', line):
                seen.setdefault(line)
    return list(seen)


def block_progress(line):
    """(done, total) from a compiler line, None for any other line"""
    m = re.search(r'compiled block (\d+)/(\d+)', line)
    return (int(m.group(1)), int(m.group(2))) if m else None


def build_command(runtime, source, out, repo, jobs):
    return [str(runtime), '-r', str(source), '--name', NAME, '--outdir', str(out), '--repo', str(repo),
            '--runtime', str(runtime), '--jobs', str(jobs), '-O', CODE_LEVEL]


def build_code(report, started=lambda process: None):
    """Compile the game's code for this PC into GAME. None when it worked, else what went wrong.

    Done in a work folder holding only ExeFS, so the 15 GB RomFS isn't copied, and with a runtime folder holding no
    game code: the compiler deploys every DLL beside its runtime over its output, which would replace the new code
    with an old one."""
    runtime = WORK / 'runtime' / 'scarletnx.exe'
    source, out, repo = WORK / 'input', WORK / 'out', WORK / 'repo'
    process = None
    try:
        if WORK.exists():
            shutil.rmtree(WORK)
        runtime.parent.mkdir(parents=True)
        for path in [GAME / (NAME + '.exe')] + [p for p in GAME.glob('*.dll') if not p.name.endswith('_code.dll')]:
            target = runtime if path.suffix == '.exe' else runtime.parent / path.name
            try:
                os.link(path, target)
            except OSError:
                shutil.copy2(path, target)
        shutil.copytree(DATA / 'exefs', source / 'exefs', copy_function=shutil.copyfile)
        # the code mods on, laid out as in the game's data folder: the compiler applies them as the game does
        patches = code_mods()
        built_print = code_mods_print(patches)
        for name, f, data in patches:
            (source / 'mods' / name / 'exefs').mkdir(parents=True, exist_ok=True)
            (source / 'mods' / name / 'exefs' / f).write_bytes(data)
        if patches:
            names = list(dict.fromkeys(n for n, _, _ in patches))
            (source / 'mods' / 'mods.txt').write_text(''.join(n + '\n' for n in names), encoding='utf8')
        (out / NAME).mkdir(parents=True)
        repo.mkdir()
        hints = merged_hints(SEED_HINTS, GAME / (NAME + '.hints'))
        (out / NAME / (NAME + '.hints')).write_text(''.join(x + '\n' for x in hints), encoding='ascii')
        if TABLE_CALLS.is_file():
            shutil.copyfile(TABLE_CALLS, out / NAME / (NAME + '.tablecalls'))
        if HOOK_POINTS.is_file():
            shutil.copyfile(HOOK_POINTS, out / NAME / (NAME + '.insnhooks'))
        env = {k: v for k, v in os.environ.items() if k != 'SNX_RECOMPILE_PORTABLE'}  # built for this PC's CPU
        LOGS.mkdir(exist_ok=True)
        tail = []
        with (LOGS / 'build-code.log').open('w', encoding='utf8') as log:
            process = subprocess.Popen(build_command(runtime, source, out, repo, build_jobs(os.cpu_count())), cwd=WORK,
                                       env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, errors='replace', creationflags=HIDDEN)
            started(process)
            for line in process.stdout:
                log.write(line)
                log.flush()
                tail = (tail + [line.rstrip()])[-3:]
                got = block_progress(line)
                if got:
                    report(got)
            code = process.wait()
        built, stamp, patched = out / NAME / CODE.name, out / NAME / STAMP.name, out / NAME / PATCHES.name
        if code or not built.is_file() or not stamp.is_file():
            return f'The compiler stopped (code {code}).\n' + '\n'.join(tail)
        if patches and not patched.is_file():
            return 'This version of the game program can\'t apply code mods. Turn them off, or update B3-Express.'
        if STAMP.exists():
            STAMP.unlink()
        os.replace(built, CODE)
        if patched.is_file():
            os.replace(patched, PATCHES)
        elif PATCHES.exists():
            PATCHES.unlink()
        CODEMODS.write_text(built_print + '\n', encoding='ascii')
        os.replace(stamp, STAMP)
    except OSError as e:
        if process and process.poll() is None:
            process.kill()
        return error_text(e)
    shutil.rmtree(WORK, ignore_errors=True)  # the game is ready by now: what is left goes with the next build
    return None


# the game's dither values (gpu/vk/dither.c), as the Graphics page names them
FADES = (('Game', '0'), ('Off', '1'), ('Even pattern', '2'), ('Transparent', '3'))
FADE_HELP = {'Game': 'Fading objects use the game\'s checkerboard, as on the Switch.',
             'Off': 'No checkerboard. Objects that would fade stay solid, so one near the camera can block the view.',
             'Even pattern': 'The checkerboard at your screen\'s own pixels: fine and even on Balanced and Sharp. '
                             'Smooth 60 looks the same as Game.',
             'Transparent': 'Recommended. Fading objects are see-through instead of checkered. Things behind them can be hidden '
                            'or drawn in the wrong order.'}


def prep_mode(text):
    """What the game's prepared shaders are for, as it writes it (nvn_res.c prep_mode): the scaling, plus 2 x dither"""
    try:
        dither = max(0, min(3, int(ini_get(text, 'dither', '3'))))
    except ValueError:
        dither = 0
    return int(scaled_mode(text)) + 2 * dither


# the game's button_prompts values, as the Controller page names them
PROMPTS = (('Auto', '0'), ('Switch', '1'), ('PlayStation', '2'), ('Xbox', '3'))


def check_game(data=DATA):
    """(problems, files checked). game-files.txt comes with the launcher; without it only the two folders can be checked"""
    listing = ROOT / 'game-files.txt'
    if not listing.is_file():
        return ([] if has_game(data) else ['The game files are missing.']), 0
    problems, count = [], 0
    for line in listing.read_text(encoding='utf8').splitlines():
        if not line:
            continue
        size, digest, name = line.split(' ', 2)
        path = data / name
        count += 1
        if not path.is_file():
            problems.append(f'Missing: {name}')
        elif path.stat().st_size != int(size):
            problems.append(f'Wrong size: {name}')
        elif name.startswith('exefs/') and hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            problems.append(f'Different: {name}')
    return problems, count


def version_problem(data=DATA):
    """What to tell the player when these aren't the files the game code must be built from, else None"""
    problems = check_game(data)[0]
    return ('This isn\'t version 1.2.0: the base game with the 1.2.0 update applied is needed.\n'
            + '\n'.join(problems[:5])) if problems else None


def session_summary(path):
    """What the log says about smoothness, in plain numbers. None when there is nothing to read"""
    try:
        text = path.read_text(encoding='utf8', errors='replace')
    except OSError:
        return None
    frames = re.findall(r'nvn-live: (\d+) frames: draws', text)
    pace = re.findall(r'frames over 33\.4 ms (\d+) of (\d+)', text)
    worst = [float(x) for x in re.findall(r'worst frame ([\d.]+) ms', text)]
    times = re.findall(r'^\[\s*([\d.]+)\]', text, re.M)
    if not frames or not pace:
        return None
    over = sum(int(a) for a, _ in pace)
    total = sum(int(b) for _, b in pace)
    return {'minutes': float(times[-1]) / 60, 'frames': int(frames[-1]), 'over33': 100 * over / max(total, 1),
            'worst_ms': max(worst) if worst else 0}


def summary_text(s):
    if not s:
        return 'No session to report yet.'
    return (f"{s['minutes']:.0f} min played. {s['over33']:.1f}% of frames took over 33 ms. "
            f"Longest hitch: {s['worst_ms']:,.0f} ms.")


def keep_last_log():
    """The game overwrites its log at each launch: the last one goes to logs/ first, the newest few kept"""
    log = GAME / (NAME + '.log')
    if not log.is_file():
        return
    LOGS.mkdir(exist_ok=True)
    stamp = int(log.stat().st_mtime)
    shutil.copy2(log, LOGS / f'session-{stamp}.log')
    for old in sorted(LOGS.glob('session-*.log'))[:-KEEP_LOGS]:
        old.unlink()


def game_running(name=NAME):
    """Read as bytes: tasklist answers in the PC's language and code page, never UTF-8, and a decode error there once
    made Install do nothing on a non-English Windows. Only the ASCII program name matters"""
    try:
        out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}.exe', '/NH'], capture_output=True,
                             creationflags=HIDDEN, timeout=10).stdout or b''
    except (OSError, subprocess.SubprocessError):
        return False
    return (name + '.exe').lower().encode('ascii') in out.lower()


def shaders_ready(cache, scaled):
    """A preparation pass finished for this scale mode, and the last run's driver is the one it was for"""
    try:
        done = (cache / 'prepared.txt').read_text().split()
        now = (cache / 'gpu.txt').read_text().split()
    except OSError:
        return False
    return len(done) == 2 and len(now) == 2 and done[0] == now[0] and done[1] == str(int(scaled))


def prepare_progress(cache):
    """Stages done and left, or None before the first report"""
    try:
        done, left = (cache / 'prepare_progress.txt').read_text().split()[:2]
        return int(done), int(left)
    except (OSError, ValueError):
        return None


def can_prepare():
    """The installed game has the preparation pass: an older build would just start and never report"""
    try:
        return b'SNX_NVN_PREP_RUN' in (GAME / (NAME + '.exe')).read_bytes()
    except OSError:
        return False


def scaled_mode(text):
    """As the game's rp_scaling: drawn above the console's size, through FSR, or wider than 16:9 (aspect_ratio). Shaders
    differ between the two"""
    aspect = to_float(ini_get(text, 'aspect_ratio', '0'), 0.0)
    return to_float(ini_get(text, 'resolution', '1'), 1.0) != 1.0 or to_float(ini_get(text, 'fsr', '0'), 0.0) > 1.0 \
        or (16 / 9 + 0.001 < aspect <= 4.0)


def launch_command():
    return [str(GAME / (NAME + '.exe')), '--game=' + str(GAME)]


def gpu_info():
    try:
        out = subprocess.run(['nvidia-smi', '--query-gpu=name,memory.total,driver_version', '--format=csv,noheader,nounits'],
                             capture_output=True, creationflags=HIDDEN, timeout=10).stdout
        out = (out or b'').decode('utf8', errors='replace')
        name, memory, driver = [x.strip() for x in out.splitlines()[0].split(',')]
        return f'{name}, {int(memory) / 1024:.0f} GB, driver {driver}'
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return 'No NVIDIA graphics card found. This build is only tested on NVIDIA.'


def log_error(kind, value, trace):
    """Appends the error to logs/launcher-errors.log and returns what to show the player"""
    import traceback
    text = ''.join(traceback.format_exception(kind, value, trace))
    try:
        LOGS.mkdir(exist_ok=True)
        with (LOGS / 'launcher-errors.log').open('a', encoding='utf8') as log:
            log.write(time.strftime('[%Y-%m-%d %H:%M:%S]\n') + text + '\n')
        where = f'Details are in {LOGS / "launcher-errors.log"}.'
    except OSError:
        where = text
    return f'The launcher hit an error: {value!r}\n{where}\nPlease attach that file to a GitHub issue.'


# ---- what the new launcher window (web_launcher.py) asks for, kept here with the rest of the logic and its tests

SAVE = DATA / 'save'
BACKUPS = ROOT / 'backups'
REPORTS = ROOT / 'reports'
REPO = 'LuvAbyss/B3-Express'
FLAGS = (('fullscreen', '1'), ('shadows', '1'), ('mute_unfocused', '1'), ('gamepad_swap_ab', '0'), ('model_lod', '0'),
         ('film_grain', '1'))
ASPECTS = ('0', '1.6', '2.3333', '3.5556')  # aspect_ratio the Options page offers: 16:9 as made, 16:10, 21:9, 32:9
LOD_BIASES = ('0', '-0.5', '-1')  # texture_lod_bias the Options page offers: the game's, sharper, sharpest


def launcher_version():
    """The package's version (VERSION, written by make-release.py), 'dev' when run from the repository"""
    try:
        return (ROOT / 'VERSION').read_text(encoding='ascii').strip() or 'dev'
    except OSError:
        return 'dev'


def options_read(text):
    """Every option the launcher shows, as the settings file has it, with the names its pages use"""
    res = to_float(ini_get(text, 'resolution', '1'), 1)
    fsr = to_float(ini_get(text, 'fsr', '0'), 0)
    return {'screen': screen_of(res), 'quality': quality_of(res, fsr),
            **{key: ini_get(text, key, default) == '1' for key, default in FLAGS},
            'anisotropy': ini_get(text, 'anisotropy', '0'), 'lod_bias': ini_get(text, 'texture_lod_bias', '0'),
            'aspect': ini_get(text, 'aspect_ratio', '0'),
            'fade': dict((v, n) for n, v in FADES).get(ini_get(text, 'dither', '3'), FADES[3][0]),
            'prompts': dict((v, n) for n, v in PROMPTS).get(ini_get(text, 'button_prompts', '0'), PROMPTS[0][0]),
            'deadzone': ini_get(text, 'gamepad_deadzone', '20'), 'rumble': ini_get(text, 'gamepad_rumble', '100')}


def options_apply(text, o):
    """The settings file's text with the options o set, every comment kept. A screen or quality the pages don't name
    (set by hand) stays as it is"""
    res, scene = dict(SCREENS).get(o['screen']), dict(QUALITY).get(o['quality'], 1.0)
    if res is not None and o['quality'] in dict(QUALITY):
        resolution, fsr = scale_settings(res, scene)
        text = ini_set(text, 'resolution', f'{resolution:g}', 'docked')
        text = ini_set(text, 'fsr', f'{fsr:g}', 'resolution')
    for key, _ in FLAGS:
        text = ini_set(text, key, '1' if o[key] else '0', 'fullscreen')
    if o['anisotropy'] not in ('0', '2', '4', '8', '16') or o['deadzone'] not in ('10', '15', '20', '25', '30') \
            or o['rumble'] not in ('0', '25', '50', '75', '100'):
        raise ValueError('an option has a value the launcher doesn\'t offer')
    if o['lod_bias'] not in LOD_BIASES or o['aspect'] not in ASPECTS:
        raise ValueError('an option has a value the launcher doesn\'t offer')
    text = ini_set(text, 'anisotropy', o['anisotropy'], 'vsync')
    text = ini_set(text, 'texture_lod_bias', o['lod_bias'], 'anisotropy')
    text = ini_set(text, 'aspect_ratio', o['aspect'], 'resolution')
    text = ini_set(text, 'dither', dict(FADES)[o['fade']], 'resolution')
    text = ini_set(text, 'gamepad_deadzone', o['deadzone'], 'gamepad_swap_ab')
    text = ini_set(text, 'gamepad_rumble', o['rumble'], 'gamepad_deadzone')
    return ini_set(text, 'button_prompts', dict(PROMPTS)[o['prompts']], 'gamepad_swap_ab')


def write_ini(text):
    """The settings file replaced whole, the first version kept beside it as .before-launcher"""
    backup = INI.with_suffix('.ini.before-launcher')
    if INI.is_file() and not backup.exists():
        shutil.copy2(INI, backup)
    temporary = INI.with_suffix('.tmp')
    temporary.write_text(text, encoding='utf8', newline='')
    temporary.replace(INI)


def backup_list():
    """The save backups, newest first: (file name, time, bytes, note)"""
    out = []
    for path in BACKUPS.glob('save-*.zip') if BACKUPS.is_dir() else []:
        note = ''
        try:
            import zipfile
            with zipfile.ZipFile(path) as z:
                note = z.comment.decode('utf8', errors='replace')
        except (OSError, ValueError):
            continue
        out.append((path.name, path.stat().st_mtime, path.stat().st_size, note))
    return sorted(out, key=lambda b: b[1], reverse=True)


def backup_save(note='made by hand'):
    """The game's save folder zipped into backups/; the file's name, or None when there is no save to back up"""
    import zipfile
    files = [p for p in SAVE.rglob('*') if p.is_file()] if SAVE.is_dir() else []
    if not files:
        return None
    BACKUPS.mkdir(exist_ok=True)
    name = time.strftime('save-%Y%m%d-%H%M%S.zip')
    n = 1
    while (BACKUPS / name).exists():
        n += 1
        name = time.strftime(f'save-%Y%m%d-%H%M%S-{n}.zip')
    temporary = BACKUPS / (name + '.part')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as z:
        z.comment = note.encode('utf8')[:200]
        for p in files:
            z.write(p, p.relative_to(SAVE).as_posix())
    temporary.replace(BACKUPS / name)
    return name


def restore_save(name):
    """The backup name put in place of the save folder, the current save backed up first. None when it worked"""
    import zipfile
    path = BACKUPS / name
    if path.parent != BACKUPS or not re.fullmatch(r'save-[\d-]+\.zip', name) or not path.is_file():
        return 'That backup isn\'t there any more.'
    if game_running():
        return 'Close the game first.'
    backup_save('before a restore')
    temporary = DATA / 'save.restoring'
    shutil.rmtree(temporary, ignore_errors=True)
    with zipfile.ZipFile(path) as z:
        for member in z.namelist():  # nothing may land outside the save folder
            target = (temporary / member).resolve()
            if temporary.resolve() not in target.parents:
                return 'That backup holds files outside a save folder: it was not restored.'
        z.extractall(temporary)
    old = DATA / 'save.old'
    shutil.rmtree(old, ignore_errors=True)
    if SAVE.exists():
        os.replace(SAVE, old)
    os.replace(temporary, SAVE)
    shutil.rmtree(old, ignore_errors=True)
    return None


MOD_NAME = re.compile(r'[^/\\:#\r\n]+')  # one folder of mods/, as the game reads mods.txt (fs.c mod_name_valid)


def mods_dir():
    """mods/ of the game's data folder: each mod a folder holding romfs/ (yuzu's and Ryujinx's layout)"""
    return DATA / 'mods'


def mod_folders():
    root = mods_dir()
    return sorted(p.name for p in root.iterdir() if p.is_dir() and not p.name.endswith('.part')) if root.is_dir() else []


def mods_on():
    """The mods turned on, in mods.txt's order, read as the game reads it: '#' starts a comment, a name that isn't a
    folder is skipped, a name given twice counts once"""
    folders, on = set(mod_folders()), []
    path = mods_dir() / 'mods.txt'
    for line in path.read_text(encoding='utf8', errors='replace').splitlines() if path.is_file() else []:
        name = line.split('#', 1)[0].strip()
        if name in folders and name not in on:
            on.append(name)
    return on


def _mod_files(name):
    """The files of a mod's romfs/, as paths relative to it with '/'"""
    romfs = mods_dir() / name / 'romfs'
    return {p.relative_to(romfs).as_posix(): p.stat().st_size for p in romfs.rglob('*') if p.is_file()} \
        if romfs.is_dir() else {}


class _GameNames:
    """Which paths the game's romfs/ has, with their capitals exactly: the game's names are case-sensitive, Windows'
    aren't, so a mod's Ui/x.dat would be added beside ui/x.dat and never read. One listing per folder, kept"""
    def __init__(self):
        self.listed = {}

    def _names(self, rel):
        if rel not in self.listed:
            try:
                self.listed[rel] = set(os.listdir(DATA / 'romfs' / rel))
            except OSError:
                self.listed[rel] = set()
        return self.listed[rel]

    def find(self, path):
        """'same' when the game has this file, 'case' when only with other capitals, None when it hasn't"""
        parts, rel, exact = path.split('/'), '', True
        for part in parts:
            names = self._names(rel)
            if part not in names:
                lower = {n.lower(): n for n in names}
                if part.lower() not in lower:
                    return None
                part, exact = lower[part.lower()], False
            rel = f'{rel}/{part}' if rel else part
        return 'same' if exact else 'case'


PATCH_FILE = re.compile(r'.+\.(ips|pchtxt)', re.I)  # code patches the game reads (source/src/nx/loader/patch.h)


def _mod_patches(name):
    """A mod's code patches: the .ips and .pchtxt files of its exefs/, by name (the order the game applies them in)"""
    exefs = mods_dir() / name / 'exefs'
    return sorted(p.name for p in exefs.iterdir() if p.is_file() and PATCH_FILE.fullmatch(p.name)) \
        if exefs.is_dir() else []


def mod_list():
    """The mods, the ones on first in mods.txt's order (a later one wins where two have the same file), then the rest
    by name. Each a dict: name, on, files, bytes, usable (has romfs/ or code patches), replaces and adds (game files it
    replaces and new ones), case (files the game has only with other capitals, which it won't read), hidden_by ({later
    mod: files of this one it replaces}, for mods on), patches (its code patch files)"""
    on = mods_on()
    names = _GameNames()
    files = {name: _mod_files(name) for name in mod_folders()}
    out = []
    for name in on + [n for n in files if n not in on]:
        mine = files[name]
        found = [names.find(p) for p in mine]
        patches = _mod_patches(name)
        hidden = {}
        if name in on:
            for later in on[on.index(name) + 1:]:
                n = sum(1 for p in mine if p in files[later])
                if n:
                    hidden[later] = n
        out.append({'name': name, 'on': name in on, 'files': len(mine), 'bytes': sum(mine.values()),
                    'usable': bool(mine) or bool(patches), 'replaces': found.count('same'),
                    'adds': found.count(None), 'case': found.count('case'), 'hidden_by': hidden, 'patches': patches})
    return out


def mods_set(names):
    """Turns on the mods named, in that order, and every other one off: mods.txt rewritten whole. The game reads it
    when it starts. A name that isn't a mod folder with romfs/ or code patches is refused (ValueError) and nothing
    changes"""
    names = list(names)
    usable = {n for n in mod_folders() if (mods_dir() / n / 'romfs').is_dir() or _mod_patches(n)}
    for name in names:
        if not MOD_NAME.fullmatch(name) or name.strip() != name or name in ('.', '..') or name not in usable:
            raise ValueError(f'{name!r} is not a mod in {mods_dir()}')
    if len(set(names)) != len(names):
        raise ValueError('a mod is named twice')
    root = mods_dir()
    root.mkdir(parents=True, exist_ok=True)
    temporary = root / 'mods.txt.tmp'
    temporary.write_text('# Mods turned on, one folder name per line. Where two mods have the same file, the later '
                         'line wins.\n# Written by the launcher: its Mods page changes this.\n'
                         + ''.join(n + '\n' for n in names), encoding='utf8')
    temporary.replace(root / 'mods.txt')


def code_mods():
    """The code patches of the mods on, as the game applies them: [(mod, file name, bytes)], in mods.txt's order"""
    out = []
    for name in mods_on():
        for f in _mod_patches(name):
            out.append((name, f, (mods_dir() / name / 'exefs' / f).read_bytes()))
    return out


def code_mods_print(mods=None):
    """A fingerprint of the code patches on: the game code has to be compiled again when it changes"""
    h = hashlib.sha256(b'code mods 1\n')
    for name, f, data in code_mods() if mods is None else mods:
        h.update(f'{name}\0{f}\0{len(data)}\0'.encode('utf8'))
        h.update(data)
    return h.hexdigest()


def code_mods_changed():
    """The game code was compiled with other code patches than the ones on now (none recorded means none)"""
    try:
        built = CODEMODS.read_text(encoding='ascii').strip()
    except OSError:
        built = code_mods_print([])
    return built != code_mods_print()


TITLE_ID = re.compile(r'[0-9a-fA-F]{16}')
NOT_A_NAME = ('atmosphere', 'contents', 'titles', 'mods', 'romfs', 'exefs', 'exefs_patches', '')  # layout folders


def _mod_name(prefix_parts, fallback):
    """A mod's name from the folders above its romfs/ or exefs/: the nearest that isn't a title ID or a layout folder"""
    for part in reversed(prefix_parts):
        if not TITLE_ID.fullmatch(part) and part.lower() not in NOT_A_NAME:
            fallback = part
            break
    name = re.sub(r'[/\\:#\r\n]+', '_', fallback).strip(' .')[:60].strip() or 'mod'
    taken, n, base = set(mod_folders()), 1, name
    while name in taken or (mods_dir() / (name + '.part')).exists():
        n += 1
        name = f'{base} ({n})'
    return name


def _mod_layout(paths):
    """Where each file of a downloaded mod goes, from its '/' paths: (the mods found, as the folders above them, and
    {path: 'romfs/...' or 'exefs/<patch>'} for the files to keep). A mod is a folder holding romfs/ or exefs/
    (yuzu, Ryujinx, Atmosphère's contents/<title>/), or Atmosphère's exefs_patches/<name>/<build id>.ips. Everything
    under one atmosphere/ folder counts as one mod, as Atmosphère loads it so"""
    roots, keep = set(), {}
    for path in paths:
        parts = path.strip('/').split('/')
        for i, part in enumerate(parts[:-1]):
            low = part.lower()
            if low in ('romfs', 'exefs'):
                rest = parts[i + 1:]
                dest = f'{low}/' + '/'.join(rest)
            elif low == 'exefs_patches' and len(parts) == i + 3:
                rest = parts[i + 2:]
                dest = 'exefs/' + rest[0]
            else:
                continue
            lowered = [p.lower() for p in parts[:i]]
            root = tuple(parts[:lowered.index('atmosphere') + 1]) if 'atmosphere' in lowered else tuple(parts[:i])
            if low == 'exefs_patches' and 'atmosphere' not in lowered:
                root = tuple(parts[:i + 2])
            if dest.startswith('exefs/') and not (len(rest) == 1 and PATCH_FILE.fullmatch(rest[0])):
                break  # an exefs file that isn't a code patch (a replaced main, say) can't be used
            roots.add(root)
            keep[path] = (root, dest)
            break
    return roots, keep


def mod_import(source):
    """A mod from a folder or a .zip copied into mods/ and turned on, last (so it wins over the others). It may be
    laid out as yuzu's and Ryujinx's (<name>/romfs/..., <name>/exefs/<build id>.ips), Atmosphère's
    (atmosphere/contents/<title>/romfs/..., atmosphere/exefs_patches/<name>/<build id>.ips), or be a romfs or exefs
    folder itself. The new mod's name; ValueError with what's wrong, and nothing left behind"""
    import zipfile
    source = Path(source)
    data_romfs = (DATA / 'romfs').resolve()
    if any(p in (mods_dir().resolve(), data_romfs) for p in (source.resolve(), *source.resolve().parents)):
        raise ValueError('That is inside the game\'s own folder. Choose the mod you downloaded.')
    z = None
    if source.is_file() and source.suffix.lower() == '.zip':
        try:
            z = zipfile.ZipFile(source)
        except (OSError, zipfile.BadZipFile) as e:
            raise ValueError(f'That zip can\'t be opened ({e}).')
        infos = {i.filename.replace('\\', '/'): i for i in z.infolist() if not i.is_dir()}
        fallback = source.stem
    elif source.is_dir():
        base = source.parent if source.name.lower() in ('romfs', 'exefs') else source
        infos = {p.relative_to(base).as_posix(): p for p in source.rglob('*') if p.is_file()}
        fallback = base.name
    else:
        raise ValueError('Choose a mod\'s folder or its .zip.')
    try:
        roots, keep = _mod_layout(infos)
        if not roots:
            raise ValueError('That has no romfs folder and no code patches (.ips or .pchtxt in exefs). Only those '
                             'kinds of mods work so far.')
        if len(roots) > 1:
            raise ValueError(f'That holds {len(roots)} mods (or versions of one). Add the folder of the one you want.')
        root = next(iter(roots))
        # a code-only mod from exefs_patches/<name>/ is best called by that name
        named = {path.strip('/').split('/')[-2] for path, (_, dest) in keep.items()
                 if dest.startswith('exefs/') and '/exefs_patches/' in '/' + path.lower()}
        if len(named) == 1 and not any(dest.startswith('romfs/') for _, dest in keep.values()):
            root, fallback = (), named.pop()
        name = _mod_name(root, fallback)
        part = mods_dir() / (name + '.part')
        try:
            for path, (_, dest) in keep.items():
                target = (part / dest).resolve()
                if part.resolve() not in target.parents:
                    raise ValueError('That holds files outside its own folders: it was not added.')
                target.parent.mkdir(parents=True, exist_ok=True)
                if z is not None:
                    with z.open(infos[path]) as src, open(target, 'wb') as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
                else:
                    shutil.copyfile(infos[path], target)
        except BaseException:
            shutil.rmtree(part, ignore_errors=True)
            raise
    finally:
        if z is not None:
            z.close()
    if not keep:
        raise ValueError('That mod is empty.')
    os.replace(part, mods_dir() / name)
    mods_set(mods_on() + [name])
    return name


def mod_remove(name):
    """A mod's folder deleted and the mod taken off mods.txt. ValueError when it isn't a mod or the game runs (its
    files are open while it plays)"""
    if name not in mod_folders():
        raise ValueError('That mod isn\'t there any more.')
    if game_running():
        raise ValueError('Close the game first: it has the mod\'s files open.')
    mods_set([n for n in mods_on() if n != name])
    shutil.rmtree(mods_dir() / name)


_mods_supported = {}


def mods_supported():
    """Whether the installed game program reads mods/mods.txt (from build m1 on, 2026-11-07). Kept per file size and
    time, so the 55 MB is read once"""
    exe = GAME / (NAME + '.exe')
    try:
        st = exe.stat()
    except OSError:
        return False
    key = (st.st_size, st.st_mtime_ns)
    if key not in _mods_supported:
        _mods_supported.clear()
        _mods_supported[key] = b'mods/mods.txt' in exe.read_bytes()
    return _mods_supported[key]


def system_text():
    import platform
    return (f'B3-Express {launcher_version()}\nWindows {platform.version()} ({platform.machine()})\n'
            f'CPU threads {os.cpu_count()}\nGraphics {gpu_info()}\nGame folder {GAME}\n'
            f'Installed {installed()}, game code {module_ready()}\n'
            f'Mods on {", ".join(mods_on()) or "none"} (of {len(mod_folders())})\n')


def make_report():
    """A zip for a bug report: the game's logs, the settings and what PC this is. No game files, no saves. Its path"""
    import zipfile
    REPORTS.mkdir(exist_ok=True)
    path = REPORTS / time.strftime('report-%Y%m%d-%H%M%S.zip')
    sessions = sorted(LOGS.glob('session-*.log'))[-2:] if LOGS.is_dir() else []
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('system.txt', system_text())
        for p in [GAME / (NAME + '.log'), INI, LOGS / 'launcher-errors.log', LOGS / 'build-code.log', *sessions]:
            if p.is_file():
                z.write(p, p.name)
    return path


def session_stats(path):
    """The Play page's last session: average FPS, the typical 10-second stretch's 1% low and the median frame time
    on the display, from the game's reports every 600 frames. None before there are two reports"""
    try:
        text = path.read_text(encoding='utf8', errors='replace')
    except OSError:
        return None
    reports = re.findall(r'^\[\s*([\d.]+)\] nvn-live: frame times: median \d+ ms, p99 \d+ ms, 1% low ([\d.]+) FPS', text, re.M)
    shown = [float(x) for x in re.findall(r'frames on the display: median ([\d.]+) ms', text)]
    if len(reports) < 2:
        return None
    first, last = float(reports[0][0]), float(reports[-1][0])
    lows = sorted(float(x) for _, x in reports)
    return {'minutes': last / 60, 'fps': 600 * (len(reports) - 1) / max(last - first, 1e-3),
            'low': lows[len(lows) // 2], 'frame_ms': sorted(shown)[len(shown) // 2] if shown else None}


def newer_release(current, timeout=6):
    """(tag, page) of the latest GitHub release when it's newer than current, (None, None) when it isn't, or raises"""
    import urllib.request
    request = urllib.request.Request(f'https://api.github.com/repos/{REPO}/releases/latest',
                                     headers={'User-Agent': 'B3-Express-launcher', 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=timeout) as answer:
        release = json.loads(answer.read().decode('utf8'))
    tag, page = release.get('tag_name', ''), release.get('html_url', '')

    def parts(v):
        return tuple(int(x) for x in re.findall(r'\d+', v)[:3])
    if current == 'dev' or not parts(tag) or parts(tag) <= parts(current):
        return None, None
    return tag, page


class Launcher(LauncherTheme):
    def __init__(self, root):
        self.root, self.tk, self.ttk = root, tk, ttk
        self.dpi = root.winfo_fpixels('1i') / 96
        self.events = queue.Queue()
        self.process = None
        self.preparing = False
        self.dirty = False
        self.save_buttons = []
        self.save_status = tk.StringVar()
        self.installing = False
        self.builder = None
        # pythonw has no console: an error in a button or a thread would vanish and the click would seem to do nothing
        root.report_callback_exception = lambda *e: self.events.put(('error', log_error(*e)))
        threading.excepthook = lambda a: self.events.put(('error', log_error(a.exc_type, a.exc_value, a.exc_traceback)))
        root.title('Bayonetta 3')
        root.configure(bg=BG)
        root.geometry(f'{self.px(980)}x{self.px(640)}')
        root.minsize(self.px(900), self.px(600))
        self.style()
        self.text = INI.read_text(encoding='utf8', newline='') if INI.is_file() else ''
        nav = tk.Frame(root, bg=BG, padx=22, pady=24)
        nav.pack(side='left', fill='y')
        tk.Label(nav, text='BAYONETTA 3', bg=BG, fg=GOLD, font=('Georgia', 17, 'bold')).pack(pady=(0, 30))
        body = ttk.Frame(root, padding=26)
        body.pack(fill='both', expand=True)
        self.pages, self.nav = {}, {}
        for name in ('Play', 'Graphics', 'Controller', 'Install', 'Sessions'):
            self.pages[name] = ttk.Frame(body)
            self.nav[name] = ttk.Button(nav, text=name, command=lambda n=name: self.show(n))
            self.nav[name].pack(fill='x', pady=5)
        self.build_play()
        self.build_graphics()
        self.build_controller()
        self.build_install()
        self.build_sessions()
        self.refresh()
        self.show('Play' if installed() and module_ready() else 'Install')
        threading.Thread(target=lambda: self.events.put(('gpu', gpu_info())), daemon=True).start()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain)

    def close(self):
        if self.dirty:
            answer = messagebox.askyesnocancel('Settings', 'Save your changed settings before closing?')
            if answer is None or (answer and not self.save()):
                return
        builder = self.builder
        if builder and builder.poll() is None:
            builder.kill()  # the work folder is cleared by the next build
        self.root.destroy()

    # pages
    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        for n, button in self.nav.items():
            button.configure(state='disabled' if n == name else 'normal')

    def line(self, parent, title):
        var = tk.StringVar()
        self.row(parent, title, ttk.Label(parent, textvariable=var, wraplength=self.px(560), justify='left'))
        return var

    def build_play(self):
        page = self.pages['Play']
        self.section(page, 'Ready to play', top=0)
        self.game_line = self.line(page, 'Game')
        self.gpu_line = self.line(page, 'Graphics card')
        self.mode_line = self.line(page, 'Picture')
        self.last_line = self.line(page, 'Last session')
        bar = ttk.Frame(page)
        bar.grid(row=self.next_row(page), column=0, columnspan=2, sticky='w', pady=26)
        self.play_button = ttk.Button(bar, text='Play', style='Play.TButton', command=self.play)
        self.play_button.pack(side='left')
        self.status = tk.StringVar()
        ttk.Label(page, textvariable=self.status, wraplength=self.px(640)).grid(
            row=self.next_row(page), column=0, columnspan=2, sticky='w')
        self.note(page, 'In the game: F11 fullscreen, F2 the game\'s menu, F3 screenshot, F1 help. '
                        'The first Play prepares the game\'s shaders, a few minutes once for this graphics card and '
                        'driver. The game\'s window sits at its title meanwhile. You can close it: Play goes on '
                        'from where it stopped.', top=18)

    def build_graphics(self):
        page = self.pages['Graphics']
        res = to_float(ini_get(self.text, 'resolution', '1'), 1)
        fsr = to_float(ini_get(self.text, 'fsr', '0'), 0)
        self.section(page, 'Picture', top=0)
        self.screen = tk.StringVar(value=screen_of(res))
        self.quality = tk.StringVar(value=quality_of(res, fsr))
        self.custom = self.screen.get().startswith('Custom') or self.quality.get() == 'Custom'
        screen = ttk.Frame(page)
        ttk.Combobox(screen, textvariable=self.screen, state='readonly', width=20, values=[n for n, _ in SCREENS]).pack(side='left')
        ttk.Button(screen, text='Use my screen', command=self.use_my_screen).pack(side='left', padx=8)
        self.row(page, 'Screen resolution', screen)
        self.row(page, 'Quality', ttk.Combobox(page, textvariable=self.quality, state='readonly', width=20,
                                               values=[n for n, _ in QUALITY]))
        self.quality_help = tk.StringVar()
        ttk.Label(page, textvariable=self.quality_help, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
            row=self.next_row(page), column=1, sticky='w', pady=(4, 0))
        self.flags = {}
        self.check(page, 'fullscreen', 'Start in fullscreen')
        self.section(page, 'Options')
        self.check(page, 'shadows', 'Shadows')
        self.check(page, 'mute_unfocused', 'Mute when the game isn\'t the active window', default='1')
        self.aniso = tk.StringVar(value=ini_get(self.text, 'anisotropy', '0'))
        self.row(page, 'Texture filtering', ttk.Combobox(page, textvariable=self.aniso, state='readonly', width=20,
                                                         values=('0', '2', '4', '8', '16')), '0 = as the game asks.')
        names = [n for n, _ in FADES]
        self.fade = tk.StringVar(value=dict((v, n) for n, v in FADES).get(ini_get(self.text, 'dither', '3'), names[3]))
        self.row(page, 'Object fade', ttk.Combobox(page, textvariable=self.fade, state='readonly', width=20,
                                                   values=names))
        self.fade_help = tk.StringVar()
        ttk.Label(page, textvariable=self.fade_help, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
            row=self.next_row(page), column=1, sticky='w', pady=(4, 0))
        for var in (self.screen, self.quality, self.aniso, self.fade):
            var.trace_add('write', self.changed)
        self.quality.trace_add('write', self.quality_note)
        self.fade.trace_add('write', self.fade_note)
        self.quality_note()
        self.fade_note()
        self.save_bar(page)

    def build_controller(self):
        page = self.pages['Controller']
        self.section(page, 'Controller', top=0)
        self.check(page, 'gamepad_swap_ab', 'Swap A and B', default='0')
        self.deadzone = tk.StringVar(value=ini_get(self.text, 'gamepad_deadzone', '20'))
        self.rumble = tk.StringVar(value=ini_get(self.text, 'gamepad_rumble', '100'))
        names = [n for n, _ in PROMPTS]
        shown = dict((v, n) for n, v in PROMPTS).get(ini_get(self.text, 'button_prompts', '0'), names[0])
        self.prompts = tk.StringVar(value=shown)
        self.row(page, 'Button prompts', ttk.Combobox(page, textvariable=self.prompts, state='readonly', width=20,
                                                      values=names), 'Auto shows the buttons of the controller you use.')
        self.row(page, 'Stick dead zone (%)', ttk.Combobox(page, textvariable=self.deadzone, state='readonly', width=20,
                                                           values=('10', '15', '20', '25', '30')))
        self.row(page, 'Vibration (%)', ttk.Combobox(page, textvariable=self.rumble, state='readonly', width=20,
                                                     values=('0', '25', '50', '75', '100')))
        for var in (self.deadzone, self.rumble, self.prompts):
            var.trace_add('write', self.changed)
        self.save_bar(page)

    def save_bar(self, page):
        """Save settings, usable once something changed, and what state the settings file is in. Play saves too"""
        bar = ttk.Frame(page)
        bar.grid(row=self.next_row(page), column=0, columnspan=2, sticky='w', pady=(26, 0))
        button = ttk.Button(bar, text='Save settings', command=self.save_clicked, state='disabled')
        button.pack(side='left')
        ttk.Label(bar, textvariable=self.save_status, style='Muted.TLabel').pack(side='left', padx=12)
        self.save_buttons.append(button)

    def save_clicked(self):
        if self.save():
            self.refresh()

    def check(self, parent, key, label, default='1'):
        var = tk.StringVar(value=ini_get(self.text, key, default))
        widget = ttk.Checkbutton(parent, text=label, variable=var, onvalue='1', offvalue='0')
        widget.grid(row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(6, 0))
        var.trace_add('write', self.changed)
        self.flags[key] = var

    def build_install(self):
        page = self.pages['Install']
        self.section(page, 'Install your game', top=0)
        self.note(page, 'You need your own extracted dump of Bayonetta 3 with the 1.2.0 update applied. '
                        'Nothing is downloaded and nothing leaves this PC.', top=2)
        self.source = tk.StringVar()
        self.pick(page, self.source, 'Extracted game', 'Choose the folder that holds both exefs and romfs.')
        self.install_button = ttk.Button(page, text='Install', command=self.install)
        self.install_button.grid(row=self.next_row(page), column=0, sticky='w', pady=(18, 8))
        self.install_status = tk.StringVar()
        ttk.Label(page, textvariable=self.install_status, wraplength=self.px(640), justify='left').grid(
            row=self.next_row(page), column=0, columnspan=2, sticky='w')

    def pick(self, parent, var, label, hint=None):
        frame = ttk.Frame(parent)
        ttk.Entry(frame, textvariable=var, width=46).pack(side='left')

        def browse():
            value = filedialog.askdirectory(title=label)
            if value:
                var.set(value)
        ttk.Button(frame, text='Browse', command=browse).pack(side='left', padx=6)
        self.row(parent, label, frame, hint)

    def build_sessions(self):
        page = self.pages['Sessions']
        self.section(page, 'Your sessions', top=0)
        self.session_line = self.line(page, 'Last session')
        self.note(page, 'Each launch keeps the previous log here, so nothing is overwritten. '
                        'Logs hold your file paths, so check one before sharing it.')
        ttk.Button(page, text='Open the log folder', command=self.open_logs).grid(
            row=self.next_row(page), column=0, sticky='w', pady=(16, 0))

    # state
    def changed(self, *_):
        self.dirty = True
        self.save_status.set('Unsaved changes.')
        self.refresh()

    def quality_note(self, *_):
        self.quality_help.set(QUALITY_HELP.get(self.quality.get(), 'Set by hand in the settings file.'))

    def fade_note(self, *_):
        self.fade_help.set(FADE_HELP[self.fade.get()])

    def use_my_screen(self):
        self.screen.set(best_screen(self.root.winfo_screenwidth(), self.root.winfo_screenheight()))

    def refresh(self):
        for button in self.save_buttons:
            button.configure(state='normal' if self.dirty else 'disabled')
        have_files, have_code = installed(), module_ready()
        ready = have_files and have_code
        self.game_line.set('Installed, version 1.2.0' if ready else
                           'The game code isn\'t built yet. Open Install to build it.' if have_files else
                           'Not installed yet. Open Install to add your game.')
        self.mode_line.set(f'{self.screen.get()}, {self.quality.get()}'
                           + (', fullscreen' if self.flags['fullscreen'].get() == '1' else ', in a window'))
        summary = summary_text(session_summary(GAME / (NAME + '.log')))
        self.last_line.set(summary)
        self.session_line.set(summary)
        self.play_button.configure(state='normal' if ready and not self.process and not self.installing else 'disabled')
        self.install_button.configure(state='disabled' if ready or self.installing else 'normal',
                                      text='Build game code' if have_files and not have_code else 'Install')
        if ready:
            self.install_status.set(self.install_status.get() or 'The game is installed.')
        elif have_files and not self.installing:
            self.install_status.set(self.install_status.get() or
                                    'Your game files are here. The game code is compiled on this PC from them, once, '
                                    'in about 5 minutes.')

    def save(self):
        """The game's own settings file: only what changed here, comments kept, a copy of the first version beside it"""
        if not self.dirty:
            return True
        text = self.text
        res = dict(SCREENS).get(self.screen.get())
        scene = dict(QUALITY).get(self.quality.get(), 1.0)
        try:
            if res is not None and self.quality.get() != 'Custom':
                resolution, fsr = scale_settings(res, scene)
                text = ini_set(text, 'resolution', f'{resolution:g}', 'docked')
                text = ini_set(text, 'fsr', f'{fsr:g}', 'resolution')
            for key, var in self.flags.items():
                text = ini_set(text, key, var.get(), 'fullscreen')
            text = ini_set(text, 'anisotropy', self.aniso.get(), 'vsync')
            text = ini_set(text, 'dither', dict(FADES)[self.fade.get()], 'resolution')
            text = ini_set(text, 'gamepad_deadzone', self.deadzone.get(), 'gamepad_swap_ab')
            text = ini_set(text, 'gamepad_rumble', self.rumble.get(), 'gamepad_deadzone')
            text = ini_set(text, 'button_prompts', dict(PROMPTS)[self.prompts.get()], 'gamepad_swap_ab')
            backup = INI.with_suffix('.ini.before-launcher')
            if INI.is_file() and not backup.exists():
                shutil.copy2(INI, backup)
            temporary = INI.with_suffix('.tmp')
            temporary.write_text(text, encoding='utf8', newline='')
            temporary.replace(INI)
        except (OSError, ValueError) as e:
            messagebox.showerror('Settings', f'The settings could not be saved: {e}')
            return False
        self.text = text
        self.dirty = False
        self.save_status.set('Settings saved.')
        for button in self.save_buttons:
            button.configure(state='disabled')
        return True

    def play(self):
        if self.process or not self.save():
            return
        if game_running():
            self.status.set('The game is already running.')
            return
        self.preparing = can_prepare() and not shaders_ready(GAME / 'shader_cache', prep_mode(self.text))
        try:
            keep_last_log()
            env = dict(os.environ, **ENV)
            if self.preparing:
                env['SNX_NVN_PREP_RUN'] = '1'
            self.process = subprocess.Popen(launch_command(), cwd=GAME, env=env, stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=HIDDEN)
        except OSError as e:
            messagebox.showerror('Play', f'The game could not start: {e}')
            return
        self.status.set('Preparing shaders, once for this graphics card...' if self.preparing else 'Starting the game...')
        self.refresh()
        threading.Thread(target=self.wait_game, args=(self.process,), daemon=True).start()
        if self.preparing:
            threading.Thread(target=self.watch_prepare, args=(self.process,), daemon=True).start()

    def wait_game(self, process):
        self.events.put(('exit', process.wait()))

    def watch_prepare(self, process):
        while process.poll() is None:
            got = prepare_progress(GAME / 'shader_cache')
            if got and sum(got) >= PREPARE_MIN:  # the game gives its stages over its first half minute
                self.events.put(('prepare', f'Preparing shaders, once for this graphics card: '
                                            f'{100 * got[0] // sum(got)}%'))
            time.sleep(1)

    def open_logs(self):
        LOGS.mkdir(exist_ok=True)
        os.startfile(LOGS)

    def install(self):
        if self.installing:
            return
        if not (GAME / (NAME + '.exe')).is_file():
            self.install_status.set(f'The game\'s program isn\'t in {GAME}. Install needs the whole package.')
            return
        if game_running():
            self.install_status.set('Close the game first.')
            return
        if game_running('scarletnx'):
            self.install_status.set('The game code is already compiling, in another window.')
            return
        if installed():
            self.start_build()
            return
        text = self.source.get().strip('" ')
        source = Path(text)
        temp = (GAME / '_copy').resolve()
        if not text or not has_game(source) or temp in (source.resolve(), *source.resolve().parents):
            self.install_status.set('Choose the folder that holds both exefs and romfs.')
            return
        if any((DATA / name).exists() for name in ('exefs', 'romfs')):
            self.install_status.set(f'{DATA} already has part of a game. Remove its exefs and romfs first.')
            return
        problem = version_problem(source)
        if problem:
            self.install_status.set(problem)
            return
        shutil.rmtree(temp, ignore_errors=True)  # a stopped copy mustn't count against the free space
        free = shutil.disk_usage(GAME).free / 2**30
        if free < FREE_GB:
            self.install_status.set(f'This drive has {free:.0f} GB free, and installing needs about {FREE_GB} GB.')
            return
        self.installing = True
        self.refresh()
        self.install_status.set('Copying your game files.')
        threading.Thread(target=self.install_run, args=(source,), daemon=True).start()

    def start_build(self):
        problem = version_problem()
        if problem:
            self.install_status.set(f'{problem}\nRemove exefs and romfs from {DATA}, then install again.')
            return
        shutil.rmtree(WORK, ignore_errors=True)
        free = shutil.disk_usage(GAME).free / 2**30
        if free < BUILD_FREE_GB:
            self.install_status.set(f'This drive has {free:.1f} GB free, and building needs about {BUILD_FREE_GB} GB.')
            return
        self.installing = True
        self.refresh()
        threading.Thread(target=self.build_run, daemon=True).start()

    def build_run(self):
        """In the install's thread: the compile's progress, then one 'installed' event with how it ended"""
        self.events.put(('progress', 'Compiling the game code for this PC. This takes about 5 minutes.'))
        try:
            problem = build_code(lambda got: self.events.put(('progress', f'Compiling the game code: {100 * got[0] // got[1]}%')),
                                 lambda process: setattr(self, 'builder', process))
        except Exception as e:  # the page must never stay on Compiling
            problem = str(e)
        self.builder = None
        self.events.put(('installed', 'The game is ready. Open Play.' if not problem else
                         f'Compiling the game code failed.\n{problem}'))

    def install_run(self, source):
        try:
            if STAMP.exists():  # code without files is from something else: it counts only once built from these
                STAMP.unlink()
            copy_game(source, lambda percent: self.events.put(('progress', f'Copying your game files: {percent}%')))
        except Exception as e:  # the page must never stay on Copying
            shutil.rmtree(GAME / '_copy', ignore_errors=True)
            self.events.put(('installed', f'Installing failed: {error_text(e)}'))
            return
        self.build_run()

    def drain(self):
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == 'gpu':
                self.gpu_line.set(value)
            elif kind == 'error':
                self.installing = False  # a stuck flag would make every later click do nothing
                self.refresh()
                messagebox.showerror('Bayonetta 3', value)
            elif kind == 'exit':
                self.process = None
                self.text = INI.read_text(encoding='utf8', newline='') if INI.is_file() else self.text
                if self.preparing:
                    self.preparing = False
                    if shaders_ready(GAME / 'shader_cache', prep_mode(self.text)):
                        self.play()
                    else:
                        self.status.set('Shader preparation stopped. Play goes on from where it was.')
                        self.refresh()
                    continue
                self.status.set('Game closed.' if value == 0 else
                                f'The game stopped with an error (code {value}). The Sessions page has the log.')
                self.refresh()
            elif kind == 'prepare':
                self.status.set(value)
            elif kind == 'progress':
                self.install_status.set(value)
            elif kind == 'installed':
                self.installing = False
                self.install_status.set(value)
                self.refresh()
        self.root.after(100, self.drain)


if __name__ == '__main__':
    if '--check' in sys.argv:
        print(json.dumps({'command': launch_command(), 'folder': str(GAME), 'installed': installed(), 'env': ENV,
                          'gpu': gpu_info(), 'last_session': session_summary(GAME / (NAME + '.log'))}, indent=2))
    else:
        window = tk.Tk()
        app = Launcher(window)
        if '--smoke-test' in sys.argv:
            window.withdraw()
            for page in app.pages:
                app.show(page)
                window.update_idletasks()
            window.destroy()
            print('All launcher pages built.')
        else:
            window.mainloop()
