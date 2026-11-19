# 120 FPS: what it would take (2026-11-17, a study, nothing built)

**Decision (2026-11-17): stay at smooth 60.** Nothing below is planned.

RTX 3080, Ryzen 7 5700X3D, driver 621.24, fight route in the isolated copy, `resolution=1.6`.

## Why the game can't simply run faster

- The game's clock (`main+0xc1f600`) measures each frame and keeps a step in 60 Hz frames, clamped to 1.0 .. 2.0
  (Witch Time's notes, the same engine as Bayonetta 2). Below 60 FPS gameplay keeps real time; above it the step
  stays 1.0, so at 120 FPS the game runs twice as fast.
- `fps_limit=0` (bench `uncap`, `desktop-launch-20261028/bench-uncap-fight1`): frames stay at a median 17 ms with the
  pacer holding 0 ms. The game itself waits for the display's 60 Hz vsync (vi), so the limit alone changes nothing.
- Menus and cutscenes run their own fixed step (`hle/scene60.c` already handles 60 there).

## How much time a frame has today

From the pace lines of `bench-stab030b-fight2` (0.3.0, frame limit 60): the work a frame = 16.7 ms less the pacer's hold.

| stretch of the route | held by the pacer | work a frame | top rate |
|---|---|---|---|
| walking, light fight | 7.5-10 ms | 7-9 ms | about 110-140 FPS |
| heavy fight (113-123 s) | 0.7-1.3 ms | 15.4-16 ms, of it the renderer's windows 7.8-8.5 ms | about 62-65 FPS |
| GPU time a frame | | 5.4-6.1 ms | about 160 FPS |

120 FPS needs 8.3 ms. The limit is the CPU: the game thread plus our renderer, which records on the game's thread.

## What Witch Time did (their `docs/STATUS.md` "Frame rates above 60", read 2026-11-17)

- `frame_rate = 61 .. 120`: the clock's floor 1.0 becomes 1/16, cutscenes run at that rate, the display paces at it.
  About a day. Fights 105-120 FPS on their PC at scale 1.0.
- Then a census: every load-add-store of a field in the lifted code instrumented (17,370 sites), runs at 60 and 120
  compared. 62 sites count the game's frames. Fixed so far: float timers, countdowns, the demon-form double-tap, 28
  stick-motion windows, press buffers, Witch Time's intro slow-down, hit-stops, rumble. Their finder missed forms
  three times. Still "not played by hand"; about 52 sites unread.
- Bayonetta 2's 120 FPS (B2recomp) needed about 60 counters and 48 smoothings, a week after the research.

## What it would need here

1. Clock floor and pacing: a hook on `main+0xc1f600` (beside `scene60.c`), vi's vsync event at the chosen rate, the
   pacer at that rate. About a day. The game then keeps real time but frame-counted windows get shorter.
2. A census in our recompiler (`source/src/common/cpu/recompiler/`, LLVM): instrument the same site form, compare
   runs, read each site, patch the counters (hooks or code patches). Witch Time's addresses are the same game build
   and can be checked first. Weeks.
3. Speed in fights: the renderer's 8 ms off the game thread (a render thread, as Witch Time replays), then the
   game's own CPU time. Without it heavy fights stay near 60-65.
