# Prompts that follow the button map (issue 14, 2026-11-17)

Issue: https://github.com/LuvAbyss/B3-Express/issues/14. Code: `source/src/nx/gpu/vk/prompts.c` (`picture`,
`vk_prompts_paint`, `set_sig`), `prompts.h`, `source/src/nx/runtime/platform/backend/gamepad.c` (`gamepad_prompts`
returns the pad's map), `gamepad.h`. Not committed, not installed.

## B2recomp

`E:\download\B2recomp`: `config/defaults/controllers.ini` `[general] layout = auto|positional|labels` maps the face
buttons by position or by printed letter. `BUILDING.md` "Button prompts": `button_prompts = auto|switch|playstation|xbox`
picks a RomFS prompt mod the player installs (Timo654's DS4/XBOX prompts). It ships no pictures. Ours paints its own.

## Build

`source/build/desktop-isolated/scarletnx.exe` (deploy off), with the uncommitted render-thread changes in the tree.
The isolated exe and `.ini` were copied aside, swapped for each run and put back (`prompt_run.py` in the session
scratchpad).

## Runs

`desktop-launch-20261028/run_candidate.py`, test save, script `600f:1,900f:1,1200f:2,1500f:2,1800f:2`, 2100 frames,
a shot every 150. Labels `prompts14-*`. The footers are in `footers.png` (main menu, `screen_0005`; chapter select,
`screen_0013`).

| Label | `button_prompts` | `gamepad_p1_map` | Log | Confirm, Back | Throw |
|---|---|---|---|---|---|
| `xpos` | 3 | | Xbox | B, A | B X |
| `xletter` | 3 | `south:A east:B west:X north:Y` | Xbox, A on the bottom button | A, B | A Y |
| `xrb` | 3 | `rb:B` | Xbox | B, RB | B X |
| `pspos` | 2 | | PlayStation | Circle, Cross | Circle Square |
| `swrb` | 1 | `rb:B` | Switch | A, R | A Y |
| `psletter` | 2 | `south:A east:B west:X north:Y` | PlayStation, A on the bottom button | Cross, Circle | Cross Triangle |
| `psrb` | 2 | `rb:B` | PlayStation | Circle, R1 | Circle Square |

The PlayStation runs (`pspos`, `psletter`, `psrb`) are in `footers-ps.png`, added 2026-11-18. Installed 2026-11-18
(`diagnostics/install-20261118/README.md`).

All exited 0, production saves unchanged. Roblox was running on the GPU at the time; nothing here is timing.
