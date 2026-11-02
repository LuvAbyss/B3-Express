# Launcher Install (2026-11-01)

Originals: `pre-edit/`. Review fixes: `edit-review.py`.

## Decision

The user decided that players bring an extracted dump. Extraction would mean asking for `prod.keys`, so the launcher has no extractor and never asks for keys. An extractor version was built and tested first (below). The project's exe still has `-x`, the launcher just doesn't use it.

## How Install works now

Pick a folder holding `exefs` and `romfs`. The version is checked against `game-files.txt` (project root; size of every file, SHA-256 of `exefs/`) before anything is copied. Files go through `_copy` and are moved in whole. The compile runs in `_build` with `scarletnx -r`: only ExeFS as input, so the 15 GB RomFS isn't copied, and a runtime folder with no `*_code.dll`, as `copy_dlls` would put one over the new module. `code-hints.txt` is merged with the game's own `.hints`. The `.abi` stamp goes in last. `-O 1`, threads minus two, this PC's CPU. `scarletnx.exe` and `hactool.exe` aren't needed in the game's folder.

## Results

All in a throwaway `fresh/` folder, f5, Ryzen 7 5700X3D (16 threads, 14 jobs).

| test | result |
| --- | --- |
| extractor version, real XCI and update through the real window | 223 s: extract 44 s, compile 179 s. Peak compiler memory 3,539 MB |
| all extracted files against the install | 557 hashed, 0 different, 0 extra |
| Play on that install, no caches | pass and hand-over 197 s, alive 60 s, no crash lines, no `.hints` written |
| Build game code button, window closed at 10%, then a whole build | no compiler left, not ready, then ready. Same module |
| folder Install: wrong folder, same-size wrong version, good folder | refused, refused before copying, 189 s to ready, Play on, source kept, same module |
| module | `8a8d70c0...`, 249.9 MB, 1,125,557 entries (1,125,205 without hints), same bytes in every build |
| folder Install again after the review's fixes | 196 s to ready, same module. `fresh/` was then deleted |

"code 1" in `result-play.json` is the test killing the game.

## Hints A/B: no gain

A function the recompiler missed runs in the interpreter and goes in `.hints`. The installed module was built without them, so the idea was that the 351 known ones cost CPU time. Scripted fight, 2560x1440, f5, warm, alternating, after an uncounted warm-up. Both modules O1, this CPU.

| run | module | frames over 33.4 ms | before the first window ms | recording ms | GPU ms | worst ms |
| --- | --- | --- | --- | --- | --- | --- |
| la1 | `ryzen-o1` | 559 of 11,999 | 10.39 | 5.03 | 10.53 | 1,160 |
| lb1 | with hints | 554 | 10.65 | 4.92 | 10.87 | 1,094 |
| la2 | `ryzen-o1` | 535 | 10.74 | 4.88 | 10.46 | 1,117 |
| lb2 | with hints | 536 | 10.61 | 4.92 | 10.46 | 1,080 |

No difference above run-to-run spread. **Wrong hypothesis for this route:** no reason to replace the module in `output/`. Hints stay in builds, as they cost nothing. Not measured: Remnant 10 at 1,500 draws.

## Review

A separate pass found three serious faults, all fixed with tests: a failed clean of `_build` left the page stuck on Compiling, a read-only dump made a finished build report failure, and Play could be enabled during a compile. Also fixed: `_copy` counting against free space, half a game after a failed move, raw `shutil.Error` text, a compiler left running after a log write failure, a source inside `_copy`, a missing exe found late. Install now refuses while the game or a compile elsewhere runs.

Left open: `control.nacp` isn't installed, so the title's version reads 1.0.0 and save size falls back to 64 MiB, the same as `output/` today, which has none. A stale ABI after a future `SNX_ABI` bump shows ready and Play then stops with an error. There is no single-instance guard beyond the compile check. No test forces a real file lock on Windows.

## Not tested

A dump extracted by another tool. A 12-thread CPU or 16 GB of RAM. A package: none exists with this launcher yet.
