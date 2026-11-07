# Mods, build m1 (2026-11-07)

`Bayo3_120.exe` here is m1 (`af360cd1...`): the 0.1.7 runtime source (`main` at `1a7505f`) plus RomFS mod loading
(`source/src/nx/hle/filesystem/fs.c`), built in `source/build/desktop-isolated`. Not installed, uncommitted.
The launcher's Mods page (`launcher/ui/index.html`, `web_launcher.py`, `bayonetta_launcher.py`) manages the folder.

## Layout the game reads

```
Bayo3_120_data/
  romfs/                 the installed game, never changed
  mods/
    mods.txt             names of the mods turned on, one per line, '#' comments; a later line wins
    <mod name>/romfs/... files at the same paths as in romfs/ replace them; new paths are added
```

The same layout as yuzu's and Ryujinx's `<mod>/romfs/`. The log says `mods: <name>: N files replaced, M added`,
and warns for a mod with no `romfs` folder, a name that is a path, and a file where the game has a folder.
Paths are case-sensitive, as on the Switch: the launcher flags mod files that differ from the game's names only in
capitals, because Windows would hide that.

## Tested

- `ctest -R filesystem`: tree, service, raw and the new `filesystem_mods` pass.
- `launcher/test_launcher.py`: listing, order, overlaps, capitals, import from folder / Atmosphère zip / bare romfs,
  refusals (several mods in one zip, no romfs, paths escaping the zip, the game's own folder), remove, report line.
- The page in headless Edge with a stand-in API: renders, no script errors, a move saves the new order.
- Game, isolated copy, 45 s at the title (`desktop-launch-20261028/mods-m1-a`, baseline `mods-m1-off`): an identity
  mod (copies of `Text_000.pkz`, `chapter_000.pkz`, plus one new file) logged 2 replaced, 1 added; the missing mod
  in `mods.txt` was warned about; title drew normally (`title-with-identity-mod.png`); exit 0; saves unchanged; same
  frame count as with no mods. The isolated copy was put back afterwards.

## Not tested

- A real mod that changes what's on screen.
- The real window's file dialogs (the user runs the window).
- Large mods: every file on is mapped at start, like `romfs/`.

## Next

- Ship m1's runtime in a release (0.2.0 has 0.1.7's, and its Mods page says the game can't load mods).
- Mods that bring new shaders compile them during play; shader preparation doesn't know them.
