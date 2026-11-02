# Freezes (from 2026-11-01)

Plan and Witch Time's design: last section of `../witch-time-study-20261101/README.md`. VRAM work before this: `../vram-20261101/README.md`.

**Build: `Bayo3_120-f5.exe` (`8efcbbf3...`), not installed.** Source is phase5i plus the memory log, BC3, sparse copies, pipeline timing, pipeline libraries and shader preparation. Edits in order: `edit-1.py`, `edit-2.py`, `edit-3.py`, `edit-4.py`, `edit-5.py`, originals in `pre-edit-*/`. `Bayo3_120-f1.exe` to `f4` are records of the steps. `edit-3m.py` was a measure, taken back out.

All runs isolated, scripted fight `route-remnant9.txt` at 2560x1440 unless noted, O1 DLL, RTX 3080, driver 621.01. Saves unchanged in every run.

## Summary

| fight | pipelines made | waits total | longest wait | worst gameplay frame | frames over 250 ms |
| --- | --- | --- | --- | --- | --- |
| first run before (c1a-d) | 669-714 | 29.5-32.7 s | 2,288 ms | 4,430-5,110 ms | 20-25 |
| first run, libraries (c2a) | 674 | 27.1 s | 2,241 ms | 4,330 ms | 23 |
| first run, libraries, background preparation (c4a) | 682 | 24.1 s | 2,254 ms | 4,192 ms | 23 |
| **first run after the pass (c5a)** | 717 | **0.51 s** | **42 ms** | **412 ms** | **6** |
| second run (c5b) | 687 | 0.16 s | 44 ms | 260 ms | 2 |

A frame of about 1,100 ms stays at frame 11, during boot, not in play. GPU time 9.31-9.38 ms against 9.41 before, FPS windows 59.1-59.7. Frames over 250 ms that remain aren't pipelines: their time is before the first window, on the game's side (`frame N took` lines). One run each after the pass.

## Step 1: timing, and the queue idea that was wrong

Every pipeline now logs queue and compile time. Hypothesis: a waiting draw sits behind hundreds of others. **Wrong for this route:** most queued 3-6, usually 0 ahead of a waiting draw. The cost is the compile: 60 ms on average, and three pipelines of about 2.2 s alone, on 3-vertex full-screen draws.

**Correction:** `route-run.py` strips `SNX_NVN_*`, so `SNX_NVN_PIPE_FRONT=0` never applied and c1a to c1d all had the front move on. That was not an A/B. The conclusion rests on the queue depths in the logs, not on that comparison. Fixed: `RUN_PIPE_FRONT` and `RUN_PREP` now pass through. `SNX_NVN_PIPE_WORKERS` and the front move stay in the source but were never measured off. The user's play made about 1,860 pipelines, so its queue may have been deeper.

## Step 2: pipeline libraries

`VK_EXT_graphics_pipeline_library`, four libraries per pipeline, only where the driver has fast linking. `SNX_VK_GPL=0` off. Our own code, Witch Time's design without extended dynamic state 3, as blend sits in the output library, which has no shader. About 680 pipelines became 110 vertex and 206-210 fragment libraries, 35 inputs, 33 outputs. Links at most 0.2 ms. Fragment stages take 28.7 s of the compile time, vertex 5.6 s.

No link time optimisation. Picture and GPU time: street, c6c (libraries) against c6a (whole pipelines), 0.8-6.6/255 against 1.4-2.5/255 between two library runs, the 6.6 being one frame of the scene's motion. GPU 20.5-21.1 ms late in all three, 40.6-41.6 FPS.

## Measure for compiling ahead

`edit-3m.py`, run c3b: 328 variants, 7,896 stages set. Variants made a median of 78 s after the game gave the stage, 292 of 327 by 10 s or more. Fragment: 204 of 212 keys empty. Vertex: all with clip 0xFF, 76 with `int_mask` 0 and 39 with 4 (attribute 2). Translation is cheap: 72 ms for all 328.

## Step 3: background preparation

