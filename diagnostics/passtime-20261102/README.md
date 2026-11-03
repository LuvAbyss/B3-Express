# GPU time by pass (2026-11-02)

Build c2 (`Bayo3_120-c2.exe`, `5389fc20...`) adds `SNX_NVN_PASSTIME=n`: every n frames one `nvn-pass:` line with GPU time by pass and compute dispatch. `nvn-live: frame times` gives a 1% low and ASTC for each report. Start it with `Launch-Bayonetta3-PassTime.cmd`. Originals: `pre-edit/`, edits: `edit-1.py`.

## The game has dynamic resolution

Scripted fight, isolated, `resolution=1.6`, 396 timed frames, `resolution.py`.

| largest pass | at the game's scale | frames | GPU a frame |
|---|---|---|---|
| 2560x1434 and 1440 | 1600x896 | 70 | 11.4 ms, menus and light scenes included |
| 2304x1306 | 1440x816 | 265 | 10.2 ms |
| 2176x1229 | 1360x768 | 15 | 13.3 ms |
| 2048x1152 | 1280x720 | 6 | 8.9 ms |
| 1638x922 | 1024x576 | 40 | 10.1 ms |

In heavy combat the main pass moves between about 1638x922 and 2304x1306 about every 2 s (t124-150), GPU about 10 against 16 ms at similar draws. That looks like a controller hunting. Seen in a 1-in-30 sample of one run, so its period is rough, and a sample's largest pass isn't always the scene's.

**Hypothesis, not proven:** after a load the game may start at its top size and step down, which would give a GPU peak that fades by itself. Pixels alone would be about 2.4x between the two ends. The c2 load test shows the size in every `nvn-pass:` line, so it settles this with no new build.

## The c2 load test (`play-c2/`, `drop.py`)

125 s, one load in and one exit, by the user, no GPU record. **The dynamic resolution idea is ruled out for this drop:** the main pass stayed at the usual 2304x1306. What changes in the drop:

| stretch | GPU a frame | what's in it |
|---|---|---|
| settled, before | 5.6-6.5 ms | passes 4.5 ms, compute 0 |
| worst, t71-75 and t84-88 | **34-39 ms** | compute 613xN, 3 dispatches, about 20 ms. One 2304x1306 group, 21-22 passes of 74-194 draws, 13-16 ms |
| fading, t75-80 and t89-93 | 17 down to 10 ms | the same compute, 11.5, 8.7, 6.4, 5.1 ms, passes back to 4.5 ms |
| settled, after | 5-6 ms | compute gone |

- The compute job is the game's own, from `vk_nvn_replay`'s dispatch. ASTC runs on another path, and ASTC was 0.0 in these frames.
- It costs 5 to 20 ms at one size, so the cost isn't the size. What it reads or how much work it holds is unknown.
- Witch Time dispatches 5.7-7.3 a frame in the same phase, against 6-8 here, so the job likely exists there too. Its whole GPU frame is 14-16 ms, so it can't cost it 20 ms. Its per-job time isn't logged.
- Next: name the shader and what it binds. The route's fight has no such job, so this needs the user's load test again.

## What each build gives the game

- Witch Time's `OP_REPORT_VALUE` writes the value with `now_ns()` when its render thread replays the report (`nvn_gl.c:3569`), so its timestamps follow the CPU side of the replay. Its notes mention the step to 1440x816, and forcing the size down with test timestamps.
- Ours loses `ResetCounter` and `ReportCounter` in release-only mode (164 of each in 392 s, `cmd_unkept`), which is rare, so these are unlikely to be the controller's input. Which call the game's controller reads isn't known yet. Nobody here has found it in the game's code.

If the hypothesis holds, the next step is finding that input, and what ours reports against Witch Time, by reading the game's code.

## The c3 load test (`play-c3/`, 2026-11-03)

Build c3 (`2cef14fd...`) through `Launch-Bayonetta3-PassTime.cmd`, by the user, about 250 s, with a GPU record (`play-c3/vram-c3-loads.csv`, from `nvidia gpu record/`). Log hash `1889bec3...`, record `87d3efbe...`. `nvn-top:` names the largest jobs in every 30th frame.

**The drop is one fixed chain of three game compute shaders, plus one or two heavy full-screen passes.** It appeared four times (t62-66, t74-80, t145-152, t161-172) and looks the same every time:

| job | size | binds | time in the drop | time as it fades |
|---|---|---|---|---|
| compute `614fd3a3` | 613xN groups of 64, N 1-4 | 2 storage buffers, 24.6 MiB, both from video memory copies | **12-16 ms** | 6.6-7.5 ms |
| compute `66b9a528` | the same | 3 buffers, 24.7 MiB, one read from a pool in host memory | 4.2-6.6 ms | 3.0-3.4 ms |
| compute `ea51dd0e` | the same | 2 buffers, 24.7 MiB, from copies | 2.1-2.6 ms | 1.0-1.2 ms |
| pass, last fragment `1e842019` | 2304x1306 | no buffers, textures half from copies, half from pools | **10.6-10.9 ms, with 3 to 61 draws** | gone |
| pass, last fragment `e9d06cf6` (t161-166 only) | 2304x1306, 8-12 draws | the same | 10.6-17.3 ms | gone |

