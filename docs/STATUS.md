# Bayonetta 3 PC port status

**2026-11-02, phase 6 (smooth 60 FPS and a friendly launcher).** Bayonetta 3 v1.2.0 runs on Windows through ScarletNX: the game's ARM64 code is recompiled to a native DLL, and an NVN-level renderer on Vulkan replaces the Maxwell path. Build a2 is installed in `output/Bayo3_120` (`3b78e390...`, 2026-11-03 at the user's request, rollback to c7 in `diagnostics/scene-hitch-20261103/preserved-installed-a2/`): c7 plus button prompts, shaders in order, the pipeline cache and pool imports off the frame, and the batched ASTC decoder (area-change hitch 264 to 101 ms in the replay), not yet played by the user. c7 was: f5 plus cutscenes at 60, GPU timing by pass naming each large job, a shader module dump, input recording and a test switch for software denormal flushing (`SNX_SOFT_FTZ=0`), with the module recompiled. Release 0.1.0 is c2. Release 0.1.1 (2026-11-03, published on GitHub) is c7 built from the committed source (`2c85ecac...`): it fixes the FPS drop when loading into or out of a verse. Release 0.1.2 (2026-11-04, packaged, not published yet) is a2 built from `c5774e0` (`ac31d727...`) with the launcher fix for issue 1: zip `7790fbfe...`, 82.7 MB, new-player test passed (Install in 214 s, the game started and exited 0), notes in `release/RELEASE-NOTES-0.1.2.md`. Its zip (`bfb72d76...`, 82.6 MB) passed the new-player test: Install from an extracted dump in 188 s, the game started and exited 0. On c1, the same without the timing, the user played chapter 12 at 2560x1440: **in-engine cutscenes 54.8 FPS (29.9 on f5), gameplay 54.3 FPS, combat 48.8 FPS, peak 2.9 GB of video memory for the game (about 7.4 GB before phase 6), no crash.** Open: 8-12 s at about 17 FPS after a map load. The c3 load test (2026-11-03, `diagnostics/passtime-20261102/README.md`) names it: a chain of three game compute shaders, 20-25 ms together, and one full-screen pass of about 11 ms even at 3 draws. Their size doesn't explain that cost, so our translation of them is the lead. Those shaders are captured (2026-11-03, `diagnostics/passtime-20261102/spv-c5/`, from a replay of the user's recorded session). Read against Witch Time's on 2026-11-03 (same game code, ours about twice the SPIR-V): every float op is wrapped in a software denormal flush, because the RTX 3080 offers no flush mode, and Witch Time doesn't do it. That alone looks too cheap for the cost; the other suspect is how we run branchy shaders (one block per loop pass, with a wait for lagging lanes at barriers). The user's A/B (2026-11-03) ruled the flush out (no change). The cause is memory: storage buffers a game compute job writes were bound to the pools, guest memory in system RAM, so every access crossed PCIe. Build c7 (2026-11-03, INSTALLED 15:35 at the user's request, rollback to c6 in `diagnostics/vram-cb-20261103/preserved-installed-c7/`, `diagnostics/vram-cb-20261103/README.md`) keeps them in video memory, landing in guest memory only when needed and merged so the game's own writes stay (Witch Time's design, credited). In a replay of the street it lifts the 1% low from about 30 to 46-48 FPS (median 23-24 to 17 ms), with no waits and no rendering faults seen; **the user's verse exit on c7 (2026-11-03): the drop is gone** (compute at most 0.6 ms a frame against up to 24 ms on c6, no sampled frame over 20 ms of GPU against 43, median 17 ms throughout; the user: "better better"). Left: the full-screen pass `1e842019` (about 7-9 ms, half its textures from the pools) and ASTC load hitches. Build c5 (2026-11-03, installed) also records a play session's input and replays it, so a load test recorded once can be rerun on every build: checked on the scripted fight route, which replays to the same place (`diagnostics/input-replay-20261103/README.md`). PlayStation and Xbox button prompts are built (2026-11-03, build p1, not installed, see "Button prompts" below). Goal: butter smooth at 60, judged by hitches and frame pacing, not by FPS above 60. This is an undergraduate's graduate project, started 2026-10-19.

