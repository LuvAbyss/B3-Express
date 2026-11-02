# Cutscenes at 60 FPS (2026-11-02)

The user asked how Witch Time runs cutscenes at 60. Its method was read from its source and docs, the same design was written as our own code, and it works on the scenes below. **Installed in `output/Bayo3_120` at the user's request, 2026-11-02 (`deployment-c1.json`, `deploy-c1.py`), with `cutscene_fps=60` in the INI. Rollback: copy the exe, DLL and INI from `preserved-installed-c1/` (f5 `8efcbbf3...`, module `782891b7...`). Not played yet: hash `Bayo3_120.log` against `preserved-installed-c1/Bayo3_120-before-install.log`.** The default stays 30.

Build: `Bayo3_120-c1.exe` (`0ee0d7e0...`), module `work/out/Bayo3_120/Bayo3_120_code.dll` (`85df1689...`). Originals of edited files: `pre-edit/`.

## How the game times these scenes

Addresses are offsets from `main`. All were checked against our v1.2.0 by `../chapter12-20261102/check-cutscene-sites.py`.

- The main loop (`+670b80`) asks each frame for a mode through `+d02de0`: 0 is 60, 1 is 30, 2 is 15. The title, menus and cutscenes ask for 1.
- The clock `+c1f600` keeps the frame's step at clock+0xe0. A cutscene has a fixed 1.0, and each frame runs two logic steps.
- The two steps are different kinds. The first recomputes the timeline. The second latches what was asked for: a skip, a branch, the next act.

Forcing mode 0 alone plays a cutscene at double speed: 46.6 s for 92.1 s in Witch Time's notes. Halving the step keeps the time but runs 120 steps a second, so anything counting steps runs fast. Dropping the second step keeps the count but latches nothing, so skips stick and a ride runs into its death cut.

## What is done

Four functions are replaced in `source/src/nx/hle/scene60.c`, as in Witch Time's `game_shim_eh.c`:

| function | what ours does |
|---|---|
| mode set `+d02de0` | a 1 becomes 0 |
| clock `+c1f600` | the step follows the measured frame, what a late frame owes is paid gently, a ride's step stays exact |
| step kind `+67037c` | the first kind's flag is cleared every other frame, so the kinds take turns |
| step end `+b20a20` | clears the loop's request for a second step, except while the scene player seeks |

The clock also restores flag 28 if a movie scene loses it.

These are direct `bl` calls, which compile straight to the function and skip `g_disp`. So the recompiler gained a list: `<name>.tablecalls` (`launcher/code-tablecalls.txt`), hex addresses whose direct calls go through the table. `SNX_ABI` is unchanged. With an older module the handlers never run and the game stays at 30. `scene60_init` reads the game's own call sites first and does nothing on another game or version. Only the LLVM backend reads the list.

## Measured

Isolated, 2560x1440, saves unchanged, exit 0.

| scene | at 30 | at 60 |
|---|---|---|
| title loop, in-engine, 120-210 draws | 39.95, 39.98, 39.96 s, 1,200 frames each | 40.05, 40.06, 40.04 s, 2,364, 2,096, 2,236 frames |
| movie on the Jeanne route, 3 draws | 79.66 s, 2,388 frames | 79.87 s, 4,751 frames |
| gameplay between them, same route | 41.33 s | 41.28 s |

The title loop is the real test: time held at 52-59 FPS. The movie proves little, since a movie keeps its own clock. The title dips below 60 on GPU time, 13-18 ms, with the game waiting up to 12 ms on the GPU. Its picture was looked at in one shot and is right. In the scripted runs one frame in 600 takes about 115 ms: that is the screenshot, and the user's chapter 12 play has none (0 of 142 reports, `../chapter12-20261102/every-600.py`).

## The user's play of chapter 12 on c1 (2026-11-02)

16:05-16:32, 1,648 s, no crash. Log and GPU record in `play-c1/`. Scripts `compare-play.py`, `gpu-play.py`, `why-slower.py`. Hand-played, and c1's run came second, so its pipelines were already cached: stage compiles 5.9 s to 0.4 s. **So the fall in pipeline waits, 5.6 s to 0.5 s, and the end of the 2.4 s freeze aren't c1's doing.** A chapter played for the first time will still freeze until loading screens are held.

