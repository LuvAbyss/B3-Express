# Draw tracking measure and mute on focus loss (2026-10-31, night)

Closes the last measure of phase 5. Build `../nvn-phase5-20261030/Bayo3_120-phase5i.exe` (`4fd28e66...`), **not installed**: phase5h2, plus the
measure (off unless `SNX_NVN_DRAWLOG` is set) and `mute_unfocused`. Source before the edits: `*.before` here.

## The measure

`SNX_NVN_DRAWLOG=path` (`nvn_res.c`, `dl_*`), every `SNX_NVN_DRAWLOG_EVERY` (300) frames a pair of consecutive frames: one line for each draw, its occlusion query,
hashes of the bytes its vertex stages read (`cbuf_max` only, plus UpdateUniformBuffer payloads, which guest memory doesn't hold yet), and an indirect draw's arguments
after the GPU is done. `batch.sh` runs it, `look.py`, `pairs.py`, `classes.py`, `camera.py` read it. Rows hold addresses and hashes, no game files, but keep them local.

Trust: full-screen passes count exactly their target (1,175,040 at 1440x816). No multi-draw with zero sub-draws passed a sample, in 23,251 of them on the four routes.

## Results

Depth EQUAL forward pass, which counts each surface once. It covers 0.88-0.94 of the scene. Scale 1, O1 DLL, pairs of frames.

| % of visible pixels | fight | street | Jeanne | Thule |
| --- | ---: | ---: | ---: | ---: |
| through multi-draws | 98.5 | 98.2 | 96.6 | 98.1 |
| tessellated | 0 | 0 | 0 | 0 |
| one matrix, unchanged | 42.2 | 73.1 | 68.6 | 74.7 |
| one matrix, moved or vertices changed | 2.3 | 0.3 | 0.4 | 0.4 |
| palette, changed | 36.6 | 6.7 | 0.6 | 16.9 |
| palette, unchanged in the pair | 18.9 | 20.0 | 30.4 | 8.1 |
| visible sub-draws also found in the pre-pass | 98.5% | 98.2% | 86.0% | 98.9% |

- **The hardest known problem is smaller than it looked.** Almost all the picture goes through MultiDrawElementsIndirectCount, but these aren't batches: of 52,553 calls `max_draws` is 1 in 52,411 (99.7%), the most is 6, each call takes one 20-byte entry of a table, 23,251 have no sub-draw, 122 have more than 1, and no call's arguments were among the ranges this frame's compute wrote. The count works as a yes or no.
- **The same geometry is drawn directly in the depth pre-pass, just before.** Writing object vectors there needs no multi-draw tracking at all, and the Bloodborne method's direct draws apply. Tracking the multi-draws themselves is possible too, the arguments being plain data. Not run: neither way is built.
- **Camera found.** Vertex uniform buffer 0, 0x80 bytes: a view matrix (orthonormal in 153 of 153 and 61 of 61 frames, moves frame to frame) and a projection, infinite far plane (`-1, -0.1`). That is B3's answer to the Bloodborne port's 864-byte search. Its jitter would go in here.
- **Object slot.** Vertex uniform buffer 11: 0x30 bytes is one 3x4 matrix, larger is a palette (up to 0xc00 seen), the same rule as selecting by bone palette.
- **Camera vectors alone** cover the one-matrix unchanged share, 42-75%. A palette object needs object vectors. "Unchanged" is for one pair of frames only: a palette at rest there can move later, so treat all palettes as moving. That is 25-56% of visible pixels, a real cost, and the fight is the worst case.

Not covered: blended draws (a further 0.16-0.23 of area in the fight), so particles and glow stay unsolved, as in the Bloodborne port. Scripted routes only, no Gomorrah or heavy Remnant. Jeanne's 86% is a cutscene with few draws, not looked into. The count buffer's writer wasn't logged, only the arguments'. `one matrix, vertices changed`, 2.3% of the fight, not looked into.

## mute_unfocused

Default 1, Display section of the INI and the menu, `sdl3.c`. Gain 0 while another window has the focus, the game's volume kept apart and restored. The game and its audio stream run on, because pausing the stream would disturb what the cutscene pacer fixed.

In runs: the log's `audio: muted` and `audio: unmuted` follow real focus changes on the desktop, 8 lines over the 4 routes, and FPS held 59 with the window unfocused. **Not judged by ear. `mute_unfocused=0` was not run.** A log can't show silence: SDL gain is the only link.

## phase5i, measure off

8 of 8 tests. All runs exit 0, isolated saves unchanged, the user's save `55b9613a...` and installed exe `763485b5...` unchanged.

| route | setting | FPS at the end | GPU ms | phase5h |
| --- | --- | ---: | ---: | --- |
| fight | 1.6, fsr 1.6 (`f16j`) | 58.8-59.2 | 7.9 | 59.2, 6.8 |
| street | 1.6, fsr 1.6 (`s16j`) | 59.4 | 12.7 | 59.4, 12.3 |
| fight | 1.0 (`f16i`) | 59.6 | 6.5 | 59.9, 6.2 |
| street | 1.0 (`s16i`) | 59.6 | 12.8 | |

One run each, so the GPU differences are within what a run varies by, not shown either way. `f16i` and `s16i` are mislabelled: a `sed` dropped a line continuation, so they ran at scale 1 and not 1.6. `batch-off.sh` is fixed.

The laptop package still holds phase5h2.