Hardware: desktop Ryzen 7 5700X3D, RTX 3080, driver 621.01. Laptop i5-12450H, RTX 4050 6 GB. Numbers are from the desktop unless noted. Fuller notes: `RESUME-ON-DESKTOP.md` and the READMEs under `diagnostics/`.

## Release 0.1.0 (2026-11-02)

Published by the user on GitHub, repo public, tag `release`. The user judges the build playable from start to finish. Measured here so far: see "What runs".

`B3-Express-0.1.0-windows-x64.zip`, 82.6 MB, built by `diagnostics/release-0.1/make-release.py`, runtime c2. No game files or compiled game code. Guide: `docs/INSTALL.md`, in the zip as `INSTALL.txt`. Tested as a new player on this PC: unzipped to an empty folder, the package's own Python ran Install from an extracted dump in 202 s, the module came out the same as the installed one (`85df1689...`), and the game started and exited 0. Not tested on another PC, or with a dump from another tool.

## What builds

| step | result |
|---|---|
| runtime | c2, `Bayo3_120.exe` 54.4 MB (`5389fc20...`), built from `source/` with the bundled clang (`ninja_build.py scarletnx`) |
| game code | 4 modules, 107,997 functions, 7.35 M instructions, 83 blocks, 250 MB DLL, 1,125,557 entries. 179 s on 14 jobs, peak 3.5 GB of RAM. The same bytes on every build, for one CPU. Installed module `85df1689...`, compiled with the table call list |
| optimisation | O1. It gave +10% over O0, O2 nothing over O1 |
| shader preparation | 7,764 stages from the game, 5,315 unique, 171 s on 14 workers, cache 266 MB |
| launcher tests | logic, page build and real-window tests pass, `launcher/test_launcher.py`, `diagnostics/launcher-install-20261101/` |

## What runs

| scene | how it was seen | result |
|---|---|---|
| title, menus, chapter select | scripted, played | run |
| prologue | scripted, older renderer | 17,077 frames in 901 s, reached Chapter Select, exit 0 |
| street (`f1street`) | scripted | 2560x1440: 41-44 FPS native, 59.4 with FSR 1 from 1600x900 |
| scripted fight (`route-remnant9.txt`) | scripted, 12,000 frames | 59.1-59.7 FPS windows at 2560x1440, GPU 9.3-9.4 ms |
| Jeanne cutscene | scripted | 30 FPS, the game's rate |
| Remnant 10 | played on f5 | 51.0 FPS in combat, see below |
| Remnant 10, laptop | played on 5h2 | 54.0 FPS in combat, 0.43% of frames over 33.4 ms |
| Remnant 11 | played on phase 5 | froze in intense scenes then, not played since the freeze work |
| chapter 12 | played on f5 and c1 | whole chapter, 54-56 FPS in play, 48.8 at 1,300+ draws, no crash |
| chapters 10, 11, Records of Time | older tests | load and play, not measured on f5 |
| other chapters | | not run on the current build |

## Performance history

| date | work | result |
|---|---|---|
| 2026-10-19 | start | street about 17.6 FPS |
| 2026-10-29 | chunked submits | +12% |
| 2026-10-30 | NVN renderer, phases 1-4 | one frame replays correctly, then whole scenes |
| 2026-10-31 | video memory copies of the GPU-cached pools | street 40.1 to 58.5 FPS on O0, 43.7 to 59.8 on O1 |
| 2026-10-31 | FSR 1 | street 40 to 59.4 FPS, GPU 21.5 to 12.3 ms |
| 2026-11-01 | ASTC as BC3, sparse copies | fight peak 8,819 to 3,753 MiB, no FPS cost seen |
| 2026-11-01 | pipeline libraries, shader preparation | first fight on empty caches: waits 27 s to 0.51 s, worst frame about 4,400 to 412 ms |
| 2026-11-01 | launcher Install compiles the game code | extracted folder to Play ready in 196 s |

## Video memory (2026-11-01)

Each step is on by default and has its own off switch.

