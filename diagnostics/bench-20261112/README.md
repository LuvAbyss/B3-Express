# Version benchmark: 0.1.7, 0.2.5, 0.2.6 (2026-11-12)

`bench.py` runs the same fight on each build in the isolated copy, as a new player meets it: an empty shader cache,
the launcher's shader preparation pass, then the fight route (`route-remnant9.txt`, 6000 frames) twice, the first
right after preparation and one warm. The isolated copy's own build and cache are put back afterwards. Builds:
0.1.7 = `install-20261107/preserved-installed` (runtime `4aa1d57c`), 0.2.5 = `install-20261110/preserved-installed`
(`1ffcc581`), 0.2.6 = `output/Bayo3_120` (`140dbc98`, the same code as the release's `14fcdaf3`). Older releases
have no runtime and game code left on disk. RTX 3080, Ryzen 7 5700X3D, resolution 1.6, nothing else on the GPU.
All 12 runs exit 0, saves unchanged. Run logs: `desktop-launch-20261028/bench-0xx-*`.

A hitch is a frame of 50 ms or more once the frame pacer's own hold is taken out, after the first 300 frames and
before the timed exit's last frame (that one is about 115 ms in every run: the exit, not play).
`BENCH_REPARSE=1 bench.py` makes the table again from the runs already made.

| Build | Run | Frames at 60 | Over 33 ms | Hitches >=50 ms | Longest | 1% low (median window) | 1% low (worst window) | Pipeline waits | VRAM peak |
|---|---|---|---|---|---|---|---|---|---|
| 0.1.7 | 1 (first after preparation) | 5083 | 22 | 8 | 69.5 ms | 50.0 FPS | 13.7 FPS | 154 ms | 2326 MiB |
| 0.1.7 | 2 (warm) | 5087 | 20 | 5 | 65.2 ms | 47.6 FPS | 13.0 FPS | 106 ms | 2435 MiB |
| 0.2.5 | 1 (first after preparation) | 5098 | 23 | 9 | 68.9 ms | 47.6 FPS | 13.5 FPS | 202 ms | 2311 MiB |
| 0.2.5 | 2 (warm) | 5087 | 20 | 6 | 69.6 ms | 50.0 FPS | 13.7 FPS | 107 ms | 2230 MiB |
| 0.2.6 | 1 (first after preparation) | 5086 | 20 | 10 | 63.2 ms | 50.0 FPS | 13.9 FPS | 138 ms | 2320 MiB |
| 0.2.6 | 2 (warm) | 5103 | 20 | 6 | 63.6 ms | 47.6 FPS | 14.5 FPS | 105 ms | 2197 MiB |

Shader preparation: 5393 to 5399 stages in about 26 s on every build.

## Reading

**The three releases perform the same on this fight.** Every difference is within one run's noise. That fits:
0.2.x added widescreen, mods, chapter 14 and gallery fixes, nothing on this route's hot paths. The frame-pacing work
that made the big difference (0.1.7's present thread) is already in the oldest build here.

**The hitches are the same frames in every build** (warm runs of 0.1.7 and 0.2.6, frame numbers within one), so they
come from the route, not from chance, and none of the releases touched them:

- **New textures (frames 2511-2512, 3213, 3536, 5948):** 50-75 ms, of which 38-61 ms in the windows, 15-35 ms of
  that in texture lookups (258-726 misses in one frame) and 3-19 Mpx of ASTC decoded. An area or enemy wave's
  textures are made on first use, on the render thread, under `vk_lock` (`nvn_res.c` texture cache miss:
  `vk_tex_from_tic`). Shader preparation can't help, a warm run has them again because textures aren't kept between
  runs.
- **After the last window (frames 1082, 4622):** 63-78 ms, about 50-60 ms after the frame's last draw with 45-113
  draws: the game's own work at a transition (loads), not rendering. Unmeasured.
- **Worst 5 s window, 13-14 FPS in every run:** not looked at yet.

## Next

1. Texture first use: time `vk_tex_from_tic`'s parts on frames 2511 and 3536 (guest reads and hashing, ASTC
   dispatch, upload, view and sampler creation), then move the slow part off the render thread or ahead of the
   draw that needs it. `bench.py` before and after gives the number.
2. The two transition frames and the worst window: a profiled run (`SNX_PROF_CALLERS=1`, `SNX_CORE_STATS=1`).

## Texture first use, measured (2026-11-12, runtime with timers, not installed)

New timers in the slow frame line (`texture misses: ...`, `vk.h` `VK_TP_*`) and a log line for each staging ring
growth. Warm fight runs `bench-t1`..`t3`, `bench-host1` (0.2.6 game code):

| Frame | Took | Windows | Texture misses | of which upload | staging (MB) | hash | image |
|---|---|---|---|---|---|---|---|
| 2512 | 63 ms | 48 | 20 | 12.6 | 8.5 (47.7) | 5.6 | 1.0 |
| 3214 | 54 ms | 42 | 27 | 20.3 | 11.8 (39.5) | 5.0 | 0.9 |
| 3536 | 72 ms | 49 | 26 | 9.9 | 7.9 | 3.3 | 12.3 |
| 5948 | 65 ms | 51 | 17 | 11.5 | 9.2 (47.0) | 3.3 | 1.8 |

- ASTC is not it: its staging and dispatch take 1-4 ms, and the 400 Mpx of first ASTC uploads in a 10 s window
  cost 17 ms in all (`astc-ahead`).
- The upload's time is the non-ASTC textures' deswizzle into staging: about 47 MB at 5.6 GB/s. Staging ring growth
  is under 1 ms (logged). Staging in system memory instead of behind the BAR (`SNX_VK_STAGE_HOST=1`, test switch):
  7.2 ms for the same 47.7 MB, so the BAR isn't the limit, the CPU's scattered reads are.
- Texture misses are 17-27 ms of the 42-55 ms in the windows. The other 20-30 ms in the windows on those frames is
  not yet placed (frame 2513 has 4 ms of misses and 55 ms of windows).
- Every first ASTC upload was registered with the game 30+ frames before its first draw (`astc-ahead`), so a
  texture's deswizzle, hash and upload could be done ahead on a worker thread.

## The whole hitch, accounted (run `bench-t5`, timers in `cmdlist.c` and the replay)

The slow frame line now also splits the windows by kind (`windows by kind: ...; lists X, of which draw setup Y,
dispatch setup Z`). Draw setup is `cmdlist.c` `draw_pipeline`: the pipeline key, whose `vk_nvn_tex_key` makes a
new texture on its first lookup, and the replay's draw.

| Frame | Took | Windows | Lists | Draw setup | of which texture misses | replay draws (pipeline waits) | other |
|---|---|---|---|---|---|---|---|
| 2512 | 64 | 50 | 50 | 42 | 22 | 13.5 (5.7) | mirror 0.3 |
| 3213 | 54 | 41 | 41 | 31 | 28 | 2.9 | dispatch setup 9.3 |
| 3272 | 51 | 39 | 39 | 35 | 22 | 10.9 (2.4) | |
| 3536 | 51 | 38 | 37 | 23 | 17 | 4.1 | render targets 12.7 |
| 5948 | 66 | 52 | 52 | 37 | 18 | 10.6 (6.6) | render targets 7.7 |
| 2513 | 60 | 42 | 33 | 24 | 4 | 10.0 (7.0) | mirror 8.8 |
| 3788 | 52 | 46 | 25 | 12 | 2 | 6.1 | mirror 20.4, dispatch setup 8.5 |

A normal frame's windows take 9-12 ms. On the hitch frames the extra is mostly **textures on first use (17-28
ms)**, then render targets made (8-13 ms), the mirror pass at a window's opening (9-20 ms on two frames) and pipeline
waits (2-7 ms; 47 and 99 ms on two frames of this run that no other run had: compiles the cache missed).

