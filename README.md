# B3-Express

[![Latest release](https://img.shields.io/github/v/release/LuvAbyss/B3-Express)](https://github.com/LuvAbyss/B3-Express/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/LuvAbyss/B3-Express/total)](https://github.com/LuvAbyss/B3-Express/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

Bayonetta 3 (v1.2.0) running natively on Windows. The game's code is recompiled on your own PC by [ScarletNX](https://github.com/scarletnx/ScarletNX), a static ARM64 to x86 recompiler, and a renderer on Vulkan draws it. The goal is a smooth 60 FPS and a launcher anyone can use, not higher frame rates.

**No game files are included, and none ever will be.** You need your own legally dumped copy of the game.

This is an undergraduate's graduate project.

## Download

**[Get the latest release](https://github.com/LuvAbyss/B3-Express/releases/latest)** and follow the [installation guide](docs/INSTALL.md).

What each version changed is on the [releases page](https://github.com/LuvAbyss/B3-Express/releases). Found a bug? The launcher's Help page makes a report to attach to an [issue](https://github.com/LuvAbyss/B3-Express/issues).

## What you need

- Windows 10 or 11, 64-bit
- A graphics card with a Vulkan 1.3 driver
- About 20 GB of free disk space
- Your own Bayonetta 3 dump, updated to v1.2.0 and extracted to a folder holding `exefs` and `romfs`

The launcher doesn't extract dumps and never asks for keys.

## What it does

- **One button.** The launcher installs from your extracted dump, compiles the game code for your PC, prepares shaders for your graphics card, then plays.
- **Controller, or keyboard and mouse.** Every key can be changed, and the game's button prompts follow what you play with.
- **Graphics options.** Window size and quality presets, sharper textures, highest model detail, film grain off, and widescreen at 16:10, 21:9 and 32:9 (experimental).
- **Mods.** File replacements and code patches in the layouts made for yuzu, Ryujinx and Atmosphère. Your installed game files are never changed.
- **Saves.** Back up and restore from the launcher.
- **Updates.** The launcher tells you when a new version is out and updates itself with one button. Your game files, saves, settings and mods stay.

Measured results, known problems and what's left are in [docs/STATUS.md](docs/STATUS.md).

## Updating

Press **Update now** in the launcher when it says a new version is out. If your version has no such button yet, unzip the new version to a new folder, run Install again from your extracted dump, and copy the `save` folder from `Bayo3_120\Bayo3_120_data` of the old folder into the same place in the new one.

## Building it yourself

1. Build the runtime from `source/` (see `source/docs/BUILD.md`).
2. Extract your own 1.2.0 dump into a folder holding `exefs` and `romfs`.
3. Start `B3-Express.exe` (built by `launcher/exe/build.py`), then Install on that folder.

The launcher's bundled Python isn't in this repository. Releases include it.

| Folder | What |
|---|---|
| `source/` | ScarletNX at commit `5c97949`, with this project's changes. See `source/README.md` |
| `launcher/` | The launcher: install, compile, settings, mods, saves, updates |
| `docs/` | The installation guide and the project's status |
| `diagnostics/*/README.md` | The notes behind each measurement. Logs and captures stay local |

## Credits

Thanks to the ScarletNX contributors, who are happy to see this project. The detail options and widescreen are ported from [Witch Time](https://github.com/Zexyen/Witch-Time), with thanks. Witch Time and B2recomp are also references and a comparison, see `source/docs/CREDITS.md`. `launcher/ATTRIBUTION.md` lists what the launcher adapts.

## License

GPL-3.0-or-later, as ScarletNX. See `LICENSE`.

This project is not affiliated with Nintendo, SEGA or PlatinumGames.
