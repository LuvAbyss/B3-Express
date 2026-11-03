# Input recording and playback (2026-11-03)

The user asked for a way to record a play test once and replay it, so later builds can be tested without playing by hand.

## What it does

Build c5 (`Bayo3_120-c5.exe`) is c4 (`SNX_NVN_SPV_DUMP`) plus `input_record_replay` (`source/src/nx/runtime/platform/unattended.c`), called from `sdl3.c` where player 1's final state is set: after keyboard, pads, on-screen controls and the menu are merged.

- `SNX_INPUT_RECORD=file`: one line per change, `frame ms buttons lx ly rx ry`, flushed each line, so a window close or a crash keeps everything up to it.
- `SNX_INPUT_REPLAY=file`: the recording replaces player 1's input, each step held from its frame (the present count) until the next. `SNX_INPUT_REPLAY_BY=ms` plays by time instead. When the recording ends (60 frames or 1 s after its last step) the pads have player 1 again, and the log says so.
- `Launch-Bayonetta3-Record.cmd`: the launcher with recording on. Each run makes `recordings/<date-time>/` with `input.txt`, the save it started from and the `.ini`. `recordings/` isn't in the `.gitignore` allowlist, so it stays local.

Replay runs in the isolated copy (`diagnostics/desktop-launch-20261028/isolated/`) from the recording's own save, never in `output/`.

## Limits

- Only player 1, no motion or touch.
- Frames are counted from the first present. A load that takes a different number of frames than in the recording shifts everything after it. The game's own randomness can also make combat drift. Menus and walking routes should hold. Checked below.

## Check on the scripted fight route (isolated, c5 `167cc825...`, module `85df1689...`, controller ignored)

- Run A (`desktop-launch-20261028/range-rec-a/`): `route-remnant9.txt` as the script, recording on. 12,000 frames, 778 changes in `route-remnant9.input`.
- Run B (`range-rec-b/`): no script, only the recording. Played 778 steps to frame 11,844, gave the pads back at 11,924. Production saves unchanged.
- Side by side in `compare/` (A left, B right): frame 3000 the same menu, frame 11,400 the same spot in Giza with the same title card. Frame 6000, in the fight, the camera differs. Frame 9000, a transition, is a few frames apart. No screenshot is identical to the pixel, not even frame 0, because timing differs between runs.

**Verdict:** a recorded route replays to the same place, and the load points line up, which is what a load test needs. Fights drift, so per-frame numbers in combat compare as stretches, not frame by frame.

The isolated copy now holds c5 and the installed module. Its previous exe and module are `isolated-before.exe` (`23ed2b7f...`) and `isolated-before_code.dll` (`03dcc7fd...`) here.

## The user's first recording (`recordings/20261103-122329/`, 2026-11-03)

Recorded on c5 by the user: 214 changes, 157 s, a load into the street, a walk to a verse portal, Y held 1.5 s at frame 1943, the verse, Y at 130 s to leave it, then the drop at 134-166 s. Replayed in the isolated copy from its own save with `cutscene_fps=60`, `resolution=1.6`, `SNX_NVN_PASSTIME=30`, `SNX_NVN_SPV_DUMP` (`desktop-launch-20261028/range-replay-122329/`, production saves unchanged). `run_candidate.py` now takes `RUN_SAVE_FROM` (original in `run_candidate-before.py`).

- **The first 45 s match.** ASTC per report 47, 617, 639 Mpx against the user's 50, 626, 639, at the same times. During the walk both ran at about 38.6 FPS, and frame 1943 fell within 0.1 s of the same time.
- **The verse entry didn't happen.** At frame 1800 the replay stands at the portal with its Completion Info open, as the user did. The 1.5 s Y hold replayed, yet by frame 2400 Bayonetta has walked on along the street with no input recorded between frames 2012 and 2645, and no verse loaded. Cause not found. Next to check: whether the hold needs the panel's page or a prompt that only shows after some delay, and whether anything moves the character with no input.
- **The shaders were captured anyway:** `614fd3a3`, `66b9a528`, `ea51dd0e`, `1e842019`, `e9d06cf6` and `24e1f54e` are all in `passtime-20261102/spv-c5/`, by the same hashes as the c3 runs, so the SPIR-V is the same.
