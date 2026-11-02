# NVN route phase 4: mechanism and evidence (2026-10-30)

Plan: `PLAN.md`. Runs are in `../desktop-launch-20261028/range-port-<label>` (chapter route, isolated copy, production
saves hashed before and after by `run_candidate.py`). Nothing here is deployed.

## Step 4.0: where the driver kicks, and what the stream releases

Trace: `SNX_NVN_KICK=<frame>` (`source/src/nx/hle/nvn/kick.c`). `nvn_entry` tells it which routed function runs on
each thread, nvdrv's SUBMIT_GPFIFO reports each submission with the function running on its thread
(`g_nvdrv_submit_note`), and the GPU thread reports each release and each other memory-writing launch of a job
(`g_gpu_release_note`, `GPU_RELEASE_NOTE`). Counts every 600 presents, every event of three frames in order.
`gpu_submit` now returns the job's id. Off unless set: two null-pointer checks on the paths otherwise.

Runs `range-port-kick1`, `-kick2`: exit 0, production saves unchanged, 4,800 presents. Frame listing:
`kick-events-f4000.txt` (the first 70 lines of the trace of frames 4000-4002).

**The kick is confirmed.** 9,600 SUBMIT_GPFIFO in 4,800 presents, 0 with no NVN call on the thread:

| made in | count |
| --- | --- |
| `nvnQueueFlush` | 4,800 (the frame: ~1,242 GPFIFO entries, flags 0x104) |
| `nvnQueuePresentTexture` | 4,799 (3 entries, flags 0x104) |
| `nvnQueueInitialize` | 1 |

All on one channel (syncpoint 20), each inside the call on the calling thread, none in a nested call. In a frame:
the draw thread runs `nvnQueueSubmitCommands`, `nvnQueueFenceSync`, `nvnQueueFlush` in that order; the submission
comes ~25 us into the flush. A **second thread** calls `nvnWindowAcquireTexture`,
`nvnQueueWaitSync` and `nvnQueuePresentTexture`, about 3-5 ms after the flush in these frames. So recording the host's
work at `nvnQueueSubmitCommands`, as 4.1 does, comes before that work's GPFIFO submission, which the plan needs.

**What the stream does beyond draws** (channel 20, 4,800 presents):

| method | count | in macros |
| --- | --- | --- |
| 3D SYNC_INFO (0xB2) increment | 28,567 | 0 |
| 3D report semaphore (0x6C3) release | 15,614 | 0 |
| inline-to-memory launch | 76,855 | 2 |
| copy engine launch | 23,959 | 0 |
| compute launch | 24,657 | 0 |
| puller semaphores, puller syncpoints, SUBMIT flag 2 increments, report counters, report or semaphore acquires, compute reports, copy semaphores, 2D blits | 0 | |

So every release is a 3D SYNC_INFO or a 3D report release, the report releases of the traced frames all on one
address (`0x500000000`), and **the stream has no waits**: no acquire or syncpoint wait appeared on this route.

In the traced frames the releases come as SYNC_INFO, SYNC_INFO, report release, all outside macros: one such group at
entry 23 of 1,242, after the frame's first compute launches (presumably `nvnCommandBufferFenceSync`, which the game
records once per frame here; not proven), one at the last entry (presumably `nvnQueueFenceSync`), and one in the
present job (entry 2 of 3). Job 8003 also has a further SYNC_INFO (detail `0x100014` where the others have `0x110014`:
meaning of the difference not established) with its own report release.

Consequences for 4.3:

- Macros can be skipped entirely as far as releases go: no release came from one.
- Release-only mode needs to execute, of the NVN channel: SET_OBJECT, `0xB2`, `0x6C0`-`0x6C3` of the 3D class.
- The fence in the middle of the list is the one case the plan's risk names: performed after the batch that holds it,
  it is later than on the console, never early, and with no waits in the stream it can't deadlock the stream itself.
  What the game does with it has to be seen in 4.3 (which thread waits for it, what it reads then).
- The inline-to-memory writes of the traced frames go to `0x50007xxxx`, just before compute launches: probably launch
  descriptions and constants (to confirm in 4.3, as the game must not read them). Compute launches should be the API's
  dispatches, and copy engine launches its `nvnCommandBufferCopyBufferToBuffer` and `ClearBuffer`: to confirm by
  count against the host list.

## Step 4.1: live host frames in dual mode (`SNX_NVN_LIVE=1`)

Switches: `SNX_NVN_LIVE=1` with `SNX_NVN_CMDLIST=1 SNX_NVN_IMPORT=1`; `SNX_NVN_LIVE_OUT=<path prefix>` saves the host's
picture and the renderer's every 600 presents; `SNX_NVN_LIVE_DUMP=<frame>` also saves every host image of that frame and
logs the frame's passes and which host images each draw samples. Files: `gpu/vk/nvn_res.c` "live frames",
`hle/nvn/cmdlist.c` (`queue_submit`, `present`).

Mechanism:

