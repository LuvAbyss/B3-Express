# Chapter 12, whole chapter, this port against Witch Time (2026-11-02)

Played by hand by the user on the desktop, one run each, GPU recorded by nvidia-smi once a second. f5 (`8efcbbf3...`) 11:06-11:37, Witch Time v0.4.5 13:57-14:30. 2560x1440, 3D at scale 1.6 on both. The two runs aren't the same fight, and Witch Time runs cutscenes at 60, ours at 30.

Files: `ours/`, `witch-time/`, `vram-*.csv` (copies of the user's records), `analyze.py`, `gpu-per-frame.py`, `clean.py` (run from this folder, the last two read `windows.json` from the first).

Our figures use only the 97 windows of 600 frames wholly at 60 FPS mode: a window that crosses into a cutscene reads as slow. Our log has counts over 33.4 ms, not percentiles. `before the first window` includes the pacer's hold.

| | Witch Time | this port |
|---|---|---|
| gameplay FPS | 59.1 | 55.6 |
| combat at 1,300+ draws | 58.9 | 48.8 |
| time under 50 FPS | 0.3% | 20% |
| windows where 1% of frames passed 33.4 ms | 1 of 350 | 8 of 97 |
| frames of 100 ms or more | in 4 windows of 350 | 20 frames |
| longest frame in play | 132 ms | 2,428 ms, then 939, 768 |
| pipeline waits in play | 79 holds, 1.1 s, longest 435 ms | 3,483, 5.6 s, longest 2,386 ms |
| loading screens held for pipelines | 337 frames, 115 s, longest 9.6 s | none |
| peak video memory, nvidia-smi | 5,834 MiB | 4,881 MiB |

Same counter, GPU busy a frame (utilization x frame time). Our figure agrees with our own timer within 0.5 ms.

| draws | Witch Time | this port |
|---|---|---|
| under 500 | 7.9 ms | 8.0 |
| 500-900 | 9.2 | 10.0 |
| 900-1,100 | 9.2 | 13.3 |
| 1,100-1,300 | 10.2 | 12.7 |
| 1,300+ | 10.3 | 16.4 |

Witch Time's windows are timed from its audio lines, about 10 s, so its column is rougher.

Game side, a frame: Witch Time frame time less its pacer, ours `before` less the hold.

| draws | Witch Time | this port |
|---|---|---|
| under 500 | 2.0 ms | 3.3 |
| 500-900 | 3.2 | 3.6 |
| 1,300+ | 5.7 | 6.9 |

## Findings

1. **Loading screens.** Witch Time holds a loading screen until its workers have finished every queued stage and pipeline, 115 s here. We don't, so the same work lands in play. Likely the main reason its transitions feel seamless. The 2,428 ms frame is a pipeline wait again, plus 939 and 768 ms.
2. **Heavy drops in low draw scenes are GPU time.** Windows at t186, 202, 567, 582, 616: 35-47 FPS at 380-700 draws, GPU 16-25 ms, the game waiting 6-11 ms a frame on the GPU, 9-23% of frames over 33.4 ms. Their worst stretches run frames of about 50 ms, up to 278 ms.
3. **ASTC is a suspect in those, not proved.** Those stretches logged 15-20 Mpx of ASTC a frame at t570-610, 3.5 at t194-201, and only frames over 50 ms carry the figure. BC3 runs on the GPU in the frame. The scripted fight that showed no cost has little streaming.
4. **Combat is both.** 1,300+ draws: 6.9 game + 10.1 windows + 3.4 after = 20.4 ms in one thread, 48.8 FPS, GPU 15.9 ms. A render thread alone leaves the GPU on the edge of 16.7 ms.
5. **The game's code isn't the gap.** Corrects any reading of `before` as game cost: without the pacer it's close to Witch Time.
6. **Corrected GPU estimate.** Witch Time's GPU time is about 10 ms in combat, not the 7 estimated from one reading. Ours is 16.4, so the gap is real but grows only with draws: equal at low draws. System memory reads are still the first suspect for that, 65-69% of reads come from copies, and Witch Time logs 0 MB in system memory. Not proved.
7. **Video memory on one counter.** About 2.9 GB against 3.9 GB after the 1,950 MiB idle desktop. Witch Time's record starts in play, so its idle figure is taken from ours.

The user's 1% low of 23-30 fits the worst windows. On Witch Time, 1% low is 52.6 at the median over 5 s windows, best 58.6, 12 of 350 windows under 40, so 59 is its best moments and 40+ in heavy fights holds nearly always.

## Spikes against cutscene changes (`spikes-vs-cutscenes.py`)

The user asked whether the spikes are the change to 30 FPS cutscenes. Mostly not. Of 259 frames of 50 ms or more, 21 are within 2 s of a change between 60 and 30 mode and 31 within 5 s. Of the 20 frames of 100 ms or more, 3 and 5. The t181-203 stretch is over 400 s from any change. A cutscene's own 33 ms frames aren't in these counts. Two of the larger spikes do sit on a change: 939 ms at t1588 (884 ms a pipeline wait) and 253 ms at t1831. So do the 118 ms at t766 and the 40-odd windows left out above.

ASTC bursts: frames of 373-460 Mpx at t36.8, 570.8, 909.0 and 1705.8 are each 100+ ms. At t570.8 and t909.0 the next frame also lost 180-255 ms before its first window. This strengthens the ASTC suspect for hitches. It doesn't explain the steady GPU drop at t194-201.

## Next

1. Hold loading screens for queued preparation and pipelines.
2. Log real percentiles, ASTC per window, and the key of waits over 100 ms.
3. A/B ASTC (`SNX_VK_ASTC_BC=0`) where it streams. Needs a chapter 12 route or the user's hands.
4. Render thread measure.
5. GPU time in combat: VRAM step 3 comes back as the first lead.
