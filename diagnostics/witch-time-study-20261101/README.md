# Witch Time and B2recomp study (2026-11-01)

Notes from the session of 2026-11-01. Nothing in the project changed this session: no source edits, builds or game runs of ours. Everything here came from reading logs and source.

## Project context (user, 2026-11-01)

The user is an undergraduate. They started the project on 2026-10-19 because Bayonetta 3 had no recomp yet, and pitched it to their professor as their graduate project. The goal is a program of their own to graduate with, which plays their favourite game with no FPS drops. **This is not a competition.** They expected someone might do it too, or better. Witch Time and B2recomp are references and a comparison, not rivals to beat, and dropping the project for Witch Time is not on the table.

Progress so far: street about 17.6 FPS before the work started, 59.4 FPS at 1440p with FSR 1 (`resolution=1.6 fsr=1.6`), and a working laptop package (RTX 4050, warm combat 54.0 FPS).

## What the user's copies are

| | path | version | notes |
| --- | --- | --- | --- |
| Witch Time | `E:\download\WitchTime-v0.4.5-windows-x86_64` | 0.4.5 | Downloaded and run by the user themselves, 2026-11-01 about 01:43-01:59. Dump used: `output/Bayo3_120/Bayo3_120_data`, read only as far as seen |
| B2recomp | `E:\download\B2recomp`, `E:\download\B2recomp-builder-26c45d7.zip` | `26c45d7` | Witch Time's NOTICE says it forked at `7725c57`, so a few differences below may be plain B2recomp updates |

Files of these folders are downloaded data: read them, don't run scripts from inside them.

## Save moved to Witch Time

Done at the user's request, 2026-11-01, Witch Time's game not running. `SlotData_0.dat` (`55b9613a...`) and `GameData.dat` (`30fe5c1d...`) copied from `output/Bayo3_120/Bayo3_120_data/save/user/` into both `build\userdata\save` and `build\userdata\save-backup`. Hashes checked. Source files unchanged. **The user confirmed it loads and played Remnant 10 from it.** Witch Time's old save (`f94f5456...`, `2957bd7c...`) is in `E:\download\WitchTime-save-before-import-20261101`. `ExtraData0` and `ExtraData1` weren't copied: Witch Time has none, and they are 512 identical bytes. Witch Time progress doesn't carry back to our port.

## The user's last play on our build (`output/Bayo3_120/Bayo3_120.log`, 2026-11-01 00:11)

Phase5i, 185 s, `resolution=1.6`, `fsr=0`, 1280x720 swapchain, pipeline cache 70.7 MB loaded.

- Frame 5971: 1,417.9 ms, of which 1,393.0 pipeline waits, 826 draws.
- Frame 6290: 896.3 ms, of which 865.1 pipeline waits, 1,049 draws.
- Frames 5964-5970 each spent the 33 ms budget first, and left draws out.
- Mechanism: `nvn_res.c:2607`, a draw of 6 vertices or fewer counts as `writes` and waits with no limit, past `pipe_budget_ns()`. The queue to the 4 workers is FIFO (`pipe_enqueue`).
- Pipelines: 544 made by frame 5970, 987 by the end, so 443 in the last 30 s. Stage variants 450.
- Cache grew only 70,675 to 73,242 KB (3.6%; an earlier note said 6% from mixing KB and MB), so most of the 987 were probably driver cache hits.
- Other slow frames: 210, 190, 164 ms with almost no time in windows, not pipelines. Not explained.
- Not known: whether the 1.4 s is one slow compile or a queue backed up behind it. **Needs per-pipeline timing.**

Consequence: recording pipelines and building them at start mostly repeats what the driver cache already holds. It would help after a driver update, not for pipelines never seen. The plan's first item should change.

## Witch Time's run on the user's PC

Log `build/run/witch-time-game.log`, `build/witch-time-game_stalls.txt`. RTX 3080, 1280x720 borderless, `cutscene_fps=60`. Setup: compile 263.8 s, `game.dll` 148.8 MB, 55,103 functions lifted, `game.dll` built on the player's PC.

- 1,826 battle frames, 34 over 20 ms.
- Holds of the render thread at a draw: 0. Draws left out: 0.
- 16,050 libraries compiled by workers, 651 s of worker time. Cache 383.5 MB. 7 containers, 8,024 variations prepared.
- Pipelines linked at a draw from ready libraries: 92, 1.2 ms in all, longest 0.03 ms.
- 2,434 programs handed to workers when set up, before their draws. `nvnProgramSetShaders` held its caller 398 times, 47 ms in all.
- Windows of 300 frames: up to 456 draws a frame. Not the same load as our 1,000+ combat.
- Still stalled: 263-761 ms in the stall file, one frame of 7,617.89 ms in frames 1800-2100. Not fight pipeline waits by their counters, cause not read.

