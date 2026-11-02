# Bayonetta 3 launcher

Start it with `Launch-Bayonetta3.cmd`. It uses the Python in `launcher/python`, so nothing needs installing.

- **Play:** shows whether the game is ready, then starts it on the fast renderer. It sets the renderer's switches itself.
- **Graphics:** your screen and a quality. Smooth 60 is the recommended one. These are saved in the game's `Bayo3_120.ini`, which the game's own menu also uses, with every comment kept. The first version of that file stays beside it as `Bayo3_120.ini.before-launcher`.
- **Controller:** swap A and B, stick dead zone, vibration.
- **Install:** your own extracted dump, a folder holding `exefs` and `romfs` with the 1.2.0 update applied. It checks the version against `game-files.txt` before copying anything, copies the files in, then compiles the game code on this PC, about 5 minutes. Your folder stays as it was. The launcher doesn't extract dumps and never needs keys. It refuses a game that's already installed. If the game's files are there but its code isn't, the button is **Build game code**. A compile that stops leaves the game as it was, and the next one starts again from the beginning.
- **Sessions:** the last session's smoothness in plain numbers. Each launch keeps the previous log in `logs/`, the newest 8.

The game's folder is `output/Bayo3_120`, or `Bayo3_120` beside this folder in a package. `B3_GAME_DIR` picks another one, which is how the tests stay off your real install.

## Tests

`test_launcher.py` checks the logic on made-up data. `diagnostics/launcher-install-20261101/` has the real Install tests and results. `test_launcher_live.py <test game folder> <picture folder>` drives the real window against a test copy of the game, and refuses `output/`.

## Not done

There is no packaged single `.exe` yet, and the portable package doesn't contain this launcher yet. `code-hints.txt` lists addresses only, code that past runs found and the compiler can't, for version 1.2.0.

The look and a few helper methods are adapted from the Bloodborne launcher: see `ATTRIBUTION.md`.