- **Time doesn't follow the work.** `614fd3a3` costs 7.5 ms at 613x1 and 12-16 ms at 613x2 to 613x4, so the size of the job isn't the cost. 39k threads over 25 MiB should take well under 1 ms on an RTX 3080.
- `1e842019` takes 10.6 ms for **3 draws** at 2304x1306 (t145). That is a per-pixel cost, too high for a plain full-screen effect.
- The compute chain's buffers sit in video memory copies, so host-pool reads don't explain `614fd3a3`. They could explain part of the passes, which read about half their textures from the pools.
- Witch Time's whole frame is 14-17 ms in the same phase, so it can't spend 20 ms on these jobs. The jobs are the game's own; **their cost is most likely in our translation of them** (memory access, atomics or loops as we emit them), not in the work.
- The long phase t174-230 (GPU 18-21 ms, 650 draws, no compute) is the game's heavy phase that Witch Time has too, at 14-17 ms: ours is about 1.3x there, not 2x.
- GPU record: 91-97% busy in each drop, 36-41% in the settled stretches, peak 3,832 MiB.

### Next: read the three shaders

These hashes aren't in the 2026-10-29 corpus (`nvn-port-20261029/corpus-nvn-7/`, 73 compute stages, retranslated on 2026-11-03 with `snx-shaderc` from current source: no match). The second c3 run (`play-c3-2/`, record `vram-c3-loads-2.csv`) shows the same three hashes, `614fd3a3` up to 15.6 ms, so the hashes hold across runs. **It saved no shaders: `SNX_NVN_CORPUS` works only in the shadow path (`SNX_NVN_SHADOW=1`), and the fast renderer registers programs itself**, so the first `Launch-Bayonetta3-Capture.cmd` captured nothing. Build c4 (`Bayo3_120-c4.exe`, `1aabddf7...`) is c3 plus `SNX_NVN_SPV_DUMP=dir`: `vk_nvn_shader` writes each module it makes as `<hash>_<stage>.spv`, with the game code it came from (`.bin`) and constant buffer 1 (`.cb1`), named by the hash `nvn-top` logs. `Launch-Bayonetta3-Capture.cmd` now sets it to `diagnostics/passtime-20261102/spv-c4/`. The dumps are game data: they stay local. Not run yet.

### The shaders read (2026-11-03, from `spv-c5/`)

- **Witch Time has the same three compute stages.** Its cache (`build/cache/vk/stage/5_*.st`) holds compute stages of exactly 6208, 4800 and 3008 bytes, our three `.bin` sizes, three variants each. The keys don't reproduce from our dump alone (`ssrc_hb` folds in the constant data, which `.bin` lacks), but the float math agrees to the instruction (614fd3a3: FMul 44/44, FAdd 20/20, ExtInst 54/59).
- **Ours is about twice the size**: 614fd3a3 184,600 bytes against Witch Time's 94,548 (`2c6b5cc63e0d90f1.spv`), ea51dd0e 104,260 against 55,156 (`e623f780dd35c2d0.spv`). The extra is software denormal flushing (`float_ftz`, `spirv_translate.c:442`): every FTZ float op gets And, And, IEqual, Select on each input and its result. All five slow shaders are FTZ only (110/0, 63/0, 90/0, 54/4, 46/3), and the RTX 3080 reports `shaderDenormFlushToZeroFloat32 = false` and `shaderDenormPreserveFloat32 = false` (vulkaninfo), so `device.c:228` falls back to software ("FP32 denormal flush mode unavailable" in every log). Witch Time doesn't emulate FTZ; it takes the driver's default.
- **That isn't enough to explain the time.** About 1,300 extra integer ops per thread over 39k-78k threads is well under 1 ms on this GPU, and a 613x2 dispatch costs about 7 ms (frame 2820: 613x2 x3 = 21.7 ms, ASTC 0 Mpx, so no texture decode competes).
- **Control flow is the other difference.** The CRS resolver succeeds for all three (checked offline against `shader.c`), so no run-time stack. But 614fd3a3 (SSY/SYNC and a BAR) and 66b9a528 (PBK/BRK loops, two BARs) have flow, so we run them as one `switch (pc)` per block inside a loop, and with BAR in "converge" mode (a subgroup minimum per block, lanes ahead wait). Witch Time emits structured if/else and loops (139 labels, 57 selection merges, against our 78 and 30 in 614fd3a3). Not measured yet.
- Next: an A/B build with the software flush switchable, and a reproducible way to the drop, to measure each.
