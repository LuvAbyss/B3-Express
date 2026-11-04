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
WORK = GAME / '_build'
LOGS = ROOT / 'logs'
SEED_HINTS = Path(__file__).resolve().parent / 'code-hints.txt'  # addresses only: code past runs found that the compiler can't
TABLE_CALLS = Path(__file__).resolve().parent / 'code-tablecalls.txt'  # addresses of the functions the runtime replaces, for cutscene_fps=60


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
    """The game code is built. The stamp goes in last, so a build that stopped never counts"""
    return CODE.is_file() and STAMP.is_file()


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
        (out / NAME).mkdir(parents=True)
        repo.mkdir()
        hints = merged_hints(SEED_HINTS, GAME / (NAME + '.hints'))
        (out / NAME / (NAME + '.hints')).write_text(''.join(x + '\n' for x in hints), encoding='ascii')
        if TABLE_CALLS.is_file():
            shutil.copyfile(TABLE_CALLS, out / NAME / (NAME + '.tablecalls'))
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
        built, stamp = out / NAME / CODE.name, out / NAME / STAMP.name
        if code or not built.is_file() or not stamp.is_file():
            return f'The compiler stopped (code {code}).\n' + '\n'.join(tail)
        if STAMP.exists():
            STAMP.unlink()
        os.replace(built, CODE)
        os.replace(stamp, STAMP)
    except OSError as e:
        if process and process.poll() is None:
            process.kill()
        return error_text(e)
    shutil.rmtree(WORK, ignore_errors=True)  # the game is ready by now: what is left goes with the next build
    return None


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
    """As the game's rp_scaling: drawn above the console's size, or through FSR. Shaders differ between the two"""
    return to_float(ini_get(text, 'resolution', '1'), 1.0) != 1.0 or to_float(ini_get(text, 'fsr', '0'), 0.0) > 1.0


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


class Launcher(LauncherTheme):
    def __init__(self, root):
        self.root, self.tk, self.ttk = root, tk, ttk
        self.dpi = root.winfo_fpixels('1i') / 96
        self.events = queue.Queue()
        self.process = None
        self.preparing = False
        self.dirty = False
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
        for var in (self.screen, self.quality, self.aniso):
            var.trace_add('write', self.changed)
        self.quality.trace_add('write', self.quality_note)
        self.quality_note()

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
        self.refresh()

    def quality_note(self, *_):
        self.quality_help.set(QUALITY_HELP.get(self.quality.get(), 'Set by hand in the settings file.'))

    def use_my_screen(self):
        self.screen.set(best_screen(self.root.winfo_screenwidth(), self.root.winfo_screenheight()))

    def refresh(self):
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
            text = ini_set(text, 'gamepad_deadzone', self.deadzone.get(), 'gamepad_swap_ab')
            text = ini_set(text, 'gamepad_rumble', self.rumble.get(), 'gamepad_deadzone')
            text = ini_set(text, 'button_prompts', dict(PROMPTS)[self.prompts.get()], 'gamepad_swap_ab')
            backup = INI.with_suffix('.ini.before-launcher')
            if not backup.exists():
                shutil.copy2(INI, backup)
            temporary = INI.with_suffix('.tmp')
            temporary.write_text(text, encoding='utf8', newline='')
            temporary.replace(INI)
        except (OSError, ValueError) as e:
            messagebox.showerror('Settings', f'The settings could not be saved: {e}')
            return False
        self.text = text
        self.dirty = False
        return True

    def play(self):
        if self.process or not self.save():
            return
        if game_running():
            self.status.set('The game is already running.')
            return
        self.preparing = can_prepare() and not shaders_ready(GAME / 'shader_cache', scaled_mode(self.text))
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
                    if shaders_ready(GAME / 'shader_cache', scaled_mode(self.text)):
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