## The hitches are the game's dynamic resolution steps (2026-11-12, run `bench-tm1`)

`tex-made:` lines (textures of 1 MB and more made at a draw) show what the hitch frames load. The scene's render
buffers at `6550d0000`, `6548e0000`, `652fa0000`, `64f7a0000` and about 10 more are made again at a new size each
time the game's dynamic resolution steps: presents 939 (1440x816), 2510 (1600x896), 3212 (1440x816), 3535
(1280x720), 4209, 5221 (1280x720), 5948 (1360x768), 5952 (1024x576). Each step makes 9-27 of them, 38-71 MB, read
from guest memory and deswizzled on the CPU, though the game draws into them before it samples them ("render target
there: none" at that size). The hitch frames are the next present each time: 940, 2511, 3213, 3536, 5949. Sizes come
back (1280x720 three times), as the eviction drops a size not drawn for 10 s.

`passtime-20261102` already saw the controller hunting between 1024x576 and 1600x896 about every 2 s in combat on
this PC, with GPU time far below the console's.

**Prefetch** (texture descriptors made at the presents after their registration, `SNX_NVN_PREFETCH_MS`): runs
`off1`/`off2` against `pf7`/`pf8`, the same build: frames over 33 ms 22, 21 against 18, 18; hitches 9, 7 against 6, 8;
VRAM peak 2.1-2.4 GB against 3.4-3.5 GB; worst window 1% low 13.3, 13.9 against 10.1, 12.3. About 8% of the textures
it makes are drawn later, most are the game's whole set for an area. It can't reach the resolution steps: their
buffers at a new size are registered at boot at another size (the same address), and their memory is what the GPU
draws. Not worth 1 GB.

## The controller found: it is authored, not timed (2026-11-12 night, runs `bench-bt1`..`bt3`)

Read from the game's code (`tools/disasm.py`, runtime addresses, main at `0x8005000`) and checked in a run
(`SNX_NVN_DRS_LOG=1`, `drs-level:` lines). **Nothing in it reads a GPU or frame time.** The feedback loop guessed
above does not exist, and our timestamps are not its input.

- The graphics setup (`0x86a3c00`..) makes eight sets of scene targets at boot, one per level, over the same pool
  memory: 0 and 1 = 1600x896, 2 = 1504x848, 3 = 1440x816, 4 = 1360x768, 5 = 1280x720, 6 = 1024x576, 7 = 640x360
  (`0x86a55d8(.., level, w, h)`). No texture is initialized later (`bench-bt1`: 82 scene-sized
  `nvnTextureInitialize` at 1.44 s, none after).
- Each frame `0x86d8bb4` picks the level and points the renderer's "current target" globals at that set:
  `s = step + flag + k`, `idx = s < -3 ? 0 : min(s, step >= 3 ? 3 : 2) + 3`, level = `{1,1,2,3,4,5,6}[idx]`
  (table at `0x9840eb4`). So s = 0 is 1440x816 and s <= -2 is 1600x896.
  - `step` = `[G+0x4a34]`, G = `*(0xa4a14a8)`, copied each frame (`0x864c1e0`) from the atomic at `0xa4c7078`.
  - `flag` = `[X+0x16d4]`, X = `*(0xa4a3980)`: 0 or 1, set by game logic (`0x85a3388`: a height under -20, and
    object events at `0x882da64`).
  - `k` from bits 6 and 7 of `[*(0xa4a1388)+0x24]`, a game state word.
  - Overrides: `[G+0xd6a0]` (forced), `[*(0xa4a20d8)+0x330] == 1` (level 7), `[G+0x4a44]`, `[G+0x4a38]`.
- **`step` is written by placed scene objects** (`0x8654188`, their data read at `0x8654058`): a step of -3..3, a
  priority and a radius from the object's parameter block. While the point at `[obj+0x58]+0xc0` (the player or
  camera) is within the radius of the object (or the radius is 0), and its priority is at least the current one,
  it stores its step at `0xa4c7078` and its priority at `0xa4c7088`.