| run | scene | build | peak MiB | last FPS windows | GPU ms |
|---|---|---|---|---|---|
| m1b | fight | RGBA8, whole copies | 8,819 | 59.2, 59.5, 59.5 | 9.41 |
| m1a | fight | BC3 | 6,053 | 58.9, 59.5, 59.5 | 9.47 |
| m2a | fight | BC3, sparse | **3,753** | 58.7, 59.5, 59.5 | 9.41 |
| m2c | street | BC3, sparse | 3,607 | 44.1, 41.6, 41.7 | 12.93 |

`nvidia-smi` peaks include about 1.4 GB of desktop. Stale check: 9,364 blocks read back, 0 stale.

- **ASTC as BC3** (`SNX_VK_ASTC_BC=0` off): `astc.comp` decodes on the GPU, `bc3.comp` encodes each 4x4 block. Lossy, with a mean difference of about 1/255 in still frames.
- **Sparse copies** (`SNX_NVN_VRAM_SPARSE=0` off): each pool copy is a sparse buffer, bound per 64 KiB block when first copied.
- **Left:** 34% of combat reads still come from system memory, over 99% of them blocks the GPU writes. Video memory as the truth for those blocks is the riskiest change so far, parked. 64 MiB blocks were tried and rejected.

## Freezes (2026-11-01)

| fight | pipelines made | waits total | longest wait | worst gameplay frame | frames over 250 ms |
|---|---|---|---|---|---|
| first run before | 669-714 | 29.5-32.7 s | 2,288 ms | 4,430-5,110 ms | 20-25 |
| first run, libraries | 674 | 27.1 s | 2,241 ms | 4,330 ms | 23 |
| first run, libraries, background preparation | 682 | 24.1 s | 2,254 ms | 4,192 ms | 23 |
| **first run after the pass** | 717 | **0.51 s** | **42 ms** | **412 ms** | **6** |
| second run | 687 | 0.16 s | 44 ms | 260 ms | 2 |

- **Pipeline libraries** (`SNX_VK_GPL=0` off): four libraries per pipeline, links at most 0.2 ms.
- **Shader preparation** (`SNX_NVN_PREP=0` off): stages compile ahead when the game gives them, below normal priority, pausing in heavy frames.
- **Preparation pass:** the launcher's first Play runs it, about 3 minutes, and shows a percentage.
- **Tried:** moving a waiting draw to the front of the queue. Queues were short here, so the cost is the compile itself. It was never measured off.

## The user's play: Remnant 10 on f5 (2026-11-01)

2560x1440, `fsr=0`, 334 s, no crash. Against the matched run on phase5i, combat taken as windows of 900+ draws, 16 windows each.

| | phase5i | f5 |
|---|---|---|
| video memory | about 7.4 GB, another tool | **peak 2,491 MiB** |
| combat FPS | 49.8 | 51.0 |
| combat frames over 33.4 ms | 107, 1.11% | 84, 0.88% |
| mean draws | 1,237 | 1,268 |
| freezes in play over 1 s | 3 | 1 |
| longest freeze of 200-999 ms | 289 ms | 233 ms |

- **The 2.4 s freeze just after the fight is one pipeline, the same in both runs:** 54 draws, a 3-vertex draw waiting about 2.44 s. Every stage was prepared, so the draw most likely asks for a variant preparation doesn't predict. Which one isn't known yet.
- **Combat is CPU limited:** before the first window 5-8 ms, windows 9-12, after the last 3-4, about 19-21 ms a frame, with GPU time at 13-20 ms.
- That run didn't start from the launcher's Play. Preparation was quick only because NVIDIA's disk cache held the shaders from the isolated runs.

## Launcher (2026-11-01)

`Launch-Bayonetta3.cmd`. Pages: Play, Graphics, Controller, Install, Sessions.