That first run had no matched comparison. See the Remnant 10 run next.

## Witch Time, Remnant 10 by hand (2026-11-01, 02:10-02:20)

The user's own save, about 33,900 frames, borderless 2560x1440, `resolution_scale=1.6` on the game's 1600x900 (the same 2560x1440 as our `resolution=1.6` without FSR), `vsync=clock`, `cutscene_fps=60`, warm cache of 392,749 KB.

Combat, frames about 5100-13800, 960-1,750 draws a frame: almost every 300-frame window is 60.0 FPS, median 16.6-16.8 ms, p99 18-20 ms.

| worst windows | draws | FPS | median ms | p99 ms | max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 6300 | 1,748 | 54.8 | 17.99 | 22.49 | 52.55 |
| 10200 | 1,438 | 57.7 | 17.17 | 20.88 | 22.04 |
| 6000 | 1,359 | 58.1 | 16.79 | 20.72 | 87.00 |
| 8400 | 1,186 | 59.0 | 16.68 | 18.22 | 113.88 |

- Render thread batch 10-14.5 ms a frame in combat, of which 5-8 ms is its wait for the GPU. Timeline at 12000: frame complete on the GPU 9.53 ms after the batch starts.
- Holds at a draw, whole run: 17, 428 ms in all, longest 85.5 ms. Stage libraries 7,654 cache hits, 8 compiled, 274 ms. Links 857, 13 ms. Draws left out: 0.
- Stall file: 7 stalls of 267-518 ms this run, frames 3013, 4584, 21220, 26092, 30961, 32217, 32405. None between frames 4584 and 21220, which covers the combat.
- Frame of 2,052 ms in window 21000: `pcmenu: opened` and `closed` log right before it, so probably the user's settings menu. Not proven by timestamps.
- Device memory 2,898 MB. Not comparable with our nvidia-smi peaks.

Ours, same PC, closest log (`output/Bayo3_120/Bayo3_120.log`, 00:11, `resolution=1.6`, no FSR, 720p window, 3 minutes): the log doesn't name the chapter, so it may not be Remnant 10.

| frames | draws a frame | GPU ms | frames over 33.4 ms of 600 |
| --- | ---: | ---: | ---: |
| 5400-6000 | 566 | 13.37 | 11 |
| 6000-6600 | 901 | 17.65 | 18 |
| 6600-7200 | 1,066 | 13.43 | 11 |

Plus the 1,418 and 897 ms freezes. Witch Time is clearly smoother on this PC at the same render size. The new finding is GPU time: ours 13-18 ms at about 1,000 draws, theirs finishes within about 10 ms at 1,100-1,200 draws. We may also be spending GPU time they don't. Cause not known, and ours has 285,523 barrier commands in 7,513 lists worth checking first. A matched run needs the same scene on ours, at 2560x1440.

## Matched run on ours (2026-11-01, 02:40-02:44)

The user's play test after the Witch Time run, reported as the matched test, on phase5i in `output/`: `resolution=1.6`, `fsr=0`, swapchain 2560x1440 from 10 s, mailbox, cache 73,242 KB warm, 264 s, clean exit, no crash lines. The log doesn't name the chapter. The INI was written at 02:39, which fits a launcher save just before play, but the log can't show how the game was started. Save changed by the user's play: `3927a739...`. Cache grew to 75,595 KB, 3.2%, with 1,863 pipelines made.

Combat, frames 1800-9600, computed from log timestamps: 48.4 FPS, 100 of 7,800 frames over 33.4 ms (1.3%).

| frames ending | draws | FPS | GPU ms | before | windows | after | held | over 33.4 ms of 600 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 3600 | 1,055 | 57.0 | 12.73 | 7.5 | 7.4 | 2.6 | 3.6 | 2 |
| 4200 | 1,516 | 46.9 | 16.67 | 6.3 | 11.0 | 4.0 | 0.0 | 11 |
| 5400 | 1,449 | 46.5 | 16.56 | 6.6 | 10.7 | 4.2 | 0.2 | 14 |
| 7200 | 1,549 | 43.9 | 17.99 | 6.8 | 11.8 | 4.1 | 0.0 | 3 |
| 8400 | 1,463 | 42.4 | 18.96 | 7.8 | 11.4 | 4.5 | 0.0 | 8 |
| 10800 | 922 | 60.0 | 9.81 | 9.3 | 5.8 | 1.6 | 7.0 | 0 |

