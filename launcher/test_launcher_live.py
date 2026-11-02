"""Drives the real launcher window against a test copy of the game: test_launcher_live.py <game folder> <picture folder>.
Chooses settings, saves, presses Play, and reads what the game did. Never run it against output/: it writes the INI."""
import os, subprocess, sys, time
from pathlib import Path
game, shots = Path(sys.argv[1]), Path(sys.argv[2])
assert 'output' not in game.parts, 'test copy only'
os.environ['B3_GAME_DIR'] = str(game)
sys.path.insert(0, r'D:\Project-B3PC\B3-PC-Port-Portable\launcher')
import bayonetta_launcher as L
L.LOGS = shots / 'logs'
ini_before = L.INI.read_bytes()
window = L.tk.Tk()
app = L.Launcher(window)

def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        window.update()
        time.sleep(.02)

def shot(name):
    window.update()
    x, y, w, h = window.winfo_rootx(), window.winfo_rooty(), window.winfo_width(), window.winfo_height()
    out = shots / (name + '.png')
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    'Add-Type -AssemblyName System.Drawing; Add-Type -AssemblyName System.Windows.Forms;'
                    f'$b=New-Object Drawing.Bitmap {w},{h};$g=[Drawing.Graphics]::FromImage($b);'
                    f'$g.CopyFromScreen({x},{y},0,0,$b.Size);$b.Save("{out}")'], check=True, creationflags=L.HIDDEN)

window.lift()
window.attributes('-topmost', True)
for page in app.pages:
    app.show(page)
    pump(.6)
    shot('page-' + page.lower())
window.attributes('-topmost', False)
app.show('Play')
app.screen.set('1920 x 1080')
app.quality.set('Smooth 60')
app.flags['mute_unfocused'].set('1')
app.flags['fullscreen'].set('0')
pump(.2)
assert app.dirty
assert app.save()
text = L.INI.read_text(encoding='utf8', newline='')
print('saved:', {k: L.ini_get(text, k) for k in ('resolution', 'fsr', 'mute_unfocused', 'fullscreen', 'anisotropy')})
assert L.ini_get(text, 'resolution') == '1.2' and L.ini_get(text, 'fsr') == '1.2'
assert len(text.splitlines()) >= len(ini_before.decode().splitlines())
assert '; ' in text
app.play()
assert app.process
pump(70)
assert app.process and app.process.poll() is None, 'the game stopped by itself'
log = (game / 'Bayo3_120.log').read_text(encoding='utf8', errors='replace')
for needle in ('vulkan: render targets at 1.20x', 'nvn: routing NVN calls', 'nvn-live: window 1.20x, scene 1.00x, FSR 1', 'audio: SDL'):
    print(needle, '->', needle in log)
    assert needle in log
draws = [l for l in log.splitlines() if 'nvn-live: FSR 1, ' in l]
print('renderer:', draws[0] if draws else 'no FSR line yet')
print('old renderer draws line:', [l[:70] for l in log.splitlines() if 'vulkan: ' in l and ' draws, ' in l][-1:])
app.status.set('')
app.process.terminate()
pump(3)
print('play button after exit:', str(app.play_button.cget('state')), '| status:', app.status.get())
window.destroy()
print('live test passed')