- **Install takes an extracted dump**, a folder with `exefs` and `romfs` at 1.2.0. Players need no keys, by the user's decision. The version is checked before anything is copied, then the launcher compiles the game code on that PC.
- **Measured in the real window:** a wrong folder and a same-size wrong version refused, ready for Play in 196 s, the source folder left alone. Closing the window at 10% left no compiler running and the game not marked ready, and the next build finished.
- **First player issue, fixed 2026-11-04 (not released yet):** on a non-English Windows, Install did nothing, with no message and no log (0.1.0 and 0.1.1). The bundled Python 3.15 reads program output as UTF-8, and `tasklist` answers in the PC's code page (German "ausgeführt" has byte 0x81), so the "is the game running" check crashed before any status was set, and `pythonw` dropped the error. That check now reads bytes, `nvidia-smi` is read the same way, and any unexpected launcher error now shows a message box and goes to `logs/launcher-errors.log`. Reproduced with German `tasklist` output on the 0.1.1 launcher (empty status, error dropped), fixed launcher goes on to the version check; `launcher/test_launcher.py` covers it.
- **Hints:** code the recompiler missed runs in the interpreter and is listed in `.hints`. Every build now includes the 351 known addresses. **Tried, no gain:** alternating runs, 559 and 535 frames over 33.4 ms without them, 554 and 536 with them, so the installed module stays.

## Witch Time, for comparison

Witch Time and B2recomp are references, not rivals. Notes: `diagnostics/witch-time-study-20261101/README.md`.

Side-by-side of 2026-11-01: the user played Remnant 10 on both, on one evening, same PC. 2560x1440, 3D at scale 1.6, mailbox on both. Combat is windows of 900+ draws. Hand-played, so the two fights differ. Logs: `diagnostics/freezes-20261101/play-f5-20261101/`, `diagnostics/witch-time-study-20261101/play-remnant10-v045-20261101/`.

| | Witch Time v0.4.5 | this port, f5 |
|---|---|---|
| combat FPS | **59.2** | 51.0 |
| mean draws in combat | 1,208 | 1,268 |
| windows of combat frames below 58 FPS | 3 of 31 (lowest 50.6) | most |
| longest combat frame | **31 ms** | 233 ms |
| longest frame in play | 136 ms | 2,454 ms |
| pipelines made | 564 | 1,717 |
| shader preparation | 8,024 variants, 58 s | 5,315 stages, 171 s |
| frame path | render thread of its own, 11.5 ms a frame, 3.8 of it draw preparation | game and recording in one thread, about 19-21 ms |
| GPU | 37-45% and 135-140 W, as the user read them | 13-20 ms a frame by our log, not read by hand |
| video memory | about 5 GB in RTSS, an older reading, likely the whole card | 2.5 GB, the game alone, not the same counter |

- **The main difference is structure.** Replaying the frame costs about the same on both, 11.5 against 9-12 ms. Witch Time does it on its own thread, while ours does it in line with the game. That is an observation, not yet a measure of what a render thread gives here.
- **The GPU gap is unmeasured but looks large:** 40% of a 16.7 ms frame would be about 7 ms, against our 13-20 ms. Reads from system memory are the first suspect, and we have no same-tool reading of ours yet. A render thread alone won't reach 60 in the windows over 16.7 ms.
- **Stated plainly:** it's ahead on smoothness, and memory is where this port may lead, but that needs one counter on both.

**Superseded on GPU time:** the 7 ms estimate above was wrong, see the chapter 12 section.

## Chapter 12, whole chapter (2026-11-02)

The user played it on both, with `nvidia-smi` recording each. Hand-played, so not the same fights. Our figures leave out windows that cross into a cutscene. Notes and scripts: `diagnostics/chapter12-20261102/README.md`.

| | Witch Time v0.4.5 | this port, f5 |
|---|---|---|
| gameplay FPS | **59.1** | 55.6 |
| combat at 1,300+ draws | **58.9** | 48.8 |
| time under 50 FPS | **0.3%** | 20% |
| windows where 1% of frames passed 33.4 ms | **1 of 350** | 8 of 97 |
| longest frame in play | **132 ms** | 2,428 ms |
| pipeline waits in play | **1.1 s**, longest 435 ms | 5.6 s, longest 2,386 ms |
| loading screens held for pipelines | 115 s in all | none |
| GPU busy a frame at 1,300+ draws, one counter | **10.3 ms** | 16.4 ms |
| GPU busy a frame under 500 draws | 7.9 ms | 8.0 ms |
| game side at 1,300+ draws | 5.7 ms | 6.9 ms |
| peak video memory, `nvidia-smi` | 5,834 MiB | **4,881 MiB** |

