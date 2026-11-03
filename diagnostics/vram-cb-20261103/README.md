# c7: storage buffers the GPU writes, in video memory (2026-11-03)

Why: the drop after a verse exit is game compute jobs whose written storage buffers were bound to the pools, guest memory imported from system RAM, so every access crossed PCIe (`diagnostics/soft-ftz-20261103/README.md`). The software FTZ flush was ruled out by the user's A/B.

## Design (ported from Witch Time's compute buffers, GPL-3.0-or-later, credited in `source/docs/CREDITS.md`)

In `source/src/nx/gpu/vk/nvn_res.c`, built on the pools' video memory copies (64 KiB blocks at the pool's offsets):

- **`MIR_GPU`**: a block a storage buffer the GPU writes is bound to (`mir_own`, from `rp_stage_set` for any stage). Filled from the pool when taken, with a snapshot of the guest bytes; the copy is the truth until it lands. Every read path that prefers the copy (`rp_read_buf`: vertex, index, constants, read-only storage, now indirect arguments too) reads it; a read partly in the copy fills the rest there.
- **Landing** (`own_merge`): the GPU's bytes back into guest memory where the guest still holds the snapshot's byte, so a byte the game wrote meanwhile stays the game's.
  - **Idle** (`own_idle`): a block the GPU hasn't written for 2 frames lands without a wait: copied to host memory with a window, merged once that submit is done.
  - **At once** (`own_land_now`, submit and wait): only where the CPU must read the bytes: a constant buffer copied on the CPU, a texture over the range, a buffer written in the pool, no memory to own, no write tracking.
- **The game writing a GPU block** (the write watch at a window's opening): `own_patch` puts exactly the bytes the game changed against the snapshot into the copy (and the snapshot), no wait; the GPU's bytes stay. More than 2048 separate runs in a block: it lands instead.
- **Buffer copies and clears** into a range the GPU owns go into the copy (`vk_nvn_replay_buffer`); sources are read from the copy when it holds them.
- `SNX_NVN_CB=0`: off, as before c7. Log: `nvn-live: GPU-written storage buffers: ...` and `landed at once because: ...` with the video memory line.

## Builds

- `Bayo3_120-c7.exe`, sha256 `01bc74812dd4a613061533fef8d3284c27c21cd469c8c4519268c7b94d19852a`: c6 plus the above. INSTALLED in `output/Bayo3_120` 2026-11-03 15:35 at the user's request (`deployment-c7.json`); rollback to c6 in `preserved-installed-c7/` (with the log as it was).

## Checks (isolated copy, replay of `recordings/20261103-122329/`, fast renderer, 75 s, `desktop-launch-20261028/range-c7b-cb0` off, `range-c7d-cb1` on)

- The replay doesn't reach the verse (known), so this checks correctness and the cost in the street, not the drop.
- **Street, t 52-72 s: median 23-24 ms and 1% low 28.6-31.2 FPS off; 17 ms and 45.5-47.6 FPS on.**
- Landings at once in steady state: 0 (0.0 ms waiting); idle 40 blocks/s; the game's writes patched about 150 times/s (10-20 KiB). Pool writes over GPU blocks: 0.
- Screenshots: frame 1500 identical off and on; the two on runs agree within 1% through frame 2400; the off run's route drifts from frame 1800 (timing: the on runs are faster). No rendering faults seen (frame 1800's purple glyphs are the HUD's intro after a menu).
- First attempts: `desktop-launch-20261028/wrong-renderer-20261103/` holds two runs made WITHOUT `SNX_NVN_FRONTEND=2 SNX_NVN_RELEASE_ONLY=1` (old renderer, the replay went astray, as the user saw) and one of a stale build. Before `own_patch` and in-copy buffer copies, about 230 landings at once a second cost 40-70 ms of waiting a second (half the game writing GPU blocks, half buffer copies).

## Next

The user plays the verse exit on c7 with `Launch-Bayonetta3-PassTime.cmd`: `614fd3a3` (12-16 ms before), `66b9a528`, `ea51dd0e` and the full-screen pass `1e842019` (about 11 ms, reads half its textures from the pools: not changed by c7).

## The user's verse exit on c7 (`play-c7/`, 2026-11-03, log `629d69bb...`)

Through `Launch-Bayonetta3-PassTime.cmd`, about 170 s, against the c6 run of the same spot (`soft-ftz-20261103/play-c6-ftz-on/`). The user: "it was better better".

| | c6 | c7 |
|---|---|---|
| compute time per sampled frame | max 83.0 ms, mean 2.81 ms | **max 0.6 ms, mean 0.08 ms** |
| the 613xN chain (`614fd3a3`, `66b9a528`, `ea51dd0e`) | up to 24 ms a frame; `614fd3a3` alone max 16.0, mean 7.81 ms | below the largest jobs (`nvn-top`) every time |
| full-screen pass `1e842019` | max 11.4, mean 7.96 ms | max 8.9, mean 6.52 ms |
| sampled frames with GPU over 20 ms | 43 of 368, max 103.8 ms | **0 of 319, max 19.8 ms** |
| median frame, every 10 s report | 17 to 23 ms | 17 ms in all 15 |
| worst 1% low after the first load | 13.5 FPS | 26.3 FPS (at an ASTC load of 384 Mpx) |

c7's own line: landings at once 0-10 per 10 s (0-2.5 ms waiting), no pool writes over GPU blocks, the game's writes patched in about 150 times a second. **The drop after a verse exit is gone.** The pass `1e842019` (textures half from the pools) and texture-load hitches (ASTC) are what's left.
