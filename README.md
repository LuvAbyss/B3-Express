# B3-Express

An undergraduate graduate project: Bayonetta 3 v1.2.0 running natively on Windows through [ScarletNX](https://github.com/scarletnx/ScarletNX), a static ARM64 to x86 recompiler. The game's code is recompiled on your own PC, and an NVN-level renderer on Vulkan draws it. The goal is butter smooth 60 FPS and a friendly launcher, not higher frame rates.

**No game files are included, and none ever will be.** You need your own legally dumped copy of the game.

Current state, measurements and what's left: [docs/STATUS.md](docs/STATUS.md).

## New in 0.2.5

A big release, so it skips ahead from 0.2.0 to 0.2.5. Download it from [Releases](https://github.com/LuvAbyss/B3-Express/releases).

- **Mods.** A new Mods page in the launcher adds a mod from its folder or its zip, turns mods on and off, sets their order and removes them. Mods that replace game files (a `romfs` folder) and code patches (`.ips` and `.pchtxt` for v1.2.0) both work, in the layouts made for yuzu, Ryujinx and Atmosphère. Your installed game files are never changed, and turning a mod off brings the originals back. Turning code patches on or off compiles the game code again at the next Play, which takes about 3 minutes.
- **More detail.** New options for sharper textures in the distance, every model at its finest level of detail, and the game's film grain off. In our tests fine detail went up by 11 to 49 percent at no measurable cost in frame rate.
- **Widescreen (experimental).** The game's picture at 16:10, 21:9 and 32:9 instead of black bars. Wider screens show more at the sides, 16:10 shows more above and below, and the HUD sits at the screen's edges at its normal size (or in the middle, your choice). Movies keep their 16:9 shape with black bars, as on a TV. A few menus can still end short of the screen's edges.

The detail options and widescreen are ported from [Witch Time](https://github.com/Zexyen/Witch-Time), with thanks. As before, updating means unzipping the new version and running Install again.

## Layout

| folder | what |
|---|---|
| `source/` | ScarletNX at commit `5c97949`, with this project's changes. See `source/README.md` |
| `launcher/` | the launcher: install from an extracted dump, compile, settings, play |
| `docs/STATUS.md` | what works, measured results, what's left |
| `diagnostics/*/README.md` | the notes behind each measurement. Logs and captures stay local |

## Use

Players: download a release and follow [docs/INSTALL.md](docs/INSTALL.md). To build it yourself:

1. Build the runtime from `source/` (`source/docs/BUILD.md`).
2. Extract your own 1.2.0 dump into a folder holding `exefs` and `romfs`.
3. Start `B3-Express.exe` (built by `launcher/exe/build.py`), then Install on that folder. It checks the version against `game-files.txt`, copies the files and compiles the game code on your PC. It never asks for keys.

The launcher's bundled Python isn't in this repo. Releases include it.

## Credits

Thanks to the ScarletNX contributors, who are happy to see this project. [Witch Time](https://github.com/Zexyen/Witch-Time) and B2recomp are references and a comparison, see `source/docs/CREDITS.md`.

## License

GPL-3.0-or-later, as ScarletNX. See `LICENSE`. `launcher/ATTRIBUTION.md` lists what the launcher adapts.
