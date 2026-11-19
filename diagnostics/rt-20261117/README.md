# The replay on its own thread (2026-11-17)

RTX 3080, Ryzen 7 5700X3D, driver 621.24, fight route (`route-remnant9.txt`) in the isolated copy, warm caches,
`resolution=1.6`, frame limit 60. Game code `f13c034c` (compiled with the 0.3.0 runtime, `rt-base`) in every run.

## What was slow

The game submits one command list a frame from its draw thread (`MainDraw`, guest core 2), in
`nvnQueueSubmitCommands`. Our hook replayed the whole list into Vulkan there: about 8 ms of a heavy fight, half of
that thread's frame (before the window 5-6 ms, the window 8 ms, after it 3 ms). The replay held `g_lk`
(`hle/nvn/cmdlist.c`) all that time, and every recorded command (`cmd_record`, about 3,400 a frame) takes `g_lk` too,
so the game's other threads recording lists waited for the replay to end.

## The change

- **Recording has its own lock** (`g_rec`): lists, their commands and the recording counters. `g_lk` keeps the
  resource tables, the draw state and the replay. Order: `g_lk` before `g_rec`.
- **A render thread** (`rt_thread`) runs, in the order the game called them: a submit's lists (a window), a present's
  bookkeeping (`present_run`), a texture's image dropped. The game's thread matches the submitted lists, decodes the
  state objects they bind (`check_list_body(c, l, 1)`: the getters run guest code, so they need the game's thread;
  the replay then only looks them up), queues the window and returns. A submit waits for the window before it, so
  the replay is at most a frame behind.
- **Lists in flight** (`CmdList.inflight`): a submitted list isn't recorded over until its replay ran.
- **The GPU thread waits for the replay** (`gpu.c`): each GPFIFO submission and call takes the count of render jobs
  queued when it came (`g_gpu_host_ticket`) and waits for them before running (`g_gpu_host_wait`), holding no lock.
  The releases the game waits on (fences, syncpoints) and vi's present then come after the replayed work, as on the
  console. Waiting in `release()` instead could deadlock: `vk_lock` is recursive and may be held there.
- `SNX_NVN_RENDER_THREAD=0`: the replay on the game's thread, as before (the lock split stays).
- New log line every 600 presents: `nvn-cmdlist: render thread on, ...` (submits waiting for the frame before, the
  game's threads waiting for `g_lk`, state objects found undecoded).

The `pace` line's windows are now measured on the render thread. "Held by the pacer" is still the game's thread.

## Measured

The heavy fight is the route's last two 10 s windows (about 113-123 s): 650 and 517 draws a frame in every run, so
the same scene. "Spare" is the pacer's hold on the game's draw thread: what is left of 16.7 ms once its work is
done. Fewer cores: the game's process limited to the first 4, 3 or 2 cores of the 5700X3D (affinity 0xFF, 0x3F, 0xF,
`pin.ps1` in the scratchpad), a stand-in for a smaller CPU (fewer cores, but each as fast).

| Cores | 0.3.0 (`73d8c4c7`): spare | New, render thread on: spare |
|---|---|---|
| 8 | 0.7-1.3, 1.3-2.0 (`stab030b`, `base030`) | 8.3-8.3, 10.2-10.1, 8.4-8.4 (`rt1`, `rt2` x2) |
| 4 | 0.6-1.0 (`base4c`) | 9.6-9.5 (`rt4c`) |
| 3 | 7.2-8.0 (`base3c`) | 9.2-8.7 (`rt3c`) |
| 2 | 0.6-1.2 (`base2c`) | 8.3-7.7 (`rt2c`) |

- 0.3.0 is bimodal: 4 runs of 5 have 1 ms or so to spare, one (`base3c`) 7-8 ms, with its replay at 4.3-4.5 ms
  instead of 8.1-8.5 and almost no `vk_lock` waits. The new build with the render thread off (`rtoff`, 2 runs) had
  6.1-6.8. Why 0.3.0 lands in one mode or the other wasn't looked into. The render thread had 7.7-10.2 ms to spare in
  all 6 runs, on 2 cores too: the extra thread doesn't cost the game time there.

Whole route (bench):

| Build | Runs | Over 33 ms | Hitches >= 50 ms | 1% low, median window | 1% low, worst window |
|---|---|---|---|---|---|
| 0.3.0, 8 cores | 1 (+2 `stab030b`) | 14 (10-13) | 3 | 47.6 | 8.9 (11.9-12.0) |
| 0.3.0, 4 / 3 / 2 cores | 1 each | 11 / 13 / 12 | 3 / 3 / 4 | 47.6 | 10.9 / 10.4 / 10.8 |
| New, off, 8 cores | 2 | 12, 12 | 3, 3 | 47.6, 50.0 | 11.5, 11.4 |
| New, on, 8 cores | 3 | 13, 12, 10 | 3, 3, 3 | 47.6, 50.0, 47.6 | 18.9, 23.3, 23.3 |
| New, on, 4 / 3 / 2 cores | 1 each | 10 / 12 / 11 | 3 / 3 / 4 | 50.0 / 50.0 / 47.6 | 22.7 / 21.7 / 15.2 |

- Every run exited 0, saves unchanged, no device lost, Vulkan error or svcBreak. 0 state objects found undecoded,
  submits waited for the frame before 0-2 times a 10 s window (0.02 ms).
- The three hitches are the same loading-transition frames in every run (949 black, about 1085 spinner, 4623 still),
  66-78 ms raw either way. The bench's "longest" reads 77 ms with the render thread against 62-64 without only
  because the pacer's hold on the game's thread no longer falls in the same frame line, and the bench subtracts it.
- The last screenshot is the same scene, drawn the same.

## Not checked

- Hand play and AMD.
- A CPU with slower cores: fewer cores of the 5700X3D (its large cache included) is only part of that.
- Why 0.3.0 is sometimes fast in the heavy fight (above).
- Above 60 FPS (not the goal).
- The runs used runtime `0b969bfb`; the source was then only reformatted (two lines).
