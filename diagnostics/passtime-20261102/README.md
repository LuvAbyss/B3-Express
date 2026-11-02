# GPU time by pass (2026-11-02)

Build c2 (`Bayo3_120-c2.exe`, `5389fc20...`) adds `SNX_NVN_PASSTIME=n`: every n frames one `nvn-pass:` line with GPU time by pass and compute dispatch. `nvn-live: frame times` gives a 1% low and ASTC for each report. Start it with `Launch-Bayonetta3-PassTime.cmd`. Originals: `pre-edit/`, edits: `edit-1.py`.

## The game has dynamic resolution

Scripted fight, isolated, `resolution=1.6`, 396 timed frames, `resolution.py`.

| largest pass | at the game's scale | frames | GPU a frame |
|---|---|---|---|
| 2560x1434 and 1440 | 1600x896 | 70 | 11.4 ms, menus and light scenes included |
| 2304x1306 | 1440x816 | 265 | 10.2 ms |
| 2176x1229 | 1360x768 | 15 | 13.3 ms |
| 2048x1152 | 1280x720 | 6 | 8.9 ms |
| 1638x922 | 1024x576 | 40 | 10.1 ms |

In heavy combat the main pass moves between about 1638x922 and 2304x1306 about every 2 s (t124-150), GPU about 10 against 16 ms at similar draws. That looks like a controller hunting. Seen in a 1-in-30 sample of one run, so its period is rough, and a sample's largest pass isn't always the scene's.

**Hypothesis, not proven:** after a load the game may start at its top size and step down, which would give a GPU peak that fades by itself. Pixels alone would be about 2.4x between the two ends. The c2 load test shows the size in every `nvn-pass:` line, so it settles this with no new build.

## What each build gives the game

- Witch Time's `OP_REPORT_VALUE` writes the value with `now_ns()` when its render thread replays the report (`nvn_gl.c:3569`), so its timestamps follow the CPU side of the replay. Its notes mention the step to 1440x816, and forcing the size down with test timestamps.
- Ours loses `ResetCounter` and `ReportCounter` in release-only mode (164 of each in 392 s, `cmd_unkept`), which is rare, so these are unlikely to be the controller's input. Which call the game's controller reads isn't known yet. Nobody here has found it in the game's code.

If the hypothesis holds, the next step is finding that input, and what ours reports against Witch Time, by reading the game's code.