Workers below normal priority, half the threads, pausing at frames of 200 draws or more. Harmless warm (frames over 50 ms 175, GPU 9.42 ms) but too slow in play: 1,441 of about 5,300 stages in the first run, so waits only went from 27 to 24 s. It stays on, for whatever a pass hasn't covered.

`stage_spirv` was split out of `vk_nvn_shader`. A worker compiles a library and destroys it at once, so it never touches the tables draws use: a wrong guess costs what it cost before. The bytes are hashed when given and again before translating, and changed code is skipped ("gone"). Hashes done are kept in `shader_cache/prepared_<driver>_<mode>.bin`.

## Step 4: the preparation pass

`SNX_NVN_PREP_RUN=1`: all threads but two, normal priority, no pause. The game sits at its start with no input, gives its 7,764 stages within about 30 s, and exits 20 s after the last one. **171 s, 5,315 unique stages, 14 workers. Vulkan cache 266 MB** (was 49), no boot delay seen. Saves unchanged.

## Step 5: launcher

Play checks `shader_cache/prepared.txt` against `gpu.txt` (driver, scale mode). If a pass is due it runs it, shows a percentage, then starts the game. Closing the window keeps progress. `test-launcher-prepare.py` drove the real window on the isolated copy from empty caches: pass and hand-over 166 s, game alive 40 s later, next run nothing to do, production saves unchanged. `test_launcher.py` and `--smoke-test` pass. An older exe without the pass just starts (`can_prepare`). After that test: the percentage waits for 1,000 stages and that guard was added, covered by logic tests and page build only. Not seen by the user.

## The user's play of Remnant 10 on f5 (2026-11-01, 21:37-21:43)

Log kept in `play-f5-20261101/`. 334 s, 2560x1440 from 86 s, no crash. The user liked the VRAM work. Against the matched run on phase5i, combat taken as windows of 900+ draws, 16 windows each:

| | matched, phase5i | f5 |
| --- | --- | --- |
| process video memory | game about 7.4 GB (other tool) | **peak 2,491 MiB** |
| combat FPS | 49.8 | 51.0 |
| combat frames over 33.4 ms | 107, 1.11% | 84, 0.88% |
| mean draws | 1,237 | 1,268 |
| freezes in play over 1 s | 3 (1,045, 1,051, 2,469 ms) | 1 (2,454 ms) |
| freezes in play of 200-999 ms | 4 | 4, longest 233 ms |
| GPU ms in combat windows | | 13-20 |

Boot frame 12 took 1,484 ms, not in play.

**How it was started:** not by the launcher's Play. `shader_cache` has no `prepared.txt` and `logs/` has no new session. Background preparation still did all 5,320 stages in the first 28 s at 6.6 ms each, as NVIDIA's own disk cache held them from the isolated runs, and the Vulkan cache grew from 75 to 329 MB. So a new PC can't expect this speed.

**The 2.4 s freeze is one pipeline, the same one in both runs:** 54 draws, 118 texture lookups, a draw of 3 waiting 2,437 ms here and 2,448 there, just after the fight's draws fall away. Every stage was prepared, so the draw asked for a key preparation doesn't build. That is the known gap, 8 of 212 fragment keys. Not known which key. The result was saved at exit, so it may be gone on this PC. Unproven, and a new PC would hit it.

**Combat FPS is still CPU limited:** in combat a frame is before the first window 5-8 ms, windows 9-12, after the last 3-4, so about 19-21 ms, with 0-2 ms held by the pacer. GPU 13-20 ms comes close but isn't first. Freezes are mostly solved, so recording cost and a render thread are next, as planned.

## Not known

- Other chapters. All 7,764 stages arrived at boot, which suggests the whole game, but only this route and the street were run.
- Tessellation and geometry stages and compute aren't prepared.
- Keys outside the prediction (8 of 212 fragment here), and depth clamp on: libraries prepared with it off, and `k->depth_clamp` wasn't measured.
- Changing between scale 1 and any other, or a driver update, needs a new pass.
- Laptop and AMD. On a 12-thread CPU expect longer.
- Your hand-played Remnant 10 at 1,500 draws: CPU and GPU time there are untouched by this work.