- A second scene-settings object (`0x8653c38`, applied at `0x8653f54`) sets a bias at `[G+0xd610]` (-1 in this
  route, 1 from present 3510 to 4622), which picks the level of some other per-level targets (`0x86d9220`).

The run agrees: step 2 at presents 3537, 4209, 5224 and 5646 for 27-33 frames each (1280x720), step 1 at 5948
(1360x768) and step 3 with priority 10 at 5952 (1024x576), back to 0 at 5984. The 1600x896 and 1440x816 changes at
939, 2510 and 3213 come with step 0, so from `flag` or `k` (game state: not logged yet). This is why the hitches
fall on the same frames in every run and every build, and it means the console steps at the same moments.

**What follows.** The steps are the game's own design, so the cost to remove is ours: at a step the next frame
samples 9-27 targets of the new set before anything has drawn them at that size, finds no render target of that
size (a size not drawn for 10 s is evicted, and the sets share memory), and builds each from guest memory, 38-71 MB
deswizzled, though that memory holds nothing the game drew (the host keeps render targets). Two ways:
1. Runtime: a texture over memory where the host has a render target of another size is not read from guest memory
   (cleared, or kept from the last time that size was drawn by not evicting the per-level sets). No game change.
2. Pin the level (a code patch at `0x86d8bb4` or on `step`): always 1600x896 or 1440x816, sharper, no steps at
   all. Needs the game code compiled again, and takes the game's own load relief away from weak GPUs.

