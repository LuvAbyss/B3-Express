# Cheats in Atmosphère's format (issue 15, 2026-11-18)

Issue: https://github.com/LuvAbyss/B3-Express/issues/15 (its second comment asks for built-in cheats).
Format: Atmosphère's `docs/features/cheats.md`, read for the format (Atmosphère is GPL-2.0-only, nothing ported).
Not committed. Installed 2026-11-18 (`diagnostics/install-20261118b/`).

## The cheats players have

`HamletDuFromage/switch-cheats-db`, `cheats/01004A4010FEA000.json`, build `B47CC79B88B7091E` (ours: `main`'s build ID):
17 cheats, every one only `0x0` writes but Fast Motion (`0x8` held L3, then `0x0`). 11 write into `main`'s code
(Invincible, MP Max, 5x Income, Items, Skills, Attack 5x, Infinite Jump, Combo, Restore Code), 6 into its data (FPS
30/60, Death Count, Seeds, Orbs, Halos, Fast Motion). Kept in the session scratchpad only, not here.

## Runs

New runtime (`source/build/desktop-isolated/scarletnx.exe`), a mod `B3 cheats` holding that file, the game code
compiled by the launcher's own `build_code` (`B3_GAME_DIR` = the isolated copy), then the fight route
(`route-remnant9.txt`, 6000 frames, a shot every 300). `cheat_run.py` in the session scratchpad; the isolated copy's
exe, code, stamp and `.ini` were copied aside and put back (checked with `cmp`), the mod, `.patches`, `.codemods` and
`code_cache/` removed.

| Label | Cheats on | Log | Exit |
|---|---|---|---|
| `cheats15-on` | Invincible, Halos, Seeds, Orbs | `4 on, 0 master codes, 6 fixed writes`; `16 bytes patched (16 in code)`; `.patches` digest `e915074f97be01d1` | 0 |
| `cheats15-off` | none | `0 on`, digest `cbf29ce484222325` (none) | 0 |
| `cheats15-probe` | a probe: `FFF4I000 <addr>` logging Halos, Seeds, Orbs, Death Count's addresses | values 0, then 2 and 1 when the fight loads (41 s), 0 when the route restarts the verse (63 s), 1 and 2 again | 0 |

- The game accepted code compiled with Invincible in (no "built with other code mods"): the digest the recompiler
  recorded matched the runtime's.
- The cheat ran its code writes every frame and found the bytes already there (compiled in): no "writes the game code"
  warning in `cheats15-on`.
- The probe shows the data addresses hold live game values our build changes, read through the VM. They are small (0
  to 2), so they may be counts for the verse rather than the totals the menus show: not confirmed.
- Same screens in `cheats15-on` and `cheats15-off`; the HP bar is full in both at the last shot (the route takes no
  damage by then), so Invincible's effect isn't seen. Halos, Seeds and Orbs weren't found in the save after the run.
- Production saves unchanged in every run.