- Reaches 60 up to about 1,000 draws. Witch Time held 60 at 1,200-1,450 and 54.8 at 1,748.
- At 1,500 draws the frame path is about 21-23 ms and the GPU 17-19 ms, the game's GPU wait only 1.5-2.2 ms. **So not only a GPU problem: recording, at 10-12 ms, is the largest single part.** Both have to fall below 16.7 ms.
- Freezes: 6 frames over 250 ms. 2,469 ms (f11403, 54 draws), 1,051 (f6475, 1,269), 1,045 (f5712, 1,453), 289 (f1832, with 273.7 Mpx ASTC), 282 (f12288, not pipelines), 240 (f2187). Logged pipeline waits 6.6 s. Left out: 592 draws over 41 frames.
- Barriers: 613,761 in 11,477 lists, 53 a list.
- Order of work now: freezes, then recording cost, then GPU time. Witch Time's render thread takes 10-14.5 ms including 5-8 ms of waiting, at the same load. Whether it runs apart from the game's threads, and ours doesn't, needs reading.

## Witch Time against B2recomp `26c45d7`

Source compared without `build/`, dump files or toolchains. 104 shared files: 48 identical, 56 changed.

**Correction:** pipeline libraries, preparation (`job_prep`), `nvn_vk_gpl` and shader workers are in B2recomp already, with the same keyword counts. Witch Time inherited them, only adding its extra pipeline types. Credit B2recomp and ssbu-recomp too.

Inherited nearly unchanged: lifter design, NVN on Vulkan design, pipeline preparation, HDR, the core of display pacing.

| Witch Time's change | what | main files |
| --- | --- | --- |
| Boot B3 | SDK 13.3.2 imports, layout read from the ELF, more AdvSIMD forms, startup | `b3_imports.c`, `sdklift*`, `recomp.py`, `recomp_eh.py`, `rt_ext.h` |
| Game data | `.pkz` through the game's own Kraken decoder | `pkz_dump.c`, `scripts/pkz.py` |
| 60 FPS cutscenes | `cutscene_fps`, host versions of game functions | `game_shim_eh.c` (+335), `gfx_options.c` |
| Renderer | tessellation, storage images, depth bounds, patch size, passes with no targets | `nvn_vk_draw.c` (+699), `nvn_vk_m4.c`, `nvn_gl.c` |
| Shaders | subgroup operations, global atomics, more, some following suyu | `mx_translate.c` (+316) |
| Textures | block width padding, HDR ASTC, invalid blocks transparent black | `tex_decode.c` (+221), `tests/bc3_encode_test.c` |
| Movies | B3's player, FFmpeg on Linux | `nnmovie_mf.c` (+280) |
| Tools | new launcher and `wtbuild`, replacing `launcher.c` and the Python and llvm-mingw downloads | `launcher/src`, `launcher/ui`, `tools/wtbuild` |
| In-game settings | row in the game's Options screen | `pcmenu.c`, `nvn_vk_pcmenu.c`, `options_row.c` |
| Linux, packaging | platform layer, `game.dll` split, rename | `runtime/posix`, `runtime/split`, `wt_main.c` |

Changes were sampled through the `[witchtime]` markers, not every line read.

## Our project against Witch Time

We use B2recomp as a design reference. Witch Time is built from its code. They share ideas, not an engine.

| | ours | Witch Time |
| --- | --- | --- |
| Base | ScarletNX, a general Switch recompiler | B2recomp, from ssbu-recomp |
| Game code | ARM64 to LLVM to a DLL, per CPU modules | ARM64 to C, clang, on the player's PC |
| Switch OS | kernel, IPC and services emulated | SDK functions replaced with PC versions |
| NVN | game's NVN driver code still runs, 188 functions routed, command lists replayed on Vulkan | own NVN implementation on Vulkan or OpenGL |
| Older renderer | Maxwell renderer as fallback | none like it |
| Shaders | Maxwell to SPIR-V directly | Maxwell to GLSL, then glslang |
| Pipelines | full pipelines at first need, driver cache | stage libraries prepared ahead, linked at the draw |
| Cutscenes | 30 | 60 |
| Image | scaling, FSR 1 | resolution scale, HDR, no FSR |
| Video memory | our copies, street 40 to 59 | not compared |
| Setup | launcher unseen by the user, private, no repo | public, launcher, in-game settings |

