# The 200-300 ms frame at a scene change (2026-11-03)

Replay of `recordings/20261103-122329` (isolated copy, fast renderer). Every run has two frames of 175-315 ms,
"before the first window", at the scene changes around frames 678 and 935.

## Cause

`SNX_NVN_PASSTIME=1` (GPU time every frame, `desktop-launch-20261028/range-gpu-every`): the frame before the spike
decodes **233.6 Mpx of ASTC** for the new scene's first draws, and its GPU time is **288.9 ms, 276.0 of it "between"
passes** (the decode, BC3 encode and copies). The next frame's game thread waits for that work in the game's own NVN
driver, which spins on a semaphore with `NvOsGetTimeUS` (sdk `e538cd0`, the loop at `e538ccc`: poll `[x26]` against
`w25`, and every 100 ms `e536140`). The CPU side of those uploads is cheap (`astc-ahead` lines, 11-32 ms a load), which
first hid it. With BC3 storage off (`SNX_VK_ASTC_BC=0`) the batch takes 328.9 ms: the decode itself is the cost, at
about 0.85 Gpx/s (`astc.comp`, one invocation per block: 64-100 texels each for the game's 8x8 and 10x10 blocks; every
level a decode, an encode and a copy, serialized on one scratch buffer).

## Ruled out on the way

- The pipeline cache save (494 MB, every 5 s, on the frontend loop that ticks vsync): moved to a thread of its own
  (`runtime/main.c` `save_in_background`) anyway; the spikes stayed (`range-bgsave-*`).
- `nvnMemoryPoolInitialize`: our host import of each pool took 0.2 ms a MiB on the game's thread (313 ms for the 1.3 GB
  pool), but at boot, not at these scene changes (`range-slow-3`). Now imported on a thread of its own
  (`nvn_res.c` `import_thread`; a lookup waits for an import still running); the GPU self-checks are `SNX_NVN_POOL_CHECK=1`.
- Shader preparation and pipeline compiles: 0-1 draws waited.

## Tools added

- `--profile=1`: a frame whose time before its first window is over 150 ms logs each busy thread's functions in that
  span (`profile:` lines; `profiler_win32.c` `plat_prof_spike`, with module names for system libraries).
- `SNX_NVN_SLOW=1`: NVN calls of 50 ms or more, by name (`hle/nvn/nvn.c`).
- `astc-ahead:` every 10 s: first ASTC uploads, their render-thread time, and how many were registered 30+ frames before.

## The fix: the decoder batched (build a1)

Measured offline on the 3,815 levels of real blocks the replay decodes (`SNX_ASTC_DUMP=dir` writes them; they stay in
the session scratchpad, never in git) with a bench built on `tests/astc_vulkan.c`: 609 Mpx in 431 ms before. Timed
level by level: **71 us a level of fixed cost (272 ms) and 0.2 ms a Mpx of decoding (122 ms)**. Every level was decode,
barrier, encode, barrier, copy on one scratch buffer, and on NVIDIA each barrier between dispatches drains the GPU: 1,000
small levels take 7.9 ms in one batch and 210 ms flushed one by one.

- `vk/astc.c`: a texture's levels decode into their own parts of the scratch buffers (now rings), and
  `vk_astc_flush` (end of `tex_upload`) runs one barrier, all BC3 encodes, one barrier with the image's move to
  TRANSFER_DST (moved there from the start of `tex_upload`), and all copies. The rings' earlier readers are waited for
  once a submission and when a ring wraps.
- `texture/astc.comp`: two phases per workgroup of 32 blocks; the block decode into workgroup memory, then the 32
  invocations make the texels together, row by row (9% alone).
- Bench: **609 Mpx in 174 ms (3.5 Gpx/s, 2.5x)**; every level byte-for-byte equal to the CPU decoder; the random-block
  suite (`tests/astc_vulkan.c`, now flushing) passes. No SPIR-V validator or validation layer was available.
- In the game (A/B on the replay, `range-astc-old` = s2, `range-astc-new` = a1, `SNX_NVN_PASSTIME=1`): first scene change,
  233.6 Mpx, GPU **288.8 -> 148.3 ms**, its frame **264 -> 117 ms**; second, 310.9 Mpx, **262.9 -> 110.6 ms**, frame
  **214 ms -> under 100**. Screenshots every 150 frames the same but for timing (the title's random scene, a moving photo).
- `Bayo3_120-a1.exe`, sha256 `7a8d599a51004f2d90b479acf3ee740f6811a4b6c972a548629456f3f52e4d0c`: c7 + prompts + shaders
  in order + background saves and imports + this. NOT installed.

## Across textures (build a2)

The batch is no longer flushed at the end of each texture: a texture whose copies wait has `astc_pending` set, and
`vk_surf_use` flushes the batch (outside a rendering pass) before anything uses or changes the image; so do `surf_free`
and `vk_submit`. The fast renderer ends its pass before a pending texture (`nvn_res.c`, its sampled-texture loop). The
batch otherwise ends when a 16 MB scratch ring wraps.
- Bench: **609 Mpx in 133 ms (4.6 Gpx/s)**, byte for byte as before; the random-block suite passes.
- Game (`range-astc-x`): the first area change, 227.9 Mpx, GPU **121.3 ms** (a1 148.3, before 288.8), its frame **101 ms**
  (a1 117, before 264); the second, 310.9 Mpx, **95.3 ms** (a1 110.6), frame under 100. Screenshots match a1 within noise.
  No Vulkan errors in the log.
- Not checked: the old renderer (not the release path), which may call `vk_surf_use` on a sampled texture inside its
  rendering pass; the flush ends that pass there.
- INSTALLED in `output/Bayo3_120` 2026-11-03 at the user's request (`deploy-a2.py`, `deployment-a2.json`); rollback to c7 in `preserved-installed-a2/`, saves unchanged.
- `Bayo3_120-a2.exe`, sha256 `3b78e3908a2b4d087aae79fa82df04e8ae351e9f2d5b51d6dd84a2edcfd42b00`: a1 + this. NOT installed.

Left: the per-block decode itself (about 120 ms of the 133 in the bench).
