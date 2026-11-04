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

## Area entry: ASTC decode spread over frames (s1, s2)

The area entry at frame ~1082 decoded 228-234 Mpx of first-time ASTC in one frame. `--profile=1` with
`SNX_NVN_SPIKE_MS=60` (the spike threshold, 150 ms by default) showed the next frame's 103-148 ms before its first
window as every thread idle except the game's, spinning on `plat_ns`: waiting for the GPU's decode. The user chose to
spread the decode (2026-11-04) over a texture pack at install or a faster decoder.

`texture.c`: a first upload of an ASTC texture with more than two levels and at least 512 x 512 decodes from level 2
down (1/16 of the work) and is drawn through views starting there (`Surf.astc_base`); its top two levels wait in a
queue, decoded at each present up to `SNX_ASTC_SPREAD_MPX` (16 Mpx by default, 0 turns it off). The views are
remade when the texture is whole.

Warm fight, same build, no screenshots (screenshots cost about 100 ms each and made the first try unreadable):

| | spread off | spread on |
|---|---|---|
| ASTC in the area-entry frame | 233.6 Mpx | about 17 Mpx |
| area-entry frames | 92 + 157 ms | 129 (93 of them a pipeline wait, not textures) + 54 ms |
| slow frames after 30 s, time lost | 17, 1,010 ms | 13, 708 ms |
| over 33.4 ms at 60 | 23 | 20 |

The route's area entry fades in from black, and the top levels are all in by about 17 presents (frame 1092 already
sharp in the screenshots). In-play texture loads (frames 3500-6000) spread over 3-39 presents with nothing waiting.
Not seen yet: a texture drawn soft in full light, and shaders that read a texture's size (they would see the smaller
level for those few frames).

## Builds

l1 to l11 and s1, s2 in this folder (not tracked). l10 (`7c183773...`): compute preparation, the interval-aware 1% low
and the logs, committed as `bf7496c`. s2 (`9a46b5ae...`) = l10 + the spread decode + `SNX_NVN_SPIKE_MS`. Not installed.
