# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the B3-Express contributors
"""The B3-Express launcher window: ui/index.html in Microsoft's WebView2 through pywebview, its buttons calling Api
below, the work done by bayonetta_launcher.py. Without pywebview or WebView2 the old window opens instead.

The page asks Api.state() twice a second for what to show; the long jobs (install, compile, shader preparation) run in
threads and only update self._job, so no call from the page ever waits on them."""
import ctypes
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

import bayonetta_launcher as L

UI = Path(__file__).resolve().parent / 'ui' / 'index.html'
ICON = UI.parent / 'b3express.ico'  # the window's and taskbar's icon, the same as B3-Express.exe's (exe/build.py)
ISSUES = f'https://github.com/{L.REPO}/issues'
RELEASES = f'https://github.com/{L.REPO}/releases'


class Job:
    """A long task the Play page shows: what it does, how far (None while that isn't known yet), how it ended"""

    def __init__(self, kind, label):
        self.kind, self.label, self.pct, self.error, self.done = kind, label, None, None, False
        self.phase_start, self.phase_pct = time.time(), 0

    def phase(self, label):
        self.label, self.pct, self.phase_start, self.phase_pct = label, None, time.time(), 0

    def progress(self, pct):
        if self.pct is None:
            self.phase_start, self.phase_pct = time.time(), pct
        self.pct = max(0, min(100, pct))

    def eta(self):
        """Seconds left in this phase from its pace so far, None until there is a pace to go by"""
        if self.pct is None:
            return None
        went, spent = self.pct - self.phase_pct, time.time() - self.phase_start
        return None if went < 2 or spent < 3 else spent / went * (100 - self.pct)

    def view(self):
        eta = self.eta()
        return {'kind': self.kind, 'label': self.label, 'pct': self.pct, 'eta': None if eta is None else round(eta),
                'error': self.error, 'done': self.done}