| | f5 | c1 |
|---|---|---|
| in-engine scenes, FPS | 29.9 | **54.8**, lowest 43.3 |
| gameplay FPS | 55.6 | 54.3 |
| gameplay without the heavy stretches | 56.7, 0.47% over 33.4 ms | 56.3, 0.25% |
| heavy stretches (GPU 16+ ms under 900 draws) | 5 windows, 73 s, 40.9 FPS | 8 windows, 128 s, 37.6 FPS |
| combat at 1,300+ draws | 48.8, GPU 16.4 ms | 48.8, GPU 16.4 ms |
| longest frame | 2,428 ms | 366 ms, see above |
| peak memory, whole card | 4,881 MiB | 4,630 MiB |
| scenes: GPU use, power | 42%, 116 W | 73%, 156 W |
| hottest | 58.3 C | 59.3 C |

**c1 didn't slow gameplay.** Gameplay looks slower only because this run spent longer in the heavy stretches. Those hold 89% of c1's frames over 33.4 ms (63% on f5): GPU 19-29 ms at 300-860 draws, the game waiting about 11 ms a frame on it, frames of 50-67 ms for up to 13 s (t348-361, t956-964). **Corrected:** ASTC is the lead for them, not ruled out. 4.4 Mpx a frame over 132 frames is about 580 Mpx, and ASTC textures in memory jump 250 to 713 MiB (t363) and 281 to 754 MiB (t968). The user reports these as the returns from the two verse maps, 15-16 FPS for 5-7 s. Our log carries ASTC only on frames over 50 ms, so the share isn't measured, and the windows just before the jump (t342, t950) already read GPU 19-21 ms at low draws, unexplained. Witch Time avoids the work: `bc7_textures=on` serves 10,026 textures in its chapter 12 from a pack made at install (about 16 GB, about 12 minutes), so nothing is decoded in play. See `map-return.py`. These are the next target.

Scenes against f5: 3 matched within 0.2 s, 40.8 to 41.0 s among them. 54.6 to 57.7 s and 29.3 to 31.2 s ran longer, cause unknown. Others were far shorter (197.8 to 94.1 s, 161.8 to 64.6 s, 35.5 to 3.5 s): the user confirms skipping a few, and play went on after them. The user reports the Baal Zebul cutscene falling to 28-35 FPS for 2-4 s with slight audio desync. By the step rule, a steady 30 FPS leaves the scene about 150 ms behind (4.5 30 Hz frames owed), which fits; not measured. Paying a late scene off faster would shrink that.

## Six map loads on c1 (2026-11-02, `play-c1-loads/`, `loads.py`)

The user loaded in and out of a verse three times each, 251 s, no GPU record. **This corrects the ASTC lead above.** ASTC arrives in one frame, not spread out: 546, 499 and 513 Mpx in single frames of up to 348 ms. The two long stretches then ran 8.2 s at 15.9 FPS and 9.2 s at 18.4 FPS at about 1,000 draws with only about 90 Mpx left in all, GPU 32 ms a window against 13 ms for 1,000 draws elsewhere, game waiting about 12 ms a frame on it. Pipeline waits 0.13 s, longest 31 ms, so not those either. A texture pack would fix the spikes, not the 15 FPS. Nothing in our log explains the GPU time: staging, copies and dispatches read as normal. Needs GPU time by pass.

**Corrected by the user:** verses aren't slow throughout. They didn't play them, they left by hand, and FPS comes back to 60 after a while in a verse. The drop follows each load in and each exit and fades: GPU 32, 22, 21, 17, 13.6 ms in windows of 12-20 s (340-430 draws at 39-47 FPS on the way down), against 7 ms at 590 draws before the first load. Our runs never stayed long enough to show the full return.

Ruled out, `kept.py`, `decay.py`: blocks held in system memory by the hot counter, 413-414 in every window. Reads from copies do fall, 72% to 62-66%, but only 50% more reads reach system memory, and over the whole chapter logs the link is weak (0.25, 0.55). Not a cause so far. The draw log (`SNX_NVN_DRAWLOG`) has occlusion queries, not time. Timing exists only per window (`g_ts_pool`), so a timing build needs timestamps at each pass and dispatch.