## The step made cheap (2026-11-12 night, `nvn_res.c` `tex_get`, not installed, uncommitted)

The read at a step was never needed. `tex_get` made the renderer's texture from guest memory for every descriptor
(`vk_tex_from_tic`: read, hash, deswizzle, upload), and only afterwards did the draw's descriptor set put the
replay's own image in its place (`rp_target_of`: an image the replay drew at that address and size). So every
sampled render target had a second copy built from memory that no draw ever bound, and a resolution step built 9-27
of them at once. Now `tex_get` asks `rp_target_of` first and makes no texture when the replay has the image (`sub`
in the handle cache: the sampler as before, the null view for a draw after the image went). The pipeline key takes
the format from the descriptor in that case, the same values the texture gave it. `SNX_NVN_TARGET_TEX=1` is the old
way.

Runs `bench-new1` (fix) and `bench-old1` (`SNX_NVN_TARGET_TEX=1`), same build, both exit 0, saves unchanged.
**Another game was running on the GPU during both (36-38 % load), so their frame times are not a measure** and the
second pair did not start. What they do show:

- `tex-made:` (textures of 1 MB or more made at a draw): 293 before, 180 with the fix. At the step presents 3537,
  5948 and 5952: 11, 17 and 11 before, 0, 1 and 0 with it.
- The hitches at 3213, 3537 and 5949 of the old run are not in the new one's list of frames over 40 ms.
- Still there: 2510-2512 (17 + 5 real ASTC textures, 28 MB, an enemy wave's assets), 3213 (15, 31 MB, the same
  kind), 949, 1088 and 4624 (transitions). Those are textures on first use and the game's loads, not steps.

To do with the GPU free: two runs each way for the numbers, and screenshots at the same presents each way to show
the picture is unchanged.

## The step fix measured with the GPU free (2026-11-13 morning)

Same test build as above (runtime of 2026-11-12 20:49, staged folder, 0.2.6 game code), warm cache, one fight run a
label, the two ways in turn. All 15 runs exit 0, saves unchanged, GPU at 6 % before the start. "Fix" is the default,
"old" is `SNX_NVN_TARGET_TEX=1`. The build's prefetch is on unless `SNX_NVN_PREFETCH_MS=0`.

| Way | Prefetch | Runs | Over 33 ms | Hitches >=50 ms | 1% low (worst window) | VRAM peak |
|---|---|---|---|---|---|---|
| fix | on | `fixa1`, `fixa2` | 14, 14 | 5, 3 | 13.5, 10.8 FPS | 2924, 3023 MiB |
| old | on | `olda1`, `olda2` | 18, 19 | 7, 7 | 10.8, 10.6 FPS | 3346, 3345 MiB |
| fix | off | `fixb1`, `fixb2` | 18, 17 | 7, 6 | 12.2, 13.9 FPS | 2205, 2255 MiB |
| old | off | `oldb1`, `oldb2` | 22, 19 | 8, 7 | 13.9, 13.0 FPS | 2321, 2325 MiB |

Frames of 40 ms and more with the pacer's hold taken out (`shots-20261113/hitch.py`):

- **The step frames 2511, 3213 and 5948 are in all four old runs** (41-61 ms, 16-27 ms of texture lookups each) **and
  in no fix run**, but for 5948 once at 45 ms (`fixb2`, 3.8 ms of lookups: its pipeline wait and draws).
- Textures of 1 MB or more made at a draw: 289, 294 old against 177, 178 fix (prefetch off); 146, 150 against 38, 35
  (prefetch on).
- With the fix and no prefetch, 2512-2513 stays (48-53 ms in the windows: 22 real textures of an asset wave, a 14 ms
  mirror pass, 8 ms of pipeline waits). With the fix and prefetch, neither run has any renderer hitch left: only
  the game's own work after the last window (949, 1080, 4623, 50-62 ms) and pipeline compiles.