Our CPU cost above 1,200 draws may come from keeping the driver layer. That is inference, not measured.

## How the translation to C works

`lifter/recomp.py`, about 1,300 lines of Python, from ssbu-recomp.

1. Reads the game's ELF and its layout.
2. Finds functions: every `.eh_frame` FDE start, then callees.
3. Each instruction becomes C. Each function is `void f_<addr>(Ctx *c)`. Registers are in `Ctx` (`c->x[n]`, `c->v[n]`).
4. Runtime jumps: `call_indirect`, a `switch` of address to function. Jump tables use a `switch` (`L_dispatch`).
5. Game memory is at its real address, `0x7100000000`, so game pointers are host pointers.
6. clang at `-O2` for the game code, with lower settings retried on a file's time limit, down to `-O0`. Flags include `-fno-strict-aliasing`, `-ffp-contract=off`, `-mcx16`.
7. `test_b3.py` compares added instruction forms in Unicorn and in C: 200 words, 4,000 cases, 0 failures on their developer's dump.

Output is machine translation, not readable game logic. Assets stay in the dump. Graphics, audio, input, saves and files are hand-written runtime. It isn't published, as it's made from each player's dump.

**Does C make optimizing easier?** For speed, barely. For people, yes.

- Registers in `Ctx` and pointers from integers stop the compiler assuming anything, so it reloads after each store.
- `regcache.py` moves registers into locals: 6.4-6.7 against 6.6-6.7 ms a frame, DLL +13%, lift 4x slower. Off by default.
- Ours: O1 over O0 about +10%, O2 nothing over O1. clang is LLVM, so both routes use the same optimizer family.
- C's real gains: a simple lifter, ordinary debugging, and game functions replaced by hand-written ones (`HOST_OVERRIDES`).

Finding for the thesis: the language the game is translated into matters far less than what surrounds the game code. Our big wins were runtime work: video memory copies, street 40 to 59 FPS.

## Agreed direction: order of work

1. **Freezes.** Per-pipeline timing (compile time, queue time, hit or miss, queue depth) and a waiting draw's pipeline moved to the front of the queue. Then stage libraries (`VK_EXT_graphics_pipeline_library`). Then compile when the game sets up a program: we already route `nvnProgramSetShaders`. Then texture hitches (100-270 ms), starting from Witch Time's ASTC work.
2. **A program that can be demonstrated.** Launcher Install page tested with a real dump, launch from `output/`, packaged `.exe`, and building the game module on the player's PC, as a compiled game DLL can never be shipped.
3. **Cutscenes at 60.** Read `game_shim_eh.c`. Needs a way to replace game functions in our recompiler, and an audio sync check.
4. **Optional if time allows:** FSR 2/3, DLSS, HDR.

Nothing of this has started. The first step is small and stays in the isolated copy.

## Graduate project points

- Ask the professor how much outside code is acceptable and how to declare it. GPL allowing a port doesn't settle what the university accepts. Using a design as a reference and writing our own keeps more of the work the user's.
- Declare sources: ScarletNX as the base, B2recomp and the Bloodborne port as design references, Witch Time as comparison, AI assistance as the university's rules require.
- The user must be able to explain the mechanisms: the GPU limit, the video memory copies, pipeline stalls.
- Keep the logs and before-and-after results in `diagnostics/`: they are the thesis data. Don't remove them in a debloat.
- Thesis angle: two ways to run the same game, and what each costs.
- `source/` has no git history and this PC seems to hold the only full copy. Keep a copy on another drive.

## VRAM: top of the list (user, 2026-11-01)

The user asked to put VRAM first: RTSS showed about 5 GB on Witch Time in Remnant 10 at 2560x1440. That is probably the whole card, desktop included, which fits the log: 2,898 MB of device memory plus a desktop of about 2 GB, so the game itself would be near 3 GB. Which RTSS counter it was isn't confirmed.

Ours at the same size: peak 9,499-9,682 MiB by nvidia-smi with 2,163-2,294 before launch, so the game is about 7.3-7.5 GB. Compare by the same tool: RTSS on ours, same scene.

