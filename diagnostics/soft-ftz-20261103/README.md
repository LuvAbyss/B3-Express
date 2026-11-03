# Software denormal flush A/B (c6, 2026-11-03)

Why: the drop after a verse exit is three game compute shaders (`614fd3a3`, `66b9a528`, `ea51dd0e`) and a full-screen pass (`1e842019`), see `diagnostics/passtime-20261102/README.md`, "The shaders read". Our SPIR-V for them is about twice Witch Time's for the same machine code, and the difference is `float_ftz` (`source/src/nx/gpu/shader/spirv_translate.c`): each FTZ float op gets And, And, IEqual, Select on its inputs and result, because the RTX 3080 reports `shaderDenormFlushToZeroFloat32 = false` (and no preserve mode either), so `device.c` can't ask for flushing by execution mode. Witch Time doesn't emulate FTZ.

## Build c6

c5 plus `SNX_SOFT_FTZ=0` (`g_shader_soft_ftz`, `shader.h`): `float_ftz` returns its value unchanged, and the driver's denormal behaviour applies. Default unchanged (1). The log says `vulkan: SNX_SOFT_FTZ=0: ...` when it's on.

- `Bayo3_120-c6.exe`, sha256 `60ca23665c59614da1616366c7b415fcd7d4932a28f52a3433f390f7c53f10f8`. INSTALLED in `output/Bayo3_120` 2026-11-03 14:47 at the user's request (they chose to play each setting), `deployment-c6.json`.
- Rollback: `preserved-installed-c6/Bayo3_120.exe` is c5 (`167cc825...`), with the log as it was (`Bayo3_120-before-install.log`, the user's recording session).
- Offline check (scratch harness linked against the translator objects, code = 80 zero bytes + `spv-c5/614fd3a3_5.bin`, local 64): 184,548 bytes with the flush (the game made 184,600), 128,900 without; BitwiseAnd 1118 to 366, Select 481 to 105, IEqual 475 to 99, FMul 44 and FAdd 20 both times.

## The A/B

Same spot both times, a verse exit (the drop lasts about 10 s after it):

1. `Launch-Bayonetta3-PassTime.cmd`: flush on, as c3 to c5.
2. `Launch-Bayonetta3-NoFTZ.cmd`: flush off.

Compare `nvn-pass`/`nvn-top` for the four jobs. The first NoFTZ run compiles new pipelines (other SPIR-V), so its frame times at loads hitch more; the GPU times by job still count.

Reading it: if `614fd3a3` falls from 12-16 ms to a few ms, the flush was it (then: make it the default on devices without a flush mode, and check the scenes for denormal artefacts). If it barely moves, the flow loop is next (structured control flow for shaders whose stack resolves, as Witch Time emits).

## Run 1: flush on (`play-c6-ftz-on/`, 2026-11-03, the user, log `fa2fd250...`)

The same picture as c3: `614fd3a3` 14.5-16.0 ms at the peaks (t64-68, t78-81, t149-152), drops at t60-87, t146-157, t162-177. New: **at the same size (613x2) its time ramps from 1.3 ms to 15.6 ms and back to 0.5 ms over about 10 s.** The cost follows data, not dispatch size, and the shader has no loop (only the dispatcher), native atomics only (`AtomicIAdd`, no compare-exchange loop), direct dispatches (not indirect).

**The cause found by reading, 2026-11-03:** `rp_stage_set` (`nvn_res.c`, the storage buffer loop) binds a storage buffer the program **writes** (`gbuf_wmask`) to `g_import[pool].buf`, the pool itself, which is guest memory imported from system RAM (`VK_EXT_external_memory_host`). Only read-only buffers get the video memory copy (`rp_read_buf`). So every LDG/STG of these jobs (614fd3a3: 43 loads and 37 stores per thread, into 24.6 MiB) crosses PCIe, scattered. About 78k threads x 80 accesses x 64 bytes is about 400 MB, about 16 ms at PCIe 4.0 speed: the order of the measured cost, and it follows how many threads do the full path (the data). This is the parked "VRAM step 3" (GPU-written memory in video memory).

The software flush is likely a minor share; the NoFTZ run is optional now.

## Run 2: flush off (`play-c6-ftz-off/`, 2026-11-03, the user, log `4c0d18c9...`)

The hashes change with the SPIR-V, so the jobs are matched by size and buffers: `614fd3a3` is `d0d68015`, `66b9a528` is `fd958edc`, `ea51dd0e` is `8964e730`, pass `1e842019` is `99113cb7`.

| job | flush on: max / mean | flush off: max / mean |
|---|---|---|
| 614fd3a3 (2 buffers) | 16.0 / 7.81 ms | 16.0 / 7.81 ms |
| 66b9a528 (3 buffers) | 6.6 / 4.09 ms | 6.3 / 4.09 ms |
| ea51dd0e (2 buffers) | 2.8 / 1.53 ms | 2.4 / 1.38 ms |
| pass 1e842019 | 11.4 / 10.84 ms | 11.4 / 10.70 ms |

**The software flush costs nothing measurable at the drop** (at most 0.15 ms on the smallest job). The default stays as it is. The cost is memory, as the reading of run 1 found: next, written storage buffers in video memory (c7).

## How Witch Time handles GPU-written storage buffers (read 2026-11-03)

`runtime/nvn_vk_draw.c` (GPL-3.0-or-later), the "CB" (compute buffer) design, comments at `g_cb_keep`, functions `cb_new`, `cb_land`, `cb_kept_land`, `cb_land_start`/`cb_land_finish`, `cb_sync`, `cb_sync_needed`, `cb_masks`; dispatch at about line 3055:

- A storage buffer a dispatch **writes** gets a GPU-side buffer of its own (`cb_new`, device-local when it can), filled from guest memory, with a snapshot of the guest bytes at fill time. Read-only buffers are read from guest memory as before.
- The translator's `track_writes` sets bit k of a mask word when buffer k was actually written (`atomicOr(mx_wm, 1 << k)`); after the batch, `cb_masks` marks only those dirty.
- **Copy-back is a word-by-word merge** against the snapshot: a word compute changed goes back, a word the game changed meanwhile stays the game's (conflicts counted). That is the answer to the risk that parked VRAM step 3.
- **Kept buffers (`WT_VK_CBKEEP=1`, default)**: after the batch the buffer stays on the GPU; the next dispatch into the same range takes it with no copy either way. It lands in guest memory only when needed: a read of an overlapping range not served by it, a new compute buffer over a different extent, a guest-memory command (buffer or texel copies, counters), two batches without a dispatch into it, or a CPU write the write watch sees (then merge and refill).
- Landings go through a host-cached staging copy made by the GPU (`cb_land_start`), merged when that submission finishes, so the CPU never reads write-combined device memory (19-20 ms for the largest buffer, their note).

Plan for c7: port this to `nvn_res.c` in two steps, credited in `source/docs/CREDITS.md`. (a) Per-batch buffers with snapshot, write mask and merge after the batch. (b) Kept buffers and the landing causes.