- Prefetch still costs about 700 MiB of VRAM (2.9-3.0 GB against 2.2-2.3 GB).
- **One compile of 80-114 ms in 6 of the 8 runs, both ways**, on frame 1081-1083, 3469-3470 or 4380 (`pipe` in the
  list). The evening runs had it too (`t5`, `old1`); the version runs above did not, with their fresh 248 MB cache
  from the preparation pass against the isolated copy's own 613 MB one here. Not looked into.
- `fixa1` has one frame of 236 ms (3896) with 226 ms in the windows, 59 ms of it in the lists, no texture or pipeline
  work, and a 43 ms wait before the first window 19 frames earlier. No other run has it. Not explained; it looks like
  a stall from outside the renderer.

**The picture.** Runs with numbered screenshots (prefetch off): `fixs1`, `fixs2` and `olds1` every 100 frames,
`fixd1`/`oldd1` every frame of 3530-3550 and `fixd2`/`oldd2` of 5940-5960 (the steps at 3537 and 5948-5952). A fight
does not replay to the pixel: two fix runs differ by a median 2.1 of 255 a shot (particles, enemies a few frames
apart), fix against old by 1.6, and the shots where the game shows the same thing are equal to the last bit either
way. So the check is by eye: `shots-20261113/fix-whole-route.png` (57 shots), `fix-steps.png` (the frames around
both steps) and three fix/old pairs show no black, missing or wrong-size image with the fix. A screenshot costs a
frame of 50 ms or more, so those runs give no timing.

The fix does what it was written for. Not installed, not committed.

## Cleaned for a commit (2026-11-13, runtime of 09:05, not installed, uncommitted)

The user's choice: the prefetch stays and is on by default. Taken out: the "made at a draw beside one made ahead"
log and `SNX_VK_STAGE_HOST`. `tex-made:` lines now need `SNX_VK_TEX_MADE_LOG=1` (`hitch.py` counts them). New: with
`SNX_NVN_PREFETCH_MB` unset, the textures kept ahead of their first draw are capped at an eighth of the video
memory budget (`device.c` `vk_mem_budget`), 1 GiB at most, so about 512 MiB on a 4 GB card. Not tried on a small card.

Runs `finc1` (with the log) and `finc2`, exit 0, saves unchanged: frames over 33 ms 14 and 13, hitches 4 and 4, VRAM
peak 3023 and 3279 MiB. No step or texture hitch in either. What is left is the game's loads (949, 1079, 4622) and
pipeline compiles of 50-101 ms (1079, 3471, 3791): the next thing to look at.

## The 50-114 ms compiles were the pipeline cache's own save (2026-11-13, fixed, not installed, uncommitted)

Every slow library in the runs above finished a multiple of 5 s after the start (cache loaded at 1.03 s, slow
libraries at 41.17, 81.17, 86.17, 96.17 s; loaded at 3.39 s in `fixb1`, slow at 43.54 and 83.53). That is the
frontend loop's save every 300 ticks. Timed in run `sav1`: `vk_pipeline_save` copied the whole cache out of the
driver in 100-106 ms and wrote it in 535-566 ms, 19 times in a two minute fight, 613320 KB each time, the same
size every time. The driver holds its lock on the cache for the copy, so a pipeline being made then waits for it:
the two slow vertex libraries of `sav1` end at 41.18 s, where the copy that began at 41.08 s ends. They were not
compiles at all. The save ran whenever a pipeline had been made since the last one, and a warm fight makes 15-150 a
10 s from a cache that already has them. A fresh 248 MB cache copies in 41 ms, which is why the version runs at the
top never showed a frame this long, and a player's cache grows with every session.