Leads, in order:
1. Pool copies: 1,348 + 833 + 41 + 111 = 2,333 MiB allocated at start, 2,172 of 37,336 blocks in them at the end of the matched run. Allocate blocks instead of whole pools, offsets remapped. This is the mirror shrink held since phase 5.
2. ASTC on GPUs without it. Witch Time stores BC3 (`docs/STATUS.md:424`, `tests/bc3_encode_test.c`). A grep of our `astc.c` and `texture.c` finds no BC3 or BC7, so ours probably stores uncompressed RGBA: about 4x per ASTC texture. Not yet read. Upstream's ASTC to BC7 is the other route.
3. Measure what's left: render targets, staging, textures that stay live.
### VRAM plan (2026-11-01, code read, nothing started)

Confirmed in code: `gpu/vk/astc.c` writes ASTC as RGBA8 (4 bytes a pixel), BC3 or BC7 would be 1. Pool copies are one full-size buffer per pool with blocks of 64 KiB (`MIR_SHIFT 16`), read at the pool's own offsets. `device.c` uses no sparse binding.

0. Measure: a log line by category (targets, textures by format, copies, staging) and a process budget line from `VK_EXT_memory_budget`.
1. Copies: try a sparse buffer, keeping the offsets and binding memory only to blocks in use, so ranges across blocks stay contiguous. Fall back to remapped offsets if binding costs too much. Check `SNX_NVN_VRAM_CHECK=1`, FPS and GPU ms against phase5i.
2. ASTC: compress after decoding on the GPU, BC7 preferred if it can keep up, BC3 if not. Check sRGB views, alpha, upload time and picture against RGBA8.
3. Whatever step 0 shows is left.

### How Witch Time keeps VRAM low (2026-11-01, code and Remnant 10 log read)

This replaces the order in the VRAM plan above: ASTC first, then copies.

| | Witch Time | ours |
| --- | --- | --- |
| ASTC on a GPU without it | CPU threads decode ahead of the first draw (`nvn_front_tex_prep`, 3 threads), then `tex_encode_bc3`, 1 byte a pixel (`nvn_vk_draw.c:644`) | GPU compute to RGBA8, 4 bytes a pixel (`astc.c`), at the use |
| Static vertex data | Own buffer per stream of 4 KiB or more, once unwritten for two frames. Cap 512 MB, freed after 600 unused frames. Peak this run 48.8 MB, 257 buffers | Full copy of each pool, 2,333 MiB, 64 KiB blocks. 2,473 blocks in use in combat, about 155 MiB |
| Dynamic data | 256 MB stream ring in device-local host-visible memory (type 5 on the 3080, heap 10,051 MB). Their note: GPU 12.5 to 9.7 ms | Hot blocks stay in system memory: **32-33% of combat reads cross PCIe**. Copies churn 4.4-5.6 GiB per 600 frames, 7,000-9,000 blocks taken back |
| Device memory | 1,426-2,898 MB, in 64 MB blocks, system memory fallback when full | not logged; about 7.4 GB by nvidia-smi |

Rough split, an estimate only, treating the game's textures as if all ASTC: copies 2.2 GB less, and if T is BC3 textures and R the rest, T + R = 2.6 and 4T + R = 5.1, so T is about 0.8 GB and ASTC costs about 2.5 GB more. Together that explains about 7.4 to 2.9 GB. Step 0 has to confirm it. Their comment's "6.7 GB in a fight" for RGBA8 is a figure of theirs, not this run.

Costs of their route: BC3 is lossy on top of ASTC (`tests/bc3_encode_test.c` only asserts smooth images stay close), CPU threads run the decodes, and write watch has to run per stream.

Order now:
0. Log memory by category.
1. ASTC to BC3 ahead of the draw on worker threads. Should also lower texture hitches: their note says Shibuya frames over 34 ms 11 to 0, worst 59 to 22 ms. Ask the professor before porting `tex_encode_bc3`, or write our own.
2. Sparse copies, about 2.2 GB.
3. Hot data through a ring in device-local host-visible memory, for the 32% of reads over PCIe. This is the first lead on the GPU time gap too. Needs a fallback where resizable BAR is missing: the laptop isn't checked.

## How Witch Time is built, by problem (2026-11-01, source read, nothing run from it)

Files: `runtime/nvn_gl.c` (NVN front end, render thread, texture decode workers), `nvn_vk_draw.c` (Vulkan replay, memory, stream ring, textures), `nvn_vk_m4.c` (included by it: stages, pipelines, workers, preparation), `nvn_vk_store.c`, `tex_decode.c`, `lifter/recomp_eh.py` with `game_shim_eh.c` (game functions replaced by host ones). `docs/STATUS.md` holds their measurements. The numbers in this section are theirs, except where it says ours.