class Api:
    def __init__(self):
        self._window = None
        self._job = None
        self._game = None  # the game's process while it runs
        self._child = None  # the compiler or the preparation run, which Stop ends
        self._exit_note = ''
        self._gpu = None
        self._text = self._read_ini()
        threading.Thread(target=lambda: setattr(self, '_gpu', L.gpu_info()), daemon=True).start()

    @staticmethod
    def _read_ini():
        return L.INI.read_text(encoding='utf8', newline='') if L.INI.is_file() else ''

    # ---- Play
    def state(self):
        files, code = L.installed(), L.module_ready()
        cache = L.GAME / 'shader_cache'
        prepared = L.shaders_ready(cache, L.prep_mode(self._text))
        step = 'install' if not (files and code) else 'prepare' if L.can_prepare() and not prepared else 'play'
        running = self._game is not None and self._game.poll() is None
        return {'step': step, 'files': files, 'code': code, 'prepared': prepared,
                'stale': (cache / 'prepared.txt').is_file() and not prepared, 'running': running,
                'job': self._job.view() if self._job else None, 'note': self._exit_note, 'gpu': self._gpu,
                'version': L.launcher_version(), 'last': L.session_stats(L.GAME / (L.NAME + '.log')),
                'codemods': files and L.CODE.is_file() and L.STAMP.is_file() and L.code_mods_changed(),
                'program': (L.GAME / (L.NAME + '.exe')).is_file()}

    def _busy(self):
        return self._job is not None and not self._job.done

    def choose_folder(self):
        import webview
        got = self._window.create_file_dialog(webview.FileDialog.FOLDER)
        return got[0] if got else ''

    def check_folder(self, folder):
        """What is wrong with this folder as the game to install, or '' when Install can go ahead"""
        source = Path(folder.strip('" '))
        if not folder or not L.has_game(source):
            return 'That folder doesn\'t hold both exefs and romfs. Choose the folder that has the two of them.'
        temp = (L.GAME / '_copy').resolve()
        if temp in (source.resolve(), *source.resolve().parents):
            return 'That is the launcher\'s own work folder. Choose your extracted game.'
        return L.version_problem(source) or ''

    def install(self, folder):
        """Copies the game and compiles its code, or only compiles when the files are already here. '' when it started,
        else why not"""
        if self._busy():
            return 'Something is already running.'
        if not (L.GAME / (L.NAME + '.exe')).is_file():
            return f'The game\'s program isn\'t in {L.GAME}. Install needs the whole package.'
        if L.game_running():
            return 'Close the game first.'
        if L.game_running('scarletnx'):
            return 'The game code is already compiling, in another window.'
        if L.installed():
            problem = L.version_problem()
            if problem:
                return f'{problem}\nRemove exefs and romfs from {L.DATA}, then install again.'
            source = None
        else:
            problem = self.check_folder(folder)
            if problem:
                return problem
            if any((L.DATA / name).exists() for name in ('exefs', 'romfs')):
                return f'{L.DATA} already has part of a game. Remove its exefs and romfs first.'
            source = Path(folder.strip('" '))
        L.shutil.rmtree(L.GAME / '_copy', ignore_errors=True)
        L.shutil.rmtree(L.WORK, ignore_errors=True)
        need = L.BUILD_FREE_GB if source is None else L.FREE_GB
        free = L.shutil.disk_usage(L.GAME).free / 2**30
        if free < need:
            return f'This drive has {free:.0f} GB free, and this needs about {need} GB.'
        self._job = Job('install', 'Copying your game files' if source else 'Compiling the game code for this PC')
        threading.Thread(target=self._install_run, args=(source,), daemon=True).start()
        return ''

    def _install_run(self, source):
        job = self._job
        try:
            if source is not None:
                if L.STAMP.exists():  # code without files is from something else: it counts only once built from these
                    L.STAMP.unlink()
                L.copy_game(source, job.progress)
                job.phase('Compiling the game code for this PC')
            problem = L.build_code(lambda got: job.progress(100 * got[0] // max(got[1], 1)),
                                   lambda process: setattr(self, '_child', process))
            job.error = (f'Compiling the game code failed.\n{problem}') if problem else None
        except Exception as e:  # the page must never stay on a running job
            L.shutil.rmtree(L.GAME / '_copy', ignore_errors=True)
            job.error = f'Installing failed: {L.error_text(e)}'
            L.log_error(type(e), e, e.__traceback__)
        self._child = None
        job.done = True

    def _start_game(self, prepare):
        L.keep_last_log()
        env = dict(os.environ, **L.ENV)
        if prepare:
            env['SNX_NVN_PREP_RUN'] = '1'
        return subprocess.Popen(L.launch_command(), cwd=L.GAME, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=L.HIDDEN)

    def prepare(self, again=False):
        """The game run once to prepare its shaders for this graphics card (it sits at its title meanwhile)"""
        if self._busy() or (self._game and self._game.poll() is None):
            return 'Something is already running.'
        if L.game_running():
            return 'Close the game first.'
        if again:
            try:
                (L.GAME / 'shader_cache' / 'prepared.txt').unlink()
            except OSError:
                pass
        try:
            self._child = self._start_game(prepare=True)
        except OSError as e:
            return f'The game could not start: {e}'
        self._job = Job('prepare', 'Preparing shaders for your graphics card')
        threading.Thread(target=self._prepare_run, args=(self._child,), daemon=True).start()
        return ''

    def _prepare_run(self, process):
        job = self._job
        while process.poll() is None:
            got = L.prepare_progress(L.GAME / 'shader_cache')
            if got and sum(got) >= L.PREPARE_MIN:  # the game gives its stages over its first half minute
                job.progress(100 * got[0] // sum(got))
                job.label = f'Preparing shaders for your graphics card · {sum(got):,} stages'
            time.sleep(0.5)
        self._text = self._read_ini()
        if not L.shaders_ready(L.GAME / 'shader_cache', L.prep_mode(self._text)):
            job.error = 'Shader preparation stopped before the end. Prepare again to go on from where it was.'
        self._child = None
        job.done = True

    def play(self):
        if self._busy() or (self._game and self._game.poll() is None):
            return 'Something is already running.'
        if L.game_running():
            return 'The game is already running.'
        try:
            self._game = self._start_game(prepare=False)
        except OSError as e:
            return f'The game could not start: {e}'
        self._exit_note = ''
        threading.Thread(target=self._wait_game, args=(self._game,), daemon=True).start()
        return ''

    def _wait_game(self, process):
        code = process.wait()
        self._text = self._read_ini()  # the game's own menu may have changed it
        self._exit_note = '' if code == 0 else f'The game stopped with an error (code {code}). Help makes a bug report.'

    def stop(self):
        child = self._child
        if child and child.poll() is None:
            child.kill()  # the compile's work folder is cleared by the next one; preparation goes on where it was
        return ''

    def dismiss(self):
        """The page has shown how the last job ended"""
        if self._job and self._job.done:
            self._job = None
        return ''

    # ---- Options
    def options(self):
        self._text = self._read_ini()
        return {'values': L.options_read(self._text), 'quality_help': L.QUALITY_HELP, 'fade_help': L.FADE_HELP,
                'screens': [n for n, _ in L.SCREENS], 'has_file': L.INI.is_file()}

    def save_options(self, values):
        try:
            text = L.options_apply(self._read_ini(), values)
            L.write_ini(text)
        except (OSError, ValueError, KeyError) as e:
            return f'The options could not be saved: {e}'
        self._text = text
        return ''

    def my_screen(self):
        user32 = ctypes.windll.user32
        return L.best_screen(user32.GetSystemMetrics(0), user32.GetSystemMetrics(1))

    # ---- Saves
    def backups(self):
        return [{'name': n, 'when': time.strftime('%d %b %Y, %H:%M', time.localtime(t)), 'size': s, 'note': note}
                for n, t, s, note in L.backup_list()]

    def backup_now(self):
        if L.game_running():
            return {'error': 'Close the game first, so the backup holds a finished save.'}
        try:
            name = L.backup_save()
        except OSError as e:
            return {'error': f'The backup could not be made: {e}'}
        return {'name': name} if name else {'error': 'There is no save to back up yet. Play first.'}

    def restore(self, name):
        try:
            return L.restore_save(name) or ''
        except (OSError, ValueError) as e:
            return f'The save could not be restored: {e}'

    # ---- Mods
    def mods(self):
        try:
            mods = L.mod_list()
        except OSError as e:
            return {'error': f'The mods folder can\'t be read: {e}', 'mods': []}
        return {'mods': mods, 'supported': L.mods_supported(), 'program': (L.GAME / (L.NAME + '.exe')).is_file(),
                'running': (self._game is not None and self._game.poll() is None) or L.game_running(),
                'folder': str(L.mods_dir())}

    def mods_save(self, names):
        """The mods on, in this order, every other one off"""
        try:
            L.mods_set(names)
        except (OSError, ValueError) as e:
            return f'The list could not be saved: {e}'
        return ''

    def mod_add(self, kind):
        """A mod from a folder or a .zip the player picks; {'name'} or {'error'}, or {} when the dialog was closed"""
        import webview
        if kind == 'zip':
            got = self._window.create_file_dialog(webview.FileDialog.OPEN, file_types=('Mod zip (*.zip)',))
        else:
            got = self._window.create_file_dialog(webview.FileDialog.FOLDER)
        if not got:
            return {}
        try:
            return {'name': L.mod_import(got[0])}
        except ValueError as e:
            return {'error': str(e)}
        except OSError as e:
            return {'error': f'The mod could not be copied: {e}'}

    def mod_remove(self, name):
        try:
            L.mod_remove(name)
        except (OSError, ValueError) as e:
            return str(e)
        return ''

    def open_mods(self):
        L.mods_dir().mkdir(parents=True, exist_ok=True)
        os.startfile(L.mods_dir())
        return ''

    # ---- Help
    def report(self):
        try:
            path = L.make_report()
        except OSError as e:
            return {'error': f'The report could not be made: {e}'}
        subprocess.Popen(['explorer', '/select,', str(path)], creationflags=L.HIDDEN)
        return {'path': str(path), 'kb': round(path.stat().st_size / 1024)}

    def open_logs(self):
        L.LOGS.mkdir(exist_ok=True)
        os.startfile(L.LOGS)
        return ''

    def open_link(self, which):
        """Only the project's own pages: the page can't open any address it likes"""
        url = {'issues': ISSUES, 'releases': RELEASES}.get(which)
        if url:
            os.startfile(url)
        return ''

    def check_update(self):
        try:
            tag, page = L.newer_release(L.launcher_version())
        except Exception as e:  # offline, GitHub down, rate limit: the page says so and nothing else changes
            return {'error': f'Couldn\'t reach GitHub ({type(e).__name__}).'}
        return {'tag': tag, 'page': page}

    def closing(self):
        child = self._child
        if child and child.poll() is None and self._job and self._job.kind == 'install':
            child.kill()  # as the old window: the next build starts again
        return True


def main():
    try:
        import webview
    except Exception:  # no pywebview in this Python: the old window
        return old_window()
    try:  # its own taskbar button and icon, not grouped under Python's
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('B3Express.Launcher')
    except (AttributeError, OSError):
        pass
    api = Api()
    window = webview.create_window('B3-Express', url=str(UI), js_api=api, width=1180, height=760, min_size=(900, 620),
                                   background_color='#0e0b15')
    api._window = window
    window.events.closing += api.closing
    try:
        webview.start(gui='edgechromium', private_mode=True, icon=str(ICON) if ICON.is_file() else None)
    except Exception as e:  # no WebView2 on this PC
        L.log_error(type(e), e, e.__traceback__)
        return old_window()


def old_window():
    root = L.tk.Tk()
    L.Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    main()
