# B3-Express launcher

Start it with `B3-Express.exe`, a small starter (`exe/b3express.c`, built with its icon by `exe/build.py`) that runs `web_launcher.py` with the Python in `launcher/python`, so nothing needs installing. `Launch-Bayonetta3.cmd` does the same from a repository checkout.

The window is `ui/index.html`, shown by Microsoft Edge WebView2 (part of Windows 11, and of Windows 10 with a current
Edge) through pywebview; `web_launcher.py` holds what its buttons call, and `bayonetta_launcher.py` the logic. On a PC
without WebView2 the old window opens instead, with the same logic.

- **Play:** one button that always does the next step: Install, then Prepare shaders (once for a graphics card and
  driver, about 3 minutes, the game sits at its title meanwhile), then Play. Progress with time left while it works,
  and the last session's average FPS, 1% low and frame time.
- **Options:** window size and quality, fullscreen, shadows, texture filtering, object fade, button prompts, A/B swap,
  dead zone, vibration, muting. Saved in the game's `Bayo3_120.ini`, which its own menu also uses, every comment kept.
  The first version of that file stays beside it as `Bayo3_120.ini.before-launcher`.
- **Saves:** back up the game's save folder to `backups/`, and restore one (the current save is backed up first).
- **Mods:** not yet: the game can't load mods.
- **Help:** a bug report zip in `reports/` (the game's log, the settings, the graphics card; no game files, no saves),
  the GitHub issues page, the update check against GitHub's latest release, preparing shaders again.

Install needs your own extracted dump, a folder holding `exefs` and `romfs` with the 1.2.0 update applied. It checks
the version against `game-files.txt` before copying anything, copies the files in, then compiles the game code on
this PC, about 5 minutes. Your folder stays as it was. The launcher doesn't extract dumps and never needs keys.

The game's folder is `output/Bayo3_120`, or `Bayo3_120` beside this folder in a package. `B3_GAME_DIR` picks another one, which is how the tests stay off your real install.

## Tests

`test_launcher.py` checks the logic on made-up data. `diagnostics/launcher-install-20261101/` has the real Install tests and results. `test_launcher_live.py <test game folder> <picture folder>` drives the old window against a test copy of the game, and refuses `output/`. `diagnostics/release-0.1/test-fresh.py ... --web` runs a new player's Install through the new window's logic.

## Not done

There is no packaged single `.exe` yet, and the portable package doesn't contain this launcher yet. `code-hints.txt` lists addresses only, code that past runs found and the compiler can't, for version 1.2.0.

The old window's look and a few helper methods are adapted from the Bloodborne launcher; the new window's packages and fonts are listed in `ATTRIBUTION.md`.