- **Baseline for map loads, set by the user:** a seamless feel, not 60 FPS. Target, from Witch Time on the same loads: no 5 s window under about 45 FPS, none with p99 over 33.4 ms, longest frame about 126 ms.
- **Transitions:** Witch Time waits at loading screens until all queued shaders and pipelines are done, 115 s here. Ours does that work in play. This is likely most of why its transitions feel seamless.
- **Heavy drops at 380-700 draws are GPU time:** 16-25 ms, with the game waiting on the GPU. ASTC to BC3 on the GPU is a suspect, not proved.
- **Combat:** 20.4 ms in one thread, and GPU 15.9 ms, so a render thread alone leaves it on the edge of 60.
- **The game's own code isn't the gap.** `before the first window` includes the pacer's hold.
- **Video memory:** on one counter, about 2.9 GB against 3.9 GB, using one idle figure for both, so this port does lead here.

## Cutscenes at 60 (2026-11-02)

Built, tested on scripted scenes and played, **installed at the user's request with `cutscene_fps=60` as c1, now in c2**. Rollback to c1: `diagnostics/cutscene60-20261102/preserved-installed-c2/`, to f5: `preserved-installed-c1/`. Notes: `diagnostics/cutscene60-20261102/README.md`. The design is Witch Time's, written as our own code: four game functions replaced, a recompiler list so their calls go through the dispatch table.

| scene | at 30 | at 60 |
|---|---|---|
| title loop, in-engine | 39.96 s | 40.05 s at 52-59 FPS |
| movie | 79.66 s | 79.87 s |

The user played chapter 12 on c1: in-engine scenes 29.9 to 54.8 FPS, gameplay unchanged once the route is allowed for, no crash. Its lower pipeline waits come from caches filled by the run before. Rides aren't tested, and two scenes ran 2-3 s longer, unexplained. The user heard slight audio desync when a cutscene fell to 28-35 FPS. c2 pays a late scene off faster, by calculation from about 150 to about 50 ms behind at a steady 30 FPS, not yet heard. Skips work, by the user's play. **New first target: stretches of GPU 19-29 ms at 300-860 draws, 89% of c1's frames over 33.4 ms, on both builds. The user reports them on returning from the verse maps, 15-16 FPS for 5-7 s. ASTC causes the single spikes of up to 348 ms at those loads, not the 8-9 s at 16-18 FPS that follow, where the GPU reads 32 ms and fades over about a minute. Witch Time has the same heavy phase, 44.7-50 FPS for 45-90 s after each load, GPU 14-17 ms, with no holds at these loads, so the phase is the game's. The 8-12 s at about 17 FPS (GPU about 55 ms, 92-94% use) are ours alone. The long phase nearly matches: 41-44 against 45-46 FPS. Named by GPU timing, c2: in the worst seconds one game compute job (613xN) takes about 20 ms and one group of passes 13-16 ms, against 0 and 3 ms when settled. The game has dynamic resolution, but it stayed at the usual size, so it isn't the cause. Which shader, and why it costs this much here, isn't known yet. See `diagnostics/passtime-20261102/README.md`.**

## Button prompts (2026-11-03)

The game's prompts can show PlayStation or Xbox buttons instead of the Switch's, with no mod. Ported from Witch Time
(credited in `source/docs/CREDITS.md`): every prompt comes from one atlas texture (`core_sys_btn`, 256x512 ASTC). When
our texture cache first makes it (found by its hash), `source/src/nx/gpu/vk/prompts.c` paints the ten Switch cells over
it in guest memory: face buttons, L/R/ZL/ZR, Plus and Minus. It paints again when the choice changes, and the cache
reloads it like any texture the game wrote.