- At `nvnQueueSubmitCommands`, before the driver's own call, `vk_nvn_live_window(1)` takes `vk_lock`, ends the
  renderer's pass and takes its `vk_cmd()`. The list's commands are replayed into that command buffer, and
  `vk_nvn_live_window(0)` lets go. The renderer sets all its pipeline and dynamic state on every draw, so nothing of
  its state has to be restored.
- Descriptor sets come from `vk_desc_alloc`, constant data from `vk_stage` (the frame's ring), so frames in flight are
  the renderer's. Images stay from frame to frame (`RP_TARGETS` 256), retired through `vk_defer_image` at
  `nvnTextureFinalize` and `nvnTextureInitialize` and when an address comes back with another size.
- `rp_tex_ready` runs before a draw's descriptor sets exist: a texture lookup can upload, and a layout change can't
  be recorded in a pass, so the host's pass ends first when either is needed, and the command buffer is taken up
  again. `g_tgen` moves at each window, so texture lookups never outlive what the renderer may free between windows.
- A texture stands for a host image only when its TIC has the image's size (`rp_target_of`): address 6398ef000 is
  720x408 and 800x448 at different times.
- **Clip distances:** with `SNX_NVN_KICK`, `SetUserClipEnable` (3D 0x544) logged the single value `0xFF` on the route
  (run `kick3`). The host's default was 0; it is now 0xFF. Street frames went from about 1.5/255 to about 0.2/255.

Run `live6` (full route, chapter route, dual mode; exit 0, saves unchanged): 2,633,498 of 2,633,498 draws, 151,784 of
151,784 clears, 24,298 of 24,298 dispatches, none left out. Host vs renderer:

| frame | scene | mean/255 | pixels over 16 |
| --- | --- | --- | --- |
| 600 | title, dark phase | 2.61 | 4.63% |
| 1200, 1800, 3000 | menus, loading | 0.00 | 0.00% |
| 2400 | chapter select | 1.99 | 1.37% |
| 3600 | street | 0.20 | 0.37% |
| 4200 | street | 0.24 | 0.44% |
| 4800 | street | 0.22 | 0.43% |

Memory: flat from second 150 to the end (private ~10.8 GB, GPU ~6.7 GB; without the host path ~8.8 and ~4.8 GB). The
extra is images and the staging ring, which carries ~60 MiB a frame on average (`live6`: 285,799 MiB in 4,800 frames).

**Exit of 4.1: not fully met.** No draw left out and no growth, street well inside 2/255, but two scenes differ:

1. **Title, dark phase (2.6/255):** red glow missing from the host's picture in the scene's dark phase. Not explained.
   One run (`l9`) came out at 0.19 and `l4` at 0.10, but `l4` was a bright phase of the same scene, so those numbers
   compare different moments, not a fix.
2. **Chapter select (2.0/255):** the medallions and red paths are black in the host's scene target. **This is intermittent**:
   runs `front2`, `front3` and all the `lN` that reached it show it, run `front1` (same code but for log lines) did
   not, and the one host-only run that reached it, `rel1`, shows the medallions right (one run, not proof). Ruled out one by one: a size-mismatched substitution, a
   vertex stream outside the pools (0 of them), the array at `64b1c0000`. A feedback loop was the working theory for
   a while and is **wrong**: the log shows `64f7a0000` is never sampled while bound. Excluding it from substitution
   "fixed" the picture only because that swaps the renderer's finished scene in. The feedback copy
   (`rp_feedback_copy`) stays as correct handling of a case that didn't occur here.
   Likely cause, not proven: in dual mode the host's frame runs ahead of the renderer, whose results it reads from guest
   memory, so a one-off result can be missing from the host's frame. Release-only mode has no such race.

## Step 4.2: host picture (`SNX_NVN_FRONTEND=2`)

`SNX_NVN_FRONTEND=2` implies command lists, imported pools and live frames. `nvnQueuePresentTexture` registers the
window texture's CPU address (`gmmu_translate` of its GPU address) with its GPU address. In `vkr_present_buffer`,
`vk_nvn_live_surf(cpu)` wraps the host's image in a `Surf`, so `show` and screenshots use it as they use a render
target; `vk_rt_for_cpu` runs only when the host has none. `show` leaves it in TRANSFER_SRC, and the next window brings
it back to GENERAL. The renderer still translates, so fences stay exact.

Run `front1` (exit 0, saves unchanged, 4,800 frames, 11.8 FPS at the end): every present of `front2`'s first ones logged
"the host's image"; boot, title, menu and street come out right in `front1-sheet.png`. Runs `front2`, `front3` show the
intermittent black medallions of 4.1.

## Step 4.3: release-only stream (`SNX_NVN_FRONTEND=2 SNX_NVN_RELEASE_ONLY=1`)

- `gpu.c` `method_call`: with `g_gpu_release_only`, of everything at 0x40 and up only 3D `0xB2` (SYNC_INFO) and
  `0x6C0`-`0x6C3` (report semaphore) run. Puller methods still run. Macros, draws, compute, copies and inline writes
  are skipped. The scan at submission takes no copy snapshots. 4.0 shows those two are the only releases on the route,
  and that the launches don't come from macros. Whether a macro ever sets the payload registers wasn't measured, but
  the route ran to its end, which wrong fence values would not.
- `vk_nvn_live_window(0)` ends the list with a barrier to the host stage and `vk_submit`, and gives the submission to
  `vk_release_host_gate`. `release()` then can't perform a release at once while that submission runs: it queues it
  behind the gate as usual (`vk_submit_done`, `gpu/vk/release.c`).
- Writes are real: `rp_ssbo` binds the pool itself and reports `gpu_mem_written`; copies and clears go to the pool;
  uniform buffers bind the pool where size and alignment allow, because a compute pass of the frame may fill them.
- `nvn.c`, `cmdlist.c`: `SNX_NVN_FRONTEND=2` turns on what it needs.

Run `rel1` (chapter route, exit 0, saves unchanged, 138 s against 167 s for the fallback):

```
nvn-live: 4800 frames: draws 2428779, drawn 2428779; clears 166134, done 166134; dispatches 28692, done 28692; left out 0
vulkan: 0 draws, 0 skipped; 0 dispatches in 0 batches (0 ms waiting), 0 skipped; 15 finishes (67 ms)
```

**The renderer drew nothing, and the game ran the whole route on the host path's picture and the stream's releases.**
Boot, title, chapter select with correct medallions, street: `rel1-sheet.png`, `rel1-cmp.png` (the game's timing moves
the camera between runs, so these compare by eye). The fence in the middle of the list is performed after the whole
list; no stall showed on this route.

**But the picture is not right yet (open, the top item for the next session):** on the street the blue energy ribbon in
the sky is missing, and the shop signs at the right are dimmer. The pause screen's purple background is darker
as well. Compare `m7-sheet.png` (ribbon there) with `m3-sheet.png` (left three: host-only, no ribbon; right three:
fallback).

Tried, in order, on the street:

| test | run | result |
| --- | --- | --- |
| stamp written ranges for the texture cache only once the GPU has run them (kept) | `m3rel` | ribbon still missing |
| any texture whose guest memory is all zero (first 64 KiB), at frame 4400 | `m5rel` | none |
| uniform buffers copied instead of bound (`SNX_NVN_REPLAY_DEBUG=256`) | `m6ubo` | still missing |
| host's compute output against the renderer's, range by range (dual mode, frame 4400) | `chk1` | match: small differences only |
| every `nvnCommandBuffer*` the game calls against those kept | | all kept, but for Initialize and AddCommand/ControlMemory |
| **real writes with the stream still run (`SNX_NVN_RELEASE_ONLY=2`)** | `m7both` | **ribbon there**, 13.8 FPS |
| release-only capped at 15 FPS | `m8lim` | not comparable: the scripted route went elsewhere |

So the host's write path is sound and **something the skipped stream does is lost**, or the game behaves differently
at the higher frame rate (every run with the ribbon ran at 23 FPS or less, every run without at 29 or more; `m8lim`
didn't settle that). The two to test next: which stream methods the ribbon needs (add methods back to the release-only
filter by engine: inline-to-memory first, then copy, then the 3D state), and a release-only run capped at about 25 FPS
on a route that doesn't diverge. The gun seen in the host-only runs at frames 3000-3600 is probably timing (loading
ends sooner, the script is by frames); it goes away by frame 4200.

