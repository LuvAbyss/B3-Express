# 1% low, 2026-11-04

The user asked for a higher and steadier 1% low. Everything here ran in the isolated copy on the fight route
(`diagnostics/nvn-phase5-20261030/route-remnant9.txt`, 6000 frames, 1.6x resolution, fast renderer).

## Tools

- `cold_fight.py <exe> <label> [frames]`: sets the shader cache aside, runs the launcher's shader preparation pass,
  then the fight, and prints the numbers; the cache and `isolated-runtime.exe` are put back whatever happens.
  `RUN_KEEP_CACHE=1` is a warm run, `RUN_NO_PREP=1` skips preparation, `RUN_SNX="K=V;..."` adds switches.
- `slow_frames.py <log>`: each "frame N took" line (50 ms and over, `SNX_NVN_SLOW_MS` to change) by its largest cause.
- A **truly cold** run also needs NVIDIA's own shader cache empty (`%LOCALAPPDATA%\NVIDIA\GLCache`, 693 MB here):
  `RUN_SNX="__GL_SHADER_DISK_CACHE_PATH=<empty dir>;__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1"`. Without it, emptying
  our cache alone still finds every compile in the driver's, and the run looks warm. A new player has neither.

## What the old numbers counted

283 of the fight's 287 frames at about 33 ms came while the game ran at its own 30 FPS (swap interval 2: loads,
in-engine scenes). The 1% low and "frames over 33.4 ms" counted them as stutter. Now only frames paced at
interval 1 count (`g_pace_interval`, `present.c`), and the pace line says how many were left out
("at the game's 30 FPS or less"): about 910 of 6000 on this route. Over-33 went from about 500 to about 25.

## Truly cold, fight route

| run | pipeline waits | over 33.4 ms (at 60) | worst frame |
|---|---|---|---|
| no preparation, l2 | 17.3 s | 57 | 4.9 s |
| no preparation, re-wait every frame (old) | 16.9 s | 74 | 3.8 s |
| preparation first (launcher Play), l3 | 0.47 s | 30 | 1.1 s |
| preparation with compute stages, l5 | 0.51 s | 30 | 1.1 s |

- **Waiting once for a compiling pipeline** (l1, then let draws skip it) changed nothing that matters and left out
  more draws (82 against 49). Removed.
- **Compute pipelines** are made on the draw thread with no budget (`vk_nvn_compute_pipeline`), and preparation
  skipped compute stages (`cmdlist.c`). Cold, 8 of them cost 150-185 ms each, 1,074 ms in all, which were the five
  200-290 ms "renderer other" frames. Preparation now does them (74 more stages, 5,389): 8 to 1 (17 ms, a keyed
  variant), and the five frames are gone. Lost time in slow frames after 30 s, 2.3 to about 1.1 s.
- l4 got this wrong first: the worker's re-check of a stage's bytes left out the new compute term, so all 74 counted
  as "gone before their turn". `prep_hash` is now the one function both use. Vertex and fragment hashes are as
  before, so earlier `prepared_*.bin` files stay valid.

## What preparation still misses (about 10 compiles, 0.6 s on this route)

`SNX_NVN_PREP_LOG=1` logs each prepared stage's SPIR-V hash; the slow-compile lines name the draw's.
- Six vertex stages, all in depth-clamp draws (shadows). Their SPIR-V was never prepared at all, so a depth-clamp
  copy of the library (tried in l7) didn't help: removed. Why their translation differs is open.
- Four fragment stages with an "unfed" key (inputs the bound vertex stage doesn't write). Draws pair stages from
  different programs, so pairing within a program (l7) never produced one: removed.
- 408 stages are gone before a worker gets to them (the game reused the memory). They now leave the seen set, but
  the game didn't give them again on this route.

Left for the 1% low: ASTC at area entry (first two windows, 233 Mpx in one frame), game CPU frames, and these misses.

## Builds

l1 to l10 in this folder (not tracked). l10 (`7c183773...`) is the candidate: compute preparation, the interval-aware
1% low, and the logs. Not installed.
