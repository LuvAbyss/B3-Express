# Frame pacing toward a steady 16.7 ms (2026-11-05)

The user's recording `recordings/20261105-201500` (110 s: menu, street, practice, a verse and back) replays in step with
the session: every load lands within 0-24 frames of the user's (`range-rec201500-check`). The same route on Witch Time
(the user's own play, its BC7 pack on) has p99 17.1-17.5 ms in normal stretches against our 21-29 ms, while its longest
frames (225, 217 ms) are as long as ours: the gap is steadiness, not hitches, so the BC7 work was dropped.

## What the 20-33 ms frames are

Build p1 (`Bayo3_120-p1.exe`) = the barrier fix + the pacer's hold on each slow-frame line (`held by the pacer`).
Replay with `SNX_NVN_SLOW_MS=20` (`range-pace-p1`) and `=0` (`range-pace-all`, every frame):

- 6,902 frames after frame 160: 6,396 at 14-19 ms, 309 over 19 ms, 197 under 14 ms. Mean 16.94 ms.
- In the 20-33 ms frames the pacer still held a median 4.6 ms (mean 5.5), the game's work before the first window a
  median 2.9 ms: they are mostly not slow work.
- The frame after a 19-33 ms frame: median 14.4 ms, under 14 ms in 121 of 298; the pair together a median 35.2 ms.
- The monitor runs at 180 Hz and the swapchain is mailbox, so a present shows within about 5.6 ms.

Cause: the pacer holds the game at the frame's start (`present_pace`, vi's DequeueBuffer), and the frame is shown at
its present after a varying amount of game, renderer and GPU work. Release times are even, present times aren't: a
frame with more work after its release lands late and the next one early. The present itself (`vi_present`, through
`nv_gpu_call_after`) runs on the GPU thread, which a hold there would also stall.

## Plan

A present thread: the GPU thread hands it the finished frame, it waits for the frame's 16.67 ms slot and presents. A
frame late past its slot sets the schedule from its own time, so no short frame follows it. Measured on the recording
by the frame-time distribution and the long/short pairs.

## The present thread (2026-11-06)

Built in `gpu/vk/renderer.c` (`pt_hand`, `pt_main`, `pt_present`): `show()` still acquires, records and submits on the
GPU thread, and hands the present to a thread of its own, which sends it out at the slot (the present before plus the
period of `present_period_ns`, 0.25 ms early so the schedule drifts back toward the frames), or at once when the frame
comes later. A frame handed while the one before still waits sends that one first. `SNX_PRESENT_THREAD=0` presents
each frame as it comes, as before, still measured; `SNX_PRESENT_TIMES=file` writes each game frame's time on the
display, hold and lateness past its slot (us). The log has `vulkan: frames on the display` every 600 frames.

pt1 held the present under `vk_lock`, which the GPU thread holds while recording the next frame: 368 presents went out
over 1 ms past their slot. pt2 gives the queues and the swapchain a lock of their own (`vk_queue_lock`, `device.c`),
taken by every submit, acquire, present, device wait and swapchain change, after `vk_lock` and never before it.

The recording replayed with each (`range-pt1-on`, `range-pt2-on`, `range-pt2-off`), frames on the display after frame
160 (`times-*.txt`, scratch `times.py`):

| | off (as 0.1.6) | pt1 | pt2 |
|---|---|---|---|
| frames | 6,768 | 6,691 | 6,757 |
| under 14 ms | 207 | 50 | 13 |
| over 19 ms | 326 | 237 | 177 |
| a frame over 19 ms followed by one under 14 | 130 | 17 | 4 |
| p99 | 24.2 ms | 24.0 ms | 23.3 ms |
| presented over 1 ms past the slot | 3 | 368 | 77 |

The 177 frames over 19 ms left are real overruns (ASTC decode, pipeline waits, the game's CPU), which the thread
can't hide. No crash or unshown frame in either run. Build pt2 `4aa1d57c`, NOT installed, uncommitted. `range-pt2-*`
note: about 144 of each 600 frames count as "sent before their slot", the high-resolution timer waking a little early
or the next frame arriving first; neither shows as a short frame.
