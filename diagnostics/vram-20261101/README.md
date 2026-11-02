# VRAM work (from 2026-11-01)

Plan and reasons: `../witch-time-study-20261101/README.md`, "VRAM". Edits: `edit-0.py`; originals of the nine files before them: `pre-edit/`.

## Step 0: memory log, done

`Bayo3_120-step0.exe` (`2a2bad8d...`): phase5i source plus a line every 600 frames, `nvn-live: video memory MiB`. Allocations carry their kind as VMA user data (`vk_mem_tag`, `vk_mem_untag`, `vk_mem_log` in `device.c`). Process use and budget come from `VK_EXT_memory_budget`, enabled where the driver has it. Not installed.

Runs: isolated, scripted fight `route-remnant9.txt`, 2560x1440 (screenshots checked), 12,000 frames, O1 DLL, warm cache, idle PC, `run-0.sh`. Logs in `../desktop-launch-20261028/range-port-m0a`, `m0b`, `m0c`. Saves unchanged in all three. Not the Remnant 10 of the matched run.

| run | build | blocks | last FPS windows | worst frame ms | frames over 50 ms | nvidia-smi peak MiB |
| --- | --- | --- | --- | --- | --- | --- |
| m0a | 5h2, staged by mistake, no log | 256 | 58.5, 58.9, 59.9 | 1,102 | 176 | 8,955 |
| m0b | step 0 | 256 | 59.4, 59.5, 60.0 | 1,126 | 165 | 8,806 |
| m0c | step 0 + `SNX_VK_BLOCK_MB=64` | 64 | 59.5, 58.4, 59.9 | 1,098 | 170 | 9,129 |

The log costs nothing visible, though one run each can't show a small cost. m0a isn't the same source as m0b.

m0b at frame 12,000, MiB: pool copies 2,333, ASTC as RGBA8 1,837 (313), targets 636, other textures 150, BC 4, staging 40, untagged 7, unused in VMA blocks 1,488, outside VMA 143, process 6,642 of budget 9,283. Process + 1,433 idle = 8,075, `nvidia-smi` read 8,060. ASTC peak 2,623 MiB (511 textures) at about frame 3,600. The 8,806 peak falls between log lines.

- Against the study's estimate: ASTC is about 1.9-2.6 GB as RGBA8, so BC3 saves about 1.4-2.0 GB, a little under the estimated 2.5.
- Pool copies: 2.1 GB to save.
- Unused in blocks, 1.2-1.5 GB after the load at frame 3,600, wasn't in the plan. Peak textures free up, but the holes stay inside blocks.

## 64 MiB blocks: rejected

Witch Time uses 64 MB blocks, so tried. Unused rose to 1,761 and process to 7,106 MiB at the end, peak 9,129. One run, but the wrong direction, so the setting was taken back out (`revert-0b.py`) and `Bayo3_120-step0b.exe` is only a record. Mechanism not proven: more small blocks each kept alive by one surviving texture fits. Lowering the texture peak with BC3 should shrink the holes, and step 0's log will show it.

## Next

Step 1: ASTC to BC3, our own encoder, decode ahead of the draw on worker threads. Check sRGB, alpha, upload time and the picture against RGBA8.

## Steps 1 and 2: done, not installed (2026-11-01)

**Build: `Bayo3_120-step2.exe` (`85692a0b...`).** Source is phase5i plus the log, BC3 and sparse copies. `Bayo3_120-step1.exe` (`2d3b0c3e...`) has the log and BC3. Edits: `edit-1.py`, `edit-2.py`; originals in `pre-edit-1/`, `pre-edit-2/`. New files: `source/src/nx/gpu/vk/shaders/bc3.comp`, `bc3_spv.h`, `source/scripts/build-bc3-shader.py`.

**Step 1, ASTC to BC3.** `astc.comp` still decodes on the GPU, then `bc3.comp` encodes each 4x4 block: colors from the principal axis, each texel the nearest of four, alpha from its lowest and highest. Our own code, no port. Off with `SNX_VK_ASTC_BC=0`, and off where the GPU lacks BC. Scratch 8 to 16 MiB, plus 16 MiB for blocks. Strips take whole fours of block rows.

**Changed from the plan:** on the GPU at the use, not on CPU worker threads ahead of the draw. Our decode is already on the GPU, and the 2.6 GB peak is about 650 Mpx a load, which a CPU route would move onto the game's cores. So this lowers VRAM only. Texture hitches at loads stay as they were, and decoding ahead stays open for the freeze work.