## Witch Time, the same six loads (`play-witch-time-loads/`, `wt-loads.py`)

The user's hand test with a minute's pause at each stop, 530 s, with a GPU record. **Witch Time has the same heavy phase, so the phase is the game's.** After each load, 870-1,070 draws at 44.7-50 FPS for about 45-90 s, GPU 14-17 ms a frame, then 60 FPS, 486 draws, GPU 7 ms. Before a load both builds match: GPU 7 ms.

| | Witch Time | this port (c1) |
|---|---|---|
| lowest window | 44.7 FPS (5 s) | 27.9 FPS (21 s) |
| the worst seconds after a load | no drop below about 45 FPS | 15.9-18.4 FPS for 8-9 s |
| lowest 1% low | 30.8 | not in our log, frames of 50-67 ms |
| longest frame | 126 ms | 348 ms |
| windows with p99 over 33.4 ms | 0 of 85 | most after a load |
| GPU a frame at about 1,000 draws after a load | 14-16 ms, nvidia-smi | 32 ms window average, our timer, no record for this run |
| holds at these loads | **none** | none |

**Corrects:** holding loading screens isn't why its map changes are smooth: with warm caches it held nothing at these six loads. It's still the fix for first-time freezes. Textures arrive the same way: about 1,400 MB in one window, longest frame 123-126 ms against our 300+ ms.

Ours has a part Witch Time doesn't have: 8-9 s of frames at about 60 ms, all over 50 ms. The window average implies about 65 ms of GPU a frame in them, an estimate. That part is ours to find. The two runs weren't the same in the middle: our 1,000 draw stretch was 8-9 s, then about 400 draws, while Witch Time held 870-1,070 draws for about a minute. Hand tests, so positions may differ.

## The same test on c1 with waits and a GPU record (`play-c1-loads2/`, `loads2.py`)

396 s, three stops of about a minute. The user stood still, so windows repeat almost exactly.

| phase | Witch Time | this port (c1) |
|---|---|---|
| settled, about 480 draws | 60 FPS, GPU 7 ms | 60 FPS, GPU 5.8 ms |
| long phase, about 850 draws, standing still | 45-46 FPS, GPU 16.7 ms | 41-44 FPS, GPU 19.7-20.6 ms |
| first seconds of it, about 1,050 draws | about 46-50 FPS, GPU 14-16 ms | **16.8 FPS for 8-12 s, 92-94% use, about 55 ms** |
| after leaving it, about 450 draws | 55-59 FPS, GPU about 14 ms | 39-47 FPS for 20-30 s, GPU 15-22 ms |
| peak memory, whole card | 5,246 MiB | 4,103 MiB |

**So the long phase nearly matches, and the gap is two short ones**, the 8-12 s above all. Both builds settle at 5.8-7 ms at the same draws, so those phases hold something that ends by itself.

Build c2 (`Bayo3_120-c2.exe`, `5389fc20...`, same module as c1) adds `SNX_NVN_PASSTIME=n`, `nvn-live: frame times` with a 1% low and ASTC per report, and the scene catch-up at a half instead of an eighth. **Installed in `output/Bayo3_120` at the user's request (`deployment-c2.json`), rollback to c1 in `preserved-installed-c2/`. `Launch-Bayonetta3-PassTime.cmd` turns the timing on.** Isolated: scripted fight and title loop exit 0, pass lines sum near the window timer (11.2-11.6 against 12.6 ms). Title loop: 10.2 of 14.4 ms in 15 full-size passes of 68 draws.

## Not tested

- Sound and lip sync in the real cutscenes the user played, until they say.
- Skipping a cutscene, chained scenes, and rides (chapter 3 spider, Gomorrah, Malphas).
- Sound and lip sync by ear.
- The launcher's compile with the list: code added and unit tests pass, no real Install run.
- Witch Time's own left-overs apply: the rides' plain frames run 60 steps a second, and frames to seconds `+cb4f54` reads mode 0 in a forced scene.

## Left

The launcher has no setting for it yet: set `cutscene_fps=60` in the INI. Add one after the user's play. A cost to expect: cutscenes now have to hold 60, so a heavy one will show the GPU limit that 30 hid.
