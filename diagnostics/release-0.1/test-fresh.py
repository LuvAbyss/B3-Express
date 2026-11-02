"""A new player's path on the unzipped package, run by the package's own Python: a real Install window from an extracted dump, then the game
starts. Writes only under the test folder. test-fresh.py <unzipped package> <folder with exefs and romfs> <result.json>"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

pack, source, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
sys.path.insert(0, str(pack / 'launcher'))
os.environ.pop('B3_GAME_DIR', None)
import bayonetta_launcher as L

assert Path(L.__file__).resolve().parent == (pack / 'launcher').resolve(), L.__file__
assert L.GAME.resolve() == (pack / 'Bayo3_120').resolve(), L.GAME
window = L.tk.Tk()
app = L.Launcher(window)
r = {'button_before': app.install_button['text'], 'play_before': str(app.play_button['state'])}
app.source.set(str(source))
t0 = time.time()
app.install()
while app.installing and time.time() - t0 < 1500:
    window.update()
    time.sleep(.05)
window.update()
r.update(seconds=round(time.time() - t0), status=app.install_status.get(), ready=L.installed(), play=str(app.play_button['state']))
window.destroy()
r['module_size'] = L.CODE.stat().st_size if L.CODE.is_file() else 0
if r['ready']:
    env = dict(os.environ, **L.ENV)
    p = subprocess.run([str(L.GAME / 'Bayo3_120.exe'), f'--game={L.GAME}', '--seconds=40', '--fullscreen=0', '--network=0', '--cutscene_fps=60'],
                       cwd=L.GAME, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=150)
    log = (L.GAME / 'Bayo3_120.log').read_text(errors='replace').splitlines()
    r.update(game_exit=p.returncode, scene60=[l[:70] for l in log if 'scene60:' in l][:3], frames=[l[:60] for l in log if ' frames: draws' in l][-1:], tail=log[-1][:80])
out.write_text(json.dumps(r, indent=2))
print(json.dumps(r, indent=2))