Now (`pipeline.c` `vk_pipeline_save(int last)`, the renderer's `save` takes the flag): the size is asked first, which
takes no time, and the cache is copied and written only when it is larger than as loaded or last written, and no
pipeline was made in the 5 s before. At exit it is written whenever it has grown. So pipelines made in a spell that
never pauses are only on disk at exit, and a crash before that loses them.

- Warm, `savw` (3 fights): no save, no slow library, 13 frames over 33 ms and 3 hitches in each, all three the
  game's own work after the last window (949, 1079, 4623). Pipeline waits 70-74 ms a run, against 75-280 before.
- Fresh cache, `savp` (preparation, then 2 fights): the preparation pass writes 247909 KB, the first fight saves 5
  times as the cache grows to 248389 KB (41 ms copies, between spells of compiling, and at exit), the second loads
  that size. 12 frames over 33 ms, 3 hitches in each fight, no slow library.
- It also ends about 600 MB written to disk every 5 s of play.

## Upstream ScarletNX's pipeline cache changes ported (2026-11-13, runtime of 10:1x, not installed, uncommitted)

Upstream (read at `a5c0354c`) found the same stall on 2026-10-20, two days after our base (`6f704735`), and later
added a check of the file and a retry. Ported into `pipeline.c`, which has no `src/common/gpu/vk` here:

- `pcache_check` (from `911fd5e9`): the cache file goes to the driver only when its header is Vulkan's and names
  this GPU and this driver build. Else the log says what it found and the cache starts empty.
- The quiet time is upstream's 20 s without a new pipeline (`PCACHE_QUIET_NS`), in place of the 5 s above. The size
  check stays, so an unchanged cache is never copied out.
- A write that fails is logged with its reason and tried again after the next quiet time or at exit (`f7649a16`).

Runs: a 10 byte cache file and the real one with another UUID (`badcache-short`, `badcache-uuid`, 25 s starts) each
log why, start empty, exit 0 and leave a good 248 MB file. Warm (`prtw`, 2 fights): no save while playing, one at
exit in the first (the cache grew 7 KB), 13 frames over 33 ms, 3 and 2 hitches, no slow library. Fresh cache
(`prtp`): written at the end of preparation and at each fight's exit, loaded by the next run, 12 and 13 frames
over 33 ms, 3 hitches each. The isolated copy's own build and cache are back.

## The three hitches left are the game reading text in menus (2026-11-13, runs `bench-tail1`, `bench-tail2`)

The profiler's spike report now also covers the time after a frame's last window (`SNX_NVN_SPIKE_MS`, with the
`profile` option; `BENCH_PROFILE=1 BENCH_SNX="SNX_NVN_SPIKE_MS=40;SNX_PROF_CALLERS=1" bench.py label=folder`), shows
24 places a thread and leaves out threads that only waited. Three profiled warm fights, exit 0, saves unchanged:

| Frame | After the last window | Draws | On screen | The one busy thread |
|---|---|---|---|---|
| 949 | 61-63 ms | 19 | a black screen between the main menu and the chapter map, before the loading spinner | sdk `__floatscan`, `strtod_l`, `strcmp`, `strcasecmp`, `strtok_r`, `stpncpy`, `strchrnul`, `__addtf3`, main `8c24ff8`, `9326340` |
| 1085-1104 | 50-51 ms | 45 | the loading spinner on black, two frames before the chapter map fades in | main `881bd3c`, `88183e4`, `8ccc728`, `8ccd05c`, `8cd7660`, `82ceca0` |
| 4623 | 61-76 ms | 113 | "The witch hunts are over" back to the verse | the same sdk text functions as 949, main `9326620`, `8c24ff8` |

- In each, one game thread runs game code for the whole time and every other thread waits: no runtime function, no
  driver, no lock. About half its samples fall in the code module outside any listed function start.
- 949 and 4623 are the game turning text into numbers and names (`strtok_r`, `strcasecmp`, `strtod` with its
  128-bit `__addtf3`): a table read as text at a menu or a restart. It is the game's own work on its own thread, and
  the console does the same there.
- None is in play: the contact sheet (`shots-20261113/fix-whole-route.png`, a picture every 100 frames) has the
  chapter map at 900, the box at 1100 and the white game-over card at 4600. A warm fight has no hitch of 50 ms
  while the player fights.
- Not done: who calls `strtod` (which table), and what the unlisted half of the samples is. Worth it only if a
  player reports the pause at a restart.
- Pictures of the very frames (2026-11-13 evening, runs `bench-demoB`..`D` with `RUN_SHOT_FROM`/`TO`, and `bench-demoA`
  with `SNX_NVN_SLOW_MS=1` for every frame's time): 949 is black before and after, 1090 is the spinner, 4624 is the still
  game-over card. The rows above are corrected from them: the contact sheet's every-100-frames pictures had misled.