- Setting: `button_prompts` in `[Controllers]`: 0 Auto (the type of the pad used last, Switch with no pad), 1 Switch,
  2 PlayStation, 3 Xbox. It is also on the launcher's Controller page (not yet in the in-game overlay menu). Face buttons follow
  `gamepad_swap_ab`: by position by default (the console's A, on the right, shows Circle or Xbox B), by letter when swapped.
- Build p1 (`fd057876...`, `diagnostics/prompts-20261103/`), c7 plus prompts. Not installed.
- Tested in the isolated copy on the scripted route: PlayStation and Xbox sets seen on Chapter Select's footer and the
  combo list (L2 / LT in place of ZL), Auto with no pad keeps Switch. Painted cells decode right in our CPU ASTC decoder.
- Not tested: Auto with a real pad, changing the setting while the game runs, a keyboard set (not ported), the game's
  BC3 atlas variant (not ported, only logged as not found).

## Shaders in order (2026-11-03)

Witch Time's change, written here from its notes: a shader whose jumps all go forward runs its blocks once, in order,
instead of in the `switch (pc)` loop (`spirv_translate.c`, `SNX_SHADER_LINEAR=0` turns it off). On the game's 5,616
recorded shaders, those left in the loop: vertex 198 to 7, compute 62 to 15, fragment 2,818 to 1,297, all translating.
Build s2 (`ba207167...`, `diagnostics/shader-order-20261103/`), not installed. On the RTX 3080: no rendering faults
seen, steady p99 23-24 to 20-22 ms on the scripted route, but GPU time in the replayed session is the same within
noise. NVIDIA wasn't paying for the loop, so our combat GPU gap is elsewhere. Witch Time's gains were on AMD, not tested
here. New shader code misses the driver's pipeline cache once after an update.

## Scene-change hitch (2026-11-03)

The 200-300 ms frame at each area change was our GPU decoding 230-310 Mpx of first-time ASTC in one frame (~276 ms),
which the game's NVN driver waits for. The decoder now batches a texture's levels (one barrier before its encodes, one
before its copies) instead of draining the GPU three times a level: on real blocks 431 -> 174 ms for 609 Mpx, byte for
byte the same as before. In the replay: GPU 289 -> 148 ms and 263 -> 111 ms at the two area changes, their frames
264 -> 117 ms and 214 -> under 100 ms; batched across textures too (build a2, `3b78e390...`): GPU 121 and 95 ms,
the first frame 101 ms (a2 INSTALLED 2026-11-03 at the user's request, rollback to c7 in `diagnostics/scene-hitch-20261103/preserved-installed-a2/`). Also moved off the frame: the pipeline cache save (494 MB, every 5 s on the vsync
loop) and the host import of memory pools (up to 313 ms at boot). Build a1 (`7a8d599a...`), not installed;
`diagnostics/scene-hitch-20261103/README.md`.

## Object fade (2026-11-04)

The game fades objects with a 64 x 64 screen-door pattern (429 of 3,755 pixel programs). Build d1 (`953aef25...`, INSTALLED
2026-11-04 at the user's request, rollback to a2 in `diagnostics/dither-20261104/preserved-installed-d1/`, uncommitted) adds `dither=0..3` (launcher Graphics "Object fade"): the game's, off, even (the pattern at
the image's own pixels) and transparent (blended by the alpha it compared). All patched programs pass spirv-val; six
fight runs at 2560x1440 ran clean with no frame time change, and transparent removes the checkerboard on a real fade. A/B on the fight route (2026-11-04, two runs a mode): slow frames 517/502 game, 505/492 off, 511/496
transparent of 5999, so no cost. Transparent is the default since then, at the user's choice (`dither=3`).
Details: `diagnostics/dither-20261104/README.md`.

## Game and source identity

- Title ID `01004A4010FEA000`, display version 1.2.0. Main build ID `B47CC79B88B7091EA514B2B725C50AE01C6F00DF`, main SHA-256 `499983f6...`.
- Four NSO modules: rtld, main, subsdk0, sdk. All 12 decompressed segments match their embedded SHA-256.
- ScarletNX source: commit `5c97949bf6cb69738c7d6ac110f07533376a3bd9`, ABI stamp 8.
- Release model: a launcher, the runtime and local compile tools. No game files, saves or compiled game code ship with it.

## History before phase 5 (2026-10-19 to 2026-10-28)

From the old root README, whose full text is `diagnostics/old-docs/README-root-to-20261028.md`. Laptop, RTX 4050, old renderer. Single runs in this period differ by up to 10%, so small gains here aren't evidence.

| date | what happened |
|---|---|
| 10-19 | First -O0 module: 107,992 functions, 7.35 M instructions, 150 s. Starts, then a black scene with prompts |
| 10-19 | Scene rendering fixed: a shader predicate bug (`O_SHFL` in `spirv_translate.c`) and dropped attachment-less storage passes (`render.c`). 4,040+ skipped draws to 0. Shader corpus 61 of 61 |
| 10-22 | Citron compared: all 86 HID layout constants match. Scripted input reaches the game's SDK |
| 10-23 | An interpreted main module reproduced the same scene, so the recompiled code was unlikely to be at fault |
| 10-24 | Title fixed: a cold start reported a stored user, so the game replaced the title with Chapter Select. Menus and new game then work |
| 10-25 | Prologue reached Chapter Select, 17,077 frames in 901 s. One stall after Skip, seen once |
| 10-26 | 2D-array compute surface stores and storage bindings on mip chains fixed. 211 skipped dispatches to 0 |
| 10-27 | Masked shader-result merge installed, 303,936 correctness cases. Chapter 10 and 11 saves load. Method barrier removal tried, slower, rejected |
| 10-27 | New Game with a save opens a warning with No selected. Not a bug. Prologue path confirmed by scripted runs |
| 10-28 | Landing by target removed whole-queue buffer waits (36,794 calls, 46.7 s of a 360 s run). Not installed then |
| 10-28 | The game wasn't GPU or CPU bound: it recorded GPU work and blocked until the queue drained. This led to the NVN renderer of 10-29 |

Open from that period, unseen since but not proven gone: intermittent native access violations at startup, the stall after Skip. Audio device commands 1 and 7 and account command 160 remain undeclared. The chapter select hand was missing then, and hasn't been checked on the new renderer.

## Left

- Button prompts: the user's check with a real controller on Auto, and a change while playing (p1, not installed).

In order of work:

1. Name the compute shader (613xN) and the pass group behind the drops after loads, and what they read. A texture pack made at install would fix load spikes only.
1a. Hold loading screens until queued shader preparation and pipelines are done.
2. Log real frame percentiles, ASTC per window, and the key of any wait over 100 ms.
3. A/B of ASTC as BC3 where textures stream, in chapter 12.
4. A measure of whether a render thread pays.
5. GPU time in combat, 16.4 against 10.3 ms. Reads from system memory are the first lead, which brings back VRAM step 3.
6. A packaged `.exe` for the launcher. The zip release is done.
7. Texture hitches, 100-270 ms at loads and camera cuts.
8. Cutscenes at 60: built, needs the user's play and a launcher setting.
9. FSR 2/3, DLSS and HDR only if time allows.

Also open: merging upstream, now 1,872 commits ahead of our base, with NVN as its only GPU path and a hook system (`docs/HOOKS.md` there). Hooks match exported symbols, and the game's main exports 25, none of the cutscene functions. Tested unchanged on the scripted fight: 25.2 FPS against our 58.5, about 4.9 against 2.3 GB of video memory, no crash, `diagnostics/upstream-20261102/README.md`. One scene, not studied further. Decision: no renderer merge now. The project has a git repo since 2026-11-02, by allowlist in `.gitignore`: no game files, keys, saves, logs or captures. ScarletNX has agreed to the project on that condition. The launcher doesn't install `control.nacp`. A future ABI change shows ready and then stops at Play.

## Not tested

- BC3 up close on faces and metal, and movies.
- FSR 1 on Thule, Gomorrah, the kraken and full-screen movies.
- `mute_unfocused` and the cutscene audio fix, by ear.
- f5 on the laptop, or any AMD GPU. A CPU of 12 threads for the compile.
- Other chapters on f5. Tessellation and geometry stages and compute aren't prepared.
- A dump extracted by another tool.
