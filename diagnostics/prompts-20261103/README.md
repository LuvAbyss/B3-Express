# Button prompts (2026-11-03)

PlayStation and Xbox prompts painted over the game's button atlas, ported from Witch Time v0.4.5 `runtime/prompts.c`
(GPL-3.0-or-later, credited in `source/docs/CREDITS.md`). Code: `source/src/nx/gpu/vk/prompts.c`, `prompt_art.h`,
the hook in `texture.c` (`tex_from_tic`, before the first hash) and `vk_tex_frame`; the choice in
`runtime/platform/backend/gamepad.c` (`button_prompts`, `gamepad_prompts`).

## Build

- `Bayo3_120-p1.exe`, sha256 `fd057876cb332306d0d93101be2ba4865a1b07e667b19d8def1a52962fa90011`: c7 plus prompts.
  NOT installed.

## Checks

- Offline: each set painted over an empty atlas and decoded with `gpu/texture/astc.c`: PlayStation (by position and by
  letter) and the Xbox shoulder, Plus and Minus cells draw right, with their backings. A hand-made block decodes to
  0 0 0 255.
- Isolated copy, scripted route (`desktop-launch-20261028/run_candidate.py`, fast renderer), 3000 frames each:
  - `prompts-ps` (`button_prompts=2`): log `button prompts: the game's atlas at 5a285c000` then `PlayStation`. Chapter
    Select footer: Create = Shop, Options = Menu, Circle = Start, Cross = Back. Combo list: L2. Crops in `ps/`.
  - `prompts-xbox` (3): View = Shop, Menu, B = Start, A = Back; LT in the combo list. Crops in `xbox/`.
  - `prompts-auto` (0, no pad connected): the atlas found, Switch kept, as designed.
- `isolated-before/` holds the isolated exe and ini as they were; the runner put them back after each run.

## Not tested

Auto with a real pad and switching pads, a change of the setting while the game runs (the repaint runs
each present, but the in-game overlay has no row for it yet), the keyboard set and the BC3 atlas (both not ported).