**Architecture.** Its own NVN on Vulkan, with one render thread. Guest threads post jobs (`gl_post`), and `nvnQueueSubmitCommands`, fence sync and flush don't wait for it. Ours records under `vk_lock` on the game's thread, so recording cost is game-thread cost. This is probably why recording matters more for us. Not measured.

**Freezes: four layers.**
1. Few keys. With extended dynamic state 3 and dynamic vertex input, blend, write mask, polygon mode and vertex layout leave the pipeline key (`nvn_vk_m4.c:12`), leaving stages, topology class, pass formats, samples, logic op and sample shading. Ours sets neither extension (`pipeline.c:604`), so those stay in the key.
2. Libraries. With `VK_EXT_graphics_pipeline_library` the stages are compiled without pass formats or topology. A draw links three libraries in microseconds (`nvn_vk_m4.c:488`).
3. Compile before the draw. Every shader in the game, about 8,000 stages from `shader.dat` and the effect shaders (`dat_walk`, `prep_main`), is compiled once per GPU and driver: "PREPARING SHADERS", about 20 s on 16 threads. At `nvnProgramSetShaders` a copy of the code goes to the workers (`nvn_vk_program_set`). The stored `pipes.bin` keys are built at start on CPUs - 2 threads, at most 16. An unrecorded pair gets predicted keys.
4. Loading screens as cover. `m4_barrier` doesn't present a frame of 60 draws or fewer while workers have queued jobs, up to 20 s, with a progress bar after 150 ms. The game leaves loading only when everything handed over is ready.
In a fight workers run below normal priority, and archive preparation stops (`g_prep_fight_cap = 0`). Four workers doing it in a fight gave 35 FPS. `shader_wait` defaults to hold, so what is left still stalls and is counted: `nvn_vk_hold:` lines, and creation feedback gives cache hit or compile for each one. Known gaps: tessellation pipelines made at their first draw (29 pairs), and an unannounced pair compiles at the draw.
Ours: `nvn_res.c:2607`, 4 workers, nothing prepared, draws of 6 vertices or fewer wait with no limit.

**VRAM.**
- ASTC: `tex_vkfmt` picks BC3, about a quarter of RGBA8, set by `SSBU_VK_ASTC_BC` (`nvn_vk_draw.c:644`). `tex_encode_bc3` is a fast min/max box encoder: bounding box inset, sign from covariance, no search. Hence "BC3 stays lossy".
- `tex_ahead`: a texture seen for 2 frames is decoded on 3 threads (`nvn_front_tex_prep`), and the image is made once that is done. The job keeps a copy of the bytes and its result counts only if memory still equals them. In a fight only at most 512 KB, or finished decodes, and no ring wrap, as a wrap waits for the GPU.
- Static streams of 4 KiB or more get their own buffer after two unwritten frames (`vrc_*`). Everything else goes through the ring.
- Own allocator: 64 MB blocks, per type and class, first fit, with a fallback to system memory when video memory is full (`vmem_alloc`).

**GPU time.** Stream ring in device-local host-visible memory when the heap is 2 GB or more (`vbuf_new`, `SSBU_VK_BUFMEM`): their GPU 12.5 to 9.7 ms. No hot data over PCIe by design.

**CPU, all found by a sampler, `SSBU_LPROF`, not by guessing.** Texture span index for flushes (83-96% of MainDraw and Flush threads), page filter in `buf_ref` (replay 30.5 to 16.3 ms on the ride), compute copies only when overlapping (render thread 10.4 to 3.9 ms), lock-free object lookups, and game functions replaced by host ones: `h_morton_split32` 171.6 to 1.7 ns, `h_quad_walk` 47 to about 0.01 ms.

**Method worth copying.** Each change has an off switch and a `_CHECK=1` mode comparing it with the slow way, for example 3,110 textures, none differ. Interleaved A/B only. Everything left over is counted.

**Their open problems.** About 0.6 s frame at the ride's start from 74 textures first seen in the frame that draws them, `f_71007e1fe4` trains (later fixed by `h_quad_walk`), an 80-86 ms frame at retry reload.

**What this changes for us (recommendation, not agreed).** Freezes are a design difference: our 6-vertex wait is the symptom, and missing dynamic state, libraries and preparation are the cause. Dynamic state comes first because it makes everything else cheaper. VRAM needs only BC3, smaller blocks and fewer pool copies. Whether a render thread is worth doing needs a measure first: game thread time waiting on recording.