Other gaps of 4.3: the title's dark phase can't be compared with the renderer in this mode; CPU reads of render targets
(`nvnTextureReadTexels`, photo mode) find guest memory unwritten; functions outside the 49 `nvnCommandBuffer` commands
kept are lost silently with the stream skipped; other chapters and cutscenes are not run yet.

## Step 4.4: measured, with the defect above

Street route (`runs_street.py`), 4,800 frames, one run at a time, production saves unchanged in all. Last two 600-present
intervals:

| build, mode | run | FPS | draws in the last 600 |
| --- | --- | --- | --- |
| installed `e24658fe...` | `m1inst` | 21.60, 21.68 | |
| this build, fallback | `m1fb` / `m9fb` | 22.51, 22.63 / 22.52, 22.60 | |
| this build, host only | `m1rel` / `m3rel` | 29.10, 29.57 / 28.97, 29.38 | ~571 k |
| this build, host only | `m2rel` | 41.72, 43.89 | ~596 k, 0 clears a frame |

**About +35% over the installed build (29.4 against 21.7), on a picture that is missing an effect**, so the number is
provisional: the missing work may be part of the gain. `m2rel` at 44 FPS is one run in a state that hasn't come back, with
no clears in the last intervals (about 4 a frame otherwise). It is not a result until it is explained. In these runs the
host path still carries its debug statistics (`check_list`), and ~60 MiB of staging copies a frame. Phase 5 starts here.

Fallback: same route, same FPS class as the installed build (22.6 against 21.7), draw counts in the same range, 0
skipped; the 8 targeted tests pass (`nvn_seam render_enable instance_array draw_texture write_pages nvdrv_service
platform_win32_stack fermi2d_blit`). Final build `Bayo3_120-phase4.exe` here, `379ac4c5...`, not deployed. `output/` and
the saves untouched: hashes equal to the session's start.