**Step 2, sparse copies.** Each copy is a sparse buffer with the pool's offsets, so ranges across blocks stay contiguous. Memory comes from 16 MiB arenas, bound per 64 KiB block when first copied, waited for by a fence (`mir_bind`). A block stays bound after being taken back, so memory can only grow during a session, up to the old 2.3 GB. Off with `SNX_NVN_VRAM_SPARSE=0`, automatic fallback to whole copies.

Same isolated setup as step 0. Saves unchanged in every run. `nvidia-smi` peak includes the idle desktop, about 1,400 MiB.

| run | scene | build | peak MiB | process MiB at end | last FPS windows | GPU ms | worst ms | over 50 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| m1b | fight | RGBA8, whole copies | 8,819 | 6,642 | 59.2, 59.5, 59.5 | 9.41 | 1,080 | 175 |
| m1a | fight | BC3 | 6,053 | 4,598 | 58.9, 59.5, 59.5 | 9.47 | 1,106 | 173 |
| m2a | fight | BC3, sparse | **3,753** | **2,265** | 58.7, 59.5, 59.5 | 9.41 | 1,112 | 177 |
| m2b | fight | same, `RUN_CHECK=1` | 3,728 | 2,265 | 59.1, 58.8, 59.4 | 9.65 | 1,101 | 179 |
| m1d | street | RGBA8, whole copies | 8,424 | | 44.1, 41.8, 42.0 | | | |
| m1c | street | BC3 | 5,784 | | 43.9, 41.7, 41.7 | 12.34 | 1,089 | 158 |
| m2c | street | BC3, sparse | 3,607 | 2,014 | 44.1, 41.6, 41.7 | 12.93 | 1,082 | 157 |

The game itself: about 7.4 GB to about 2.3 GB, in this fight. No FPS, GPU or hitch cost above run-to-run spread, one run each.

**Stale check, m2b:** 9,364 blocks read back, 8,631 equal, 733 written since, 0 stale.

**Picture.** Still frames against RGBA8: mean difference about 1/255, street 3-6/255 with the scene's own motion in it (two RGBA8 runs differ by up to 7 in the same early frames, more once the fight drifts). Crops checked by eye, fight, game-over text and street: no corruption, text sharp, a very faint tint in soft gradients. Not seen: faces and metal up close, normal maps by eye, full-screen movies, other chapters. BC3 is lossy, so only play can judge it.

**Binding.** 300 ms a run, at the load at 73-94 s, up to 265 ms in one 10 s window. Afterwards 0-3 ms a window in the windows sampled. Each call waits for a fence, so a new area in play could cost a late frame, though worst frame and frames over 50 ms didn't change here. If play shows it, use a semaphore on the next submit instead.

Unused in blocks fell from about 1,665 to about 610, as expected: the texture peak was the cause, not the block size.

## Next

Step 3 needs a measure before any code: 34% of combat reads still come from the pools, and 414 blocks stay there because the GPU writes them. That is GPU-written data the game may read, so a ring alone won't do. Measure by binding kind and size first.

## Step 3 measured, parked (2026-11-01)

`Bayo3_120-step3m.exe` (`efbf021a...`, `edit-3m.py`, taken back out afterwards: source equals step 2). Run m3a, fight, a late 600 frames:

| kind | from copies | from pools, GPU-written | waiting | written since | MiB of ranges | blocks |
| --- | --- | --- | --- | --- | --- | --- |
| vertex | 771,133 | 63,916 | 432 | 34 | 20,660 | 132 |
| index | 277,919 | 4,523 | 32 | 9 | 0 | 4 |
| uniform | 1,913,012 | 1,289,102 | 2,453 | 1,297 | 735 | 57 |
| storage | 2,350 | 170,844 | 0 | 0 | 37,404 | 289 |
| written storage | 0 | 165,275 | | | 50,016 | |

Ranges are what is bound, not what a shader touches, so MiB is an upper limit. Waiting and written since are about 0.2%, so the copies' timing isn't the problem. Over 99% of misses are blocks the GPU writes, or the host writes in command order (`nvnCommandBufferUpdateUniformBuffer` and copies go through `rp_stamp`). About 275 written storage binds a frame write to system memory, and vertex reads follow them: likely skinning, not confirmed.

A ring doesn't address this. The fix is video memory as the truth for those blocks, with a GPU copy back to the pool in order, before any fence the game can see. Blocks shared with CPU data and fences mid-window make it the riskiest change so far. Gain unknown: Witch Time's ring gave them 12.5 to 9.7 ms. Fight GPU time is 9.4-10 ms here, and matters at about 1,500 draws in Remnant 10 (17-19 ms). Parked until freezes are done.
