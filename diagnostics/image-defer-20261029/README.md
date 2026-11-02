# Deferred graphics storage-image landing (2026-10-29)

Not deployed. Built from current source (full CMake build, `SNX_DEPLOY=OFF`) into `candidate/Bayo3_120.exe`
(`568e489b...`). Source baseline before this change: `source-baseline/Bayo3_120.exe` (`ff15f742...`, street
19.80/19.89 FPS, run `range-srcbase-1`). Production runtime, module, INI and saves unchanged in every run.

## Why

`../image-land-overlap-20261029/README.md`: every graphics storage-image landing (9,000 per run, ~35 s) happened at
WAIT_FOR_IDLE or INVALIDATE_TEXTURE_DATA_CACHE, each a `vk_finish()` plus readback, detile and merge.

## Change (`source-changes.patch`)

- `gpu.c`/`gpu.h`/`engines.c`: those two methods call `gpu_barrier_point()`, which the renderer handles through
  `g_gpu_barrier` (falls back to `gpu_signal_point` without one). Job end, syncpoints and releases are unchanged.
- `renderer.c` `vkr_barrier`: what a signal point does, except pending images are copied (`vk_gfx_image_copy`).
- `surface.c`: `surf_flush` split into `surf_copy` and `surf_merge` (the synchronous path is the same sequence).
- `render.c`: a copied image records its submission and back-ring slot, and merges when that submission is done
  (`vk_gfx_image_frame_done` from `land_through`, images before buffers per submission), or earlier when read
  (`vk_gfx_image_mem_need`, signal-point land-all: wait for that submission only). A copy made stale by a later draw,
  or whose ring is recycled first (`vk_gfx_image_back_reset`), lands from the image as before.
- `buffer.c` (`gfx_gpu_src`, `atlas_fill`) and `render.c` (`cb_get`): buffer reads merge copied images first.
- `SNX_VK_IMAGE_DEFER=0` restores landing at the barriers, in the same binary.
- Tests: `instance_array` now asserts the two methods reach `gpu_barrier_point` and not `gpu_signal_point`; the
  other fixtures stub it. Seven targeted tests pass. `land_target` does not compile (volk include path), as before.

## Result (`comparison-control1-candidate1-candidate2-control2.json`, runs `range-defer-*`)

| | finishes / 600 presents | finish wait | FPS, last two intervals |
| --- | ---: | ---: | --- |
| control (`DEFER=0`) | 1,201 | ~3.4 s | 19.76 19.76 / 20.00 20.00 |
| candidate | 1 | ~1 ms | 19.74 19.97 / 19.97 19.98 |

Mechanically it works: steady-state `vk_finish` calls go from 1,201 to 1 per interval. Draws, dispatches and every
periodic screenshot's mean colour match between arms. **Frame rate does not change** (19.88 vs 19.92). Process CPU
1.5 cores and GPU utilisation 39% in both.

## What this establishes

Removing ~11% of wall time in GPU waits gave no FPS. The earlier working assumption, that these waits limit the frame
rate, is falsified for this route. Profile sample totals: game code ~0.6 cores, runtime ~0.2, most threads blocked.
Hypothesis (not yet measured directly): the GPU thread's translation (~65% of a core, ~33 ms/frame) and GPU execution
(~37%, ~19 ms/frame) run one after the other, because the game waits for a frame's fence before the next
submission. Next measurement: per frame, timestamps for GPFIFO job arrival, the GPU thread finishing it, the Vulkan
submission's completion and the syncpoint release. If confirmed, the lever is overlap (submit translated work in
chunks during the frame), for which removing whole-queue waits like these is a precondition.
