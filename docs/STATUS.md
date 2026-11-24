# Bayonetta 3 PC port status

Bayonetta 3 v1.2.0 on Windows. ScarletNX recompiles the game's ARM64 code to a native DLL (`Bayo3_120_code.dll`), and
an NVN-level renderer on Vulkan (`source/src/nx/gpu/vk/nvn_res.c`) draws it. The launcher (`B3-Express.exe`) installs
from the player's own extracted dump, compiles the game code and prepares the shaders. Goal: a smooth 60 FPS and a
launcher anyone can use. Undergraduate graduate project, started 2026-10-19.

Test PC: Ryzen 7 5700X3D with an RTX 3080, except 2026-11-14 to 16 with an RX 6700 XT (sections say which). Numbers come from that PC at `resolution=1.6`
(2560x1440 from the game's 1600x900) unless noted. Unattended runs use the scripted fight route
(`diagnostics/nvn-phase5-20261030/route-remnant9.txt`) in the isolated copy (`diagnostics/desktop-launch-20261028/isolated`),
never the user's save. The notes behind each section are in the `diagnostics/*/README.md` it names.

## Now (2026-11-25)

| | |
|---|---|
| Latest release | 0.4.0 (2026-11-25), packaged from `ae50a1f` (runtime `e06153c1`); the user publishes it on GitHub |
| Installed in `output/` | runtime `792e79f2` from 2026-11-23 (before issue 17's fix). Rollback folders `diagnostics/install-2026112*` |
| Played | the AMD fixes on the RX 6700 XT (2026-11-16) and on an NVIDIA card (2026-11-17). Issue 17's fix by the reporter on Vega (2026-11-24) |
| Next | 90 FPS step 2 and the mouse camera by hand; the clouds on Vega (issue 17's last report); FSR 3.1: plan below, not started |

## Release 0.4.0 (2026-11-25)

- **Contents:** issue 17's Vega fix (pixel loops as structured loops, `1a88671`), 90 FPS step 2's frame counters and
  the chapter select's easing (`24afeb1`), the mouse turning the play camera and the sticks' key prompts (`502f311`),
  the launcher's driver check and download-mark fix (`ae50a1f`), and the in-game preparation screen.
- **Files:** runtime `e06153c1`; zip `e8a94dc6`, 84.2 MB, 1,016 files; notes `release/RELEASE-NOTES-0.4.0.md`.
- **Measured:** the Vega reporter's build 11 (same translator output byte for byte): lines gone, frames 22-26 ms vs 27
  on build 10. RTX 3080: shop route clean, fight at 90 the same as build 10 (sections below). `test_launcher.py` passes.
- **Not checked:** the new-player test on the unzipped package (`test-fresh.py --web`): another game held the GPU at
  77% while packaging. Hand play of step 2 and the mouse camera. An update's recompile.

## Keyboard prompts for the sticks and the D-pad (2026-11-23; installed, not committed)

Since 0.2.7 the keyboard set (family 4) drew only the ten button cells; every stick and D-pad prompt still showed the
Switch's picture.

- **Where the cells are.** `SNX_PROMPTS_DUMP=file` writes the game's prompt atlas (and `file.N`, each set painted) as
  rows of ASTC blocks; decoded with `texture/astc.c`, it shows 31 stick and D-pad cells beside the ten buttons: each
  stick with four arrows, left-right, up-down, a turn, each single direction, pressed, plain; the D-pad's cross, its
  up-down and left-right, and two sets of single arrows. The four widest (sticks with four or two arrows) reach x = 44,
  so they're painted 48 wide.
- **The change** (`gpu/vk/prompts.c` `STICKS`, `stick_picture`; `prompt_art.h` `arrow_cap`; `backend_kbm_mouse_camera`
  in `sdl3.c`): each cell shows its keys from `[Keyboard and mouse]`. Several directions share one key cap (WASD, AD,
  WS), the arrow keys are drawn as arrows, a press shows its key or mouse button, and the right stick shows the mouse
  while `mouse_camera` is on (its look keys, IJKL, when it's off). A cell whose keys can't fit (a long key name among
  several, arrows mixed with letters) keeps the Switch's picture. The set is painted again when any of those keys or
  `mouse_camera` changes.
- **Measured** (test copy, `button_prompts=4`): the painted atlas, decoded, shows every cell as above with the mouse
  camera on and off; the game logs "button prompts: keyboard and mouse"; the fight route runs into play. Formatting:
  `prompts.c` clean, `sdl3.c` no new notes.
- **Not seen:** the new cells inside the game's own prompts (the route shows none of them); by the user.

## The mouse turns the play camera itself (2026-11-23; installed, not committed)

The mouse used to be made into the right stick (`mouse_stick` in `sdl3.c`, as B2recomp does). The game puts a stick
through a dead zone (200 of 1000), a response curve, the menu's camera speed and the frame time, so the mouse felt
like a stick. That was our design, not ScarletNX's.

- **Where the stick goes.** Found with a page-guard watchpoint (a probe, not kept: the page of a value set to no
  access, each fault logged by game function, then a single step to let it through). The input module's pad object
  (slot `0xa4b14e8`), the engine's pad arrays (`main+0x95f208`), the play copy (slot `0xa4a1a00`), the player's input
  component (`player+0x1780`, `main+0x559c2c`), then the play camera's update (`main+0x63c148`). That adds a step a
  frame to the camera's pitch (`+0x6a0`, `fadd` at `main+0x63c830`, clamped after) and yaw (`+0x6a4`, `main+0x63c840`).
  Another stick camera (`main+0x625514`, a turn speed that eases out) doesn't run in play.
- **The change.** `hle/mouse_look.c` hooks the two adds (`launcher/code-insnhooks.txt`, 2 lines) and adds the mouse's
  motion since the last update, 0.0015 radians a count at `mouse_sensitivity` 100 (360 degrees in about 4,200 counts).
  The game's gates (x21, x20) still decide whether the camera turns. A controller's stick still adds its own step.
  `backend_mouse_look` in `sdl3.c` hands the motion over, and while the hook took some in the last 100 ms, the poll
  stops making the mouse into the right stick. A camera without the hook keeps the stick as before.
- **Measured** (fight route, test copy `diagnostics/desktop-launch-20261028/isolated-cam/`, `SNX_MOUSE_LOOK_TEST=x,y`
  feeding counts each update, shots every 10 to 30 frames): x turns the camera right (3 counts an update for 120
  frames turned the scene about a third of its width, about 31 degrees expected). y tilts it down. No motion: the
  camera as before. No crash in 9 runs. `launcher/test_launcher.py` and `snx-test-scene60-counters` pass.
- **Not tested:** a real mouse (only the user can move it): how it feels, the sensitivity's scale, the hand-over
  between this camera and others (lock-on, the demons, Viola, Jeanne's side-scrolling chapters), and 90 FPS.

## 90 FPS: the chapter select's ring and camera ease in real time (2026-11-23; installed, not committed)

Above 60 the chapter select's pointer ran ahead: two smoothings, x += k (T - x) per update, closed in 1.5x as fast at
90. This is Witch Time's fix (GPL-3.0, `fps_ease`, `lifter/eh.rs` `FPS_EASE_SITES`, 2026-11-16), written from their
description, and each address was checked in our main.

- **The change** (`hle/scene60.c` `ease_hook`, `ease_rate`; 3 lines in `launcher/code-insnhooks.txt`, 106 now): before
  each smoothing, k becomes 1 - (1 - k)^step, where step is this update's 60 Hz frames (2/3 at 90). That closes in as
  far in a 60 Hz frame's time. The hooks are:
  - the ring drawn easing to the aim (`main+0x91f1c8`, `fmla v0.4s, v3.4s, v1.s[0]`, k in s1, 0.05 to 0.3);
  - the camera manager's easing to its goal (`main+0x626404`, right after its rate is loaded into s0). It serves every
    camera, so it is eased only in an update that the chapter select's update (`main+0x91dba8`, also hooked) ran in,
    or the one after.

  The hooks are set only above 60, next to the 67 counters, after checking all three instruction words.
- **Measured** (test copy `isolated-cam/`, fight route through the chapter select, a temporary probe that timed each
  hop's glide to 10 % of its distance, now removed):
  - **At 60:** the three hops took 267, 251 and 250 ms.
  - **At 90, stock:** 178, 167 and 167 ms.
  - **At 90, fixed:** 267, 256 and 255 ms.

  At 90 with the fix, k goes from 0.15 to 0.103 in the chapter select, and the camera's easing stops when the map is
  left. The clean build (`ease90-c`) exits 0 with the saves unchanged and logs "3 of 3 smoothings". Its shots match
  the stock 90 run through the menus and the walk (mean difference 0.0 to 0.9). In the fight at 95 s the difference is
  12, about the same as between two fixed runs. `snx-test-scene60-counters`: 86 checks pass (the ease at 90 against
  60, stock at 1.0, the camera only near the map's update). `scene60.c` is clang-format clean.
- **The snap, later the same day (installed):** when the stick is let go, the aim snaps to a
  chapter. A float t (`+0x80`) starts at 0, the aim eases to the chapter by k = t, and t += 0.2 an update
  (`fadd s0, s0, s2` at `main+0x9202bc`) until it passes 1.0. That is 6 updates: 100 ms at 60, 67 ms at 90. It is
  now the 68th frame counter (`k_counters`, one line in `code-insnhooks.txt`, 107). The counter hook already takes
  this update's share of a float's amount, so at 90 t goes up by 0.133. Measured with a temporary probe (removed) on
  the fight route: at 90, `part 0.667` and t from 0 to 0.933, so 8 updates (89 ms), and the aim's ease per update
  isn't changed (the last k is 0.93, not 1.0, which leaves 0.08 % of the hop). The probe's timer was unreliable, so
  these times come from the updates counted, not from the clock. The clean build (`792e79f2`, run `snap90-c`): "68
  of 68 frame counters", exit 0, saves unchanged. The test passes 87 checks.
- **Not done:** not played by hand, not at 120. Other cameras' easing in play is unchanged (their rates aren't read).
  The game code must be compiled again for the new hook points: the launcher does it on Play (the fingerprint counts
  hook points; 41 s here). The snap was installed the same day (`diagnostics/install-20261123d/`): its hook point
  `89252bc` is back in `launcher/code-insnhooks.txt` (107 lines), runtime `792e79f2`, game code `7a90b4f2` compiled by
  `build_code` (23 s), `module_ready` true. The installed copy wasn't run (runs never use the player's save).

## 90 FPS, step 2: frame counters count play's time (2026-11-22; run in the isolated copy, not committed)

At 90 the game's update runs 90 times a second, and what it counts in updates ran 1.5x fast: input windows, press
buffers, hit-stops, Witch Time's slow-downs, rumble. Step 1 only had the demon form's double tap (one hook on its
caller).

- **Mechanism:** Witch Time's census (same game build) lists 64 stores that count 60 Hz frames
  (`tools/wtbuild/fps_counters.txt`, GPL-3.0, credited in the code). Their site finder (`lifter/fps.rs`), ported to a
  scratch script and run on our main, finds all 64 with the same field offsets: 67 add or subtract instructions (three
  stores have two). A hook point runs before each (`launcher/code-insnhooks.txt`). The main loop's clock call adds the
  measured step to a sum; an update that completes no whole 60 Hz frame (a third of them at 90, half at 120) is a skip.
  On a skip the hook raises the loaded register by the op's amount, so the field stays and the code after the op reads
  what a field one step larger would leave. Integer counters and floats counting 1.0 skip; a countdown at 0 takes the
  stock step; other floats (seconds, `-= 0.33`) take the update's fraction of their amount. Fixed-step scenes and
  frame_rate 60 count as made (no hooks are set at 60).
- **Checked in the code:** at every site where the loaded register is read after the op (the stick tracker's
  `subs w16, w1, #1; b.lt; mov w1, w16`, the rumble blend's `sub w12, w12, #2`), it reads the value the raised field
  leaves. The three ops that are branch targets (`main+0xdd0c8`, `0x55ae90`, `0xcc37e8`) load the same field into
  the same register on every path in.
- **Files:** `hle/scene60.c` (the table, `count_update`, `counter_adjust`; the double tap's own hook and its table call
  `8560058` are gone, it is one of the 67), `runtime/rtapi.c` (128 hook points, sorted, found by binary search: they
  run hundreds of times a frame), `launcher/code-insnhooks.txt`, `launcher/code-tablecalls.txt`,
  `launcher/bayonetta_launcher.py` (the code fingerprint counts the hook points, so a changed list compiles the game
  code again), `tests/scene60_counters.c`.
- **Measured:** `snx-test-scene60-counters`: 80 checks pass (30 skips in 90 updates at 90, 60 in 120 at 120, none at
  60 or below; each form's change; every table word a form the hook knows). Launcher logic tests pass. The game code
  compiled with the 101 hook points by the launcher's `build_code` in 79 s (block cache warm). Fight route in the
  isolated copy, `resolution=1`, `cutscene_fps=60`, presses scaled by rate / 60 (milliseconds don't work: loading
  screens run fewer presents, and run `fc90-a` stuck at a chapter map pop-up), runs in
  `diagnostics/desktop-launch-20261028/fc*`:
  - `67 of 67 frame counters count play's time` at start. A temporary line (taken out) counted, per 900 updates: in
    play 293-300 skipped (a third, as due) and 10,900-18,000 counter steps held; in the title's scenes and menus 0-8
    skipped (fixed steps count as made).
  - `fc60-b` (60), `fc90-old` (0.3.6's runtime and code at 90), `fc90-c` and `fc90-d` (this change at 90): the same
    pictures every 5 s, through the menus, the walk to the ledge, the fall, the game over and the restart. Dense shots
    every 0.2 s of the walk (`fcd90-old`, `fcd90-new`) match frame for frame. `fc90-b` (this change, the first run
    after the compile) had a 40 ms window and fought an enemy instead of falling: once in three, the repeat didn't.
  - Frames on the display at 90: median 11.25 ms in every window, p99 12-15 ms (`fc90-c`), as 0.3.6 (12-16.5 ms).
    Exit 0 and production saves unchanged in every run.
- **Not done:** nothing in the game timed against the clock yet: the double tap at 200 ms, a Witch Time's length, a
  hit-stop (the route's scripted input doesn't reach them). Not played by hand. Witch Time's sites they took out or
  never read stay frame-counted (about 52 by their count, and the scenes' step counters). Not on AMD. The isolated
  copy holds this build now; its earlier exe, code, `.abi`, `.hints` and `.ini` are in the session scratchpad
  (`iso-backup`).

## Shader preparation in the game, before the title (2026-11-22; installed, not released)

A player's request (2026-11-06) and the user's (2026-11-22): prepare shaders in the game, behind a progress bar,
before the title, instead of a separate launcher step.

- **Mechanism:** when `prepared.txt` doesn't name this driver and scale mode, a play run covers the picture with its
  own screen (black, "Preparing shaders", a bar, "N of M") and holds the game's input, as the menu does (`ui_hold`).
  Preparation then runs as the launcher's pass does: all threads but two, the same finish rule (5 s with nothing new
  after the game's stages). At the end it writes `prepared.txt` and takes the screen away: the game is at its title
  loop by then. The game runs underneath, as it must: it gives its programs as the title loads (7,838 by 17 s), and
  the title's music plays. Extra workers sleep once the screen is down, so play keeps half the threads at low
  priority. `SNX_NVN_PREP_SCREEN=0` leaves preparation in the background. The launcher's Play skips its own Prepare
  step for a game with the screen (`can_prepare`); Help's Prepare again still runs the pass.
- **Files:** `gpu/vk/nvn_res.c` (`g_prep_screen`, `prep_frame`, `prep_worker`, `vk_nvn_prep_screen`) and `nvn_res.h`,
  `gpu/vk/ui.c` (`prep_draw`), `runtime/platform/ui.c` and `ui.h` (`ui_hold`), `runtime/platform/backend/sdl3.c`,
  `launcher/bayonetta_launcher.py` (`can_prepare`).
- **Measured** (isolated copy, shader cache set aside, from boot with no route, A pressed every 5 s from 20 s, at 90,
  `prepscreen-a`): the screen came up at 1.6 s; preparation ran on 14 workers, 5,797 stages, finished at 106 s; at
  99 s the title still stood (17 presses held), the press at 110 s opened the main menu. A second run on the same
  empty folder (`prepscreen-b`) finished at 12 s: the driver's own disk cache had the compiles. Exit 0 and production
  saves unchanged in both. Launcher logic tests pass.
- **Not done:** the screen itself not seen: the runtime's screenshots copy the game's picture before the overlay, and
  the window was behind other windows. To check on the first Play after installing (the installed game's cache is
  for the old driver, so it will show). Not run on AMD. A tessellation or geometry stage isn't prepared, as before.

## A driver update asks for Prepare shaders before the first Play (2026-11-22; not released)

The user's play of 2026-11-22 23:19 stuttered. The NVIDIA driver went from 621.24 (`9b460000`) to 621.35 (`9b48c000`)
since the play of 2026-11-18, so the game threw the 270 MB pipeline cache away ("written by another build of the
driver") and prepared shaders during play: 134 pipelines took long after 60 s (26 s in all, the longest 851 ms), 44
of 52 long frames waited mostly on one, 4,040 of 7,838 stages were left when the user quit at 125 s. Frames held a
median of 11.25 ms throughout.

- **Mechanism:** `shaders_ready` compared the driver of the last preparation (`prepared.txt`) with the driver of the
  last run (`gpu.txt`), which the game writes when it starts. After a driver update, the first Play still saw the old
  driver in `gpu.txt` and played without preparing. Now the launcher records Windows' display adapters and driver
  versions (the registry's display class, `DriverDesc` and `DriverVersion`, any vendor, no admin rights) in
  `shader_cache/drivers.txt` each time it starts the game, and shaders aren't ready when they differ from what Windows
  says now. Nothing recorded yet, or nothing readable: the game's own check alone, as before.
- **Files:** `launcher/bayonetta_launcher.py` (`display_drivers`, `record_drivers`, `shaders_ready`, the old window's
  Play), `launcher/web_launcher.py` (`_start_game`), `launcher/test_launcher.py`.
- **Measured:** launcher logic tests pass (no record, the same drivers, a changed driver, an unreadable registry). On
  the RTX 3080 it reads `NVIDIA GeForce RTX 3080 32.0.16.2135`.
- **Not done:** not run through a real driver update. An integrated GPU's driver update also asks for a preparation
  (quick: the game finds nothing to do).

## The Update button, tested headless (2026-11-22)

`newer_release`, `update_asset`, `update_fetch`, `update_script` run with Python against a test install in the
session scratchpad: the 0.3.6 package with VERSION set to 0.3.5 (the launcher's update code is the same in both), with
a player's save, mods, `.ini`, shader cache, code cache, backups and an `.abi` stamp.

- **Result:** GitHub's 0.3.6 found as newer, its 88 MB zip downloaded, size and SHA-256 matched, unpacked in 8.4 s. The
  batch file (without its last line, which starts `B3-Express.exe` again) copied 1,016 files, none failed; VERSION
  reads 0.3.6, the exe and launcher files are the new ones, every player file is unchanged, the stamp is gone and
  `update/new` removed.
- **Two runs that failed, both the test's own doing:** without `logs/` (made by `update_apply`, which the first run
  skipped) the redirect to `update.log` fails, robocopy never runs, and errorlevel 1 counts as success: the stamp and
  download are deleted and the old version stays. Run from the package's own Python, `python.exe` is in use, robocopy
  gives up after 30 retries and the script takes its failure path: the releases page opens, stamp and download stay.
- **Not tested:** the window's button, waiting for the real launcher to close, the starter `B3-Express.exe` still
  running while robocopy replaces it, and the next Play's compile from `code_cache`.

## AMD Vega: pixel stages read their quad through fine derivatives (issue 17, 2026-11-23; not released)

- **Lead:** Witch Time PR 88 (and 74). AMD's Windows compilers count the subgroup lane over live lanes only and drop
  helpers early. Derivatives are the one quad read every driver must keep helpers running for. Our test builds 1-3
  showed Vega's compiler crashes on a shuffle whose lane comes from the position, and that quad swaps by position
  leave the dots.
- **Change** (`gpu/shader/spirv_translate.c`, written here with Witch Time's `mx_quad` as the reference):
  - `quad_place`: a pixel's place in its 2x2 quad from FragCoord (x & 1, plus 2 on an odd row). SR_LANEID, SHFL's
    lane and FSWZADD's mode use it in pixel stages; the lane masks (EQ, LT...) stay on the subgroup lane for VOTE.
  - `quad_read`: the neighbour is the value plus or minus `OpDPdxFine` / `OpDPdyFine` of its 16-bit halves as
    floats (exact). An XOR by a constant emits only that direction.
  - Pixel-stage SHFL uses no subgroup operation any more: no shuffle, no live-bit ballot, no quad swap. A source
    outside the quad keeps the pixel's own value. Other stages are unchanged.
  - `g_shader_quad` and `spv_emit_subgroup_quad_swap_u32` are gone; `spv_emit_derivative_fine` and the
    DerivativeControl capability are new. `source/docs/CREDITS.md` credits Witch Time.
- **Measured** (RTX 3080, runtime 21ecd272 on 0.3.6's game code f13c034c, 1.0x):
  - Shop route, cold and warm (runs `deriv-10`, `deriv-10b`): exit 0, saves unchanged, no crash, 0 failed
    pipelines; dots 1,842 and 1,861 against 1,878 (`rev-10`) and 1,882 (`scale-10`). Bayonetta's crop looks the
    same as 0.3.6; whole-frame differences follow her pose and the animated menu, not the picture.
  - Costume route on the user-save copy (`costume-deriv` against `costume-rev`): exit 0, saves unchanged, no crash or
    failed pipeline; the character select's models show no dots or lines in either.
  - Shop's SPIR-V (`SNX_NVN_SPV_DUMP`): 105 pixel stages, 26 with fine derivatives, none with a shuffle or quad
    swap. The 26 compile with `amdllpc` gfx1100 at wave 64 and 32 (52 of 52), none with `mbcnt`.
  - `shader_direct_structure` checks a pixel stage's SHFL emits fine derivatives and no shuffle, quad swap or
    subgroup lane input. `shader_direct_regressions` fails its lane-after-branch case (issue 126) on HEAD's
    translator too: not from this change.
  - Fight bench (`diagnostics/bench-20261112/bench.py`, 1.6x, a picture every 500 frames; the GPU at 30-40 % from
    Wallpaper Engine for every run, so the idle check was skipped): shader preparation from an empty cache, then the
    remnant 9 fight twice, against `rev` (0.3.6 code plus the crash naming). With NVIDIA's driver cache warm for both
    (`bench-deriv2.out`): preparation 13.9 s against 14.2 s, all 5,797 stages. The rows are first fight / warm fight:

    | Build | Frames | Over 33 ms | Hitches >= 50 ms | Longest | 1% low (median / worst window) | Pipeline waits |
    |---|---|---|---|---|---|---|
    | deriv2 | 5,084 / 5,098 | 23 / 24 | 15 / 15 | 144 / 135 ms | 43.5 / 13.2 FPS | 129 / 111 ms |
    | rev | 5,084 / 5,080 | 24 / 21 | 15 / 15 | 141 / 140 ms | 43.5 / 13.7 FPS | 143 / 88 ms |

    Exit 0 and saves unchanged in every run, no crash, failed pipeline or rejected shader. The pictures follow the
    same route as 0.3.6's (title, chapter map, character select, Giza arena, the restart card) with no dots, lines
    or floor grid. The first attempt (`bench-deriv-first.out`) had its preparation window closed at 56 s (an SDL
    quit, not the runtime), so its first fight waited behind the in-game progress bar: not counted.
- **Test build 4:** `release/B3-Express-0.3.6-fix-issue17-4.zip` (17.6 MB, runtime 21ecd272 on 0.3.6, README).
- **Reporter's result (2026-11-22, Vega, driver 0x00800117):** it runs, no crash. Faces and skin are clean now;
  hair, Bayonetta's outfit and the shop characters' clothes still have the dots and lines.
- **Not checked:** on NVIDIA it has run the shop, the character select and the bench fight, but nobody has played
  it by hand.

## AMD Vega: the flow loop reads the quad at its header (issue 17, 2026-11-23; not released)

- **What's left after test build 4:** 2 of the shop's 26 quad-reading pixel stages are straight-line code (the
  face, now clean); 24 run in the flow loop (`OpLoopMerge` plus a switch on pc). There a pixel takes the
  derivatives inside its block while quad mates wait at another pc (the converging loop runs only the subgroup's
  lowest pc), or have returned at `EXIT`. Vulkan leaves those derivatives undefined: AMD's Windows driver reads
  garbage, NVIDIA's keeps the quad. Witch Time hit the same in its dispatcher (PR 98, the dots and lines gone on
  an RX 6700 XT on AMD's Windows driver).
- **Change** (`gpu/shader/spirv_translate.c`, pixel stages with a SHFL and flow, `quad_loop`):
  - `EXIT` writes the outputs, sets pc to the end and stays in the loop. The loop ends when no pixel of the quad
    has work (`quad_any` through derivatives, the same answer in all four), so every pass has the whole quad.
  - Each SHFL starts its block. The loop header, on a pass where any pixel of the quad runs a SHFL block,
    snapshots every shuffled register's three neighbours (`quad_snap`, through `quad_across`); the SHFL reads the
    snapshot. The block starts with the SHFL, so the snapshot is its source's value.
  - The alpha test demotes instead of killing in these stages, so the pixel stays readable; KIL already demoted.
  - These stages skip the in-order (linear) path. `quad_read` is split into `quad_across` and `quad_pick`.
- **Measured** (RTX 3080, runtime e0f3e6ab on 0.3.6's game code f13c034c, 1.0x):
  - The shop's 105 pixel stages from test build 4's dump (`fix4spv/*.bin` wrapped as corpus entries) all
    translate. The 24 flow-loop stages now have one `OpReturn` (the end), a demote and no `OpKill`, 18 derivative
    ops, all in the header and the exit test (one shuffled register each), and no shuffle or quad swap. All 105
    compile with `amdllpc` gfx1100 at wave 64 and 32 (210 of 210), with `s_wqm` around the derivatives. Vega has no
    offline target.
  - Shop route: the cold run (`snap-10`) missed its presses while shaders compiled and stayed on a chapter-select
    popup; warm (`snap-10b`): exit 0, saves unchanged, 0 failed pipelines, dots 1,849 (Bayonetta's crop 292)
    against test build 4's 1,858 (294, `deriv-10c`, run the same hour). The fur coat, outfit and hair look the
    same as build 4.
  - `shader_direct_structure` has a new case: a pixel stage with a branch and a SHFL keeps the loop, uses fine
    derivatives, no shuffle, and has a single `OpReturn`. `shader_direct_regressions` still fails only its
    lane-after-branch case (issue 126), as on HEAD.
- **Test build 5:** `release/B3-Express-0.3.6-fix-issue17-5.zip` (17.6 MB, runtime e0f3e6ab on 0.3.6, README;
  it names the mouse-look and 90 FPS changes it also carries).
- **Not checked:** Vega (the reporter's test), the costume route (another session was running its game), the
  fight bench, hand play. The runtime is built from today's tree, so it also carries the uncommitted work of other
  sessions (mouse look, 90 FPS counters).
- **Reporter's result (2026-11-23):** build 5 ran (its log has the `mouse_look` line; translated SPIR-V isn't
  cached on disk, so the new shaders were used), no crash, mouse look "works perfectly". The dots are unchanged:
  Bayonetta's dress and hair, Viola's jacket, props (bottles, a statue) and the ground. Enlarged, they are dark
  pixels and dashes along the meshes' triangle edges, where a 2x2 quad has helper pixels outside the triangle.
- **Next lead (not built):** the converging loop sends helper pixels to their own pc every pass
  (`lowest = is_helper ? selector : lowest`, there since the repo started), so a helper runs ahead of its quad's
  real pixels while they wait for the subgroup's lowest pc. Derivatives, implicit-LOD samples and `quad_snap` then
  read the helper's registers from another point of the program. On Maxwell a helper follows its warp. A wave of 64
  waits more than NVIDIA's 32, which would fit Vega showing it most. Fix idea: a helper takes its quad's real
  pixels' lowest pc, with the quad-wide loop exit for every pixel stage that converges, not only those with a SHFL.

## AMD Vega: helper pixels follow their quad in the converging loop (issue 17, 2026-11-24; not released)

- **Change** (`gpu/shader/spirv_translate.c`, `quad_wide`): every pixel stage whose flow loop converges (not the
  in-order path) now keeps its quads together the way build 5 did for the SHFL stages: `EXIT` stays in the loop, the
  loop ends quad-wide, the alpha test demotes. At the loop header a helper takes the minimum of its quad's real
  pixels' lowest pc (`quad_across` through fine derivatives) instead of its own pc, so it runs each block with them.
  A quad with no real pixel left runs at its own pc, so the loop still ends. The old rule (a helper at its own pc,
  there since the repo started) stays for stages without `quad_wide`, which have no such loop.
- **Measured** (RTX 3080, runtime e0491be6 on 0.3.6's game code):
  - The shop's 105 pixel stages translate; 26 converge (24 with a SHFL, 2 without), each with one `OpReturn`, no
    `OpKill`, 12-24 fine derivative ops. All 210 compile with `amdllpc` gfx1100 at wave 64 and 32. No `spirv-val`
    here.
  - `shader_direct_structure` passes; `shader_direct_regressions` fails only its issue 126 lane case, as on HEAD.
  - Shop route: cold `help-10` and warm `help-10b`, exit 0, saves unchanged, 0 failed pipelines, dots 1,828 and
    1,835 (Bayonetta's crop 291, 295) against build 5's 1,849 (292, `snap-10b`). Viola's coat and hair look the
    same, close up. The 3080 never showed the helper drift, so this only shows nothing broke there. A build 5 rerun
    the same hour (`snap-10c`) was cold again and stopped on a popup.
- **Test build 6:** `release/B3-Express-0.3.6-fix-issue17-6.zip` (17.6 MB, runtime e0491be6 on 0.3.6, README; it
  asks for the same pictures as build 5 and whether the frame rate dropped).
- **Not checked:** Vega, the GPU cost of the extra derivatives and the longer loops (no fight bench), the costume
  route, hand play. The runtime also carries the other sessions' uncommitted work.
- **Reporter's result (2026-11-23):** runs, no crash, but 5-10 FPS in open areas where 0.3.6 holds 25 at least. The
  log agrees: 104 GPU waits (`nvnSyncWait`) with a median of 92 ms, against 1 in build 5's log. So the per-pass
  derivatives (the helper's quad minimum and the quad-wide exit test) in every converging pixel stage cost Vega's
  GPU several times its frame. Their one new picture is a level we have no build 5 picture of: Bayonetta's suit looks
  cleaner, the sky still has dotted lines along triangle edges. They said nothing about the dots and re-attached
  build 4's shop picture. Build 6 can't ship as it is.
- **Reverted (2026-11-24):** the user judged the level picture: the sky's lines belong to a mesh that should be
  invisible, so build 6 didn't fix them, and it's several times slower. `quad_wide` is out of `spirv_translate.c`;
  the shop's 105 stages translate byte for byte as build 5's (`snap-out` against `r5-out`), and the shader tests
  are as before (the issue 126 lane case only). The helper drift lead is dead as the cause of the Vega lines.
- **Build 5 reverted too (2026-11-24, the user's call):** `quad_loop`, `quad_snap` and `quad_any` are out, with
  build 5's structure test case and its CREDITS wording. The tree is build 4's derivative quad reads again: of the
  shop's 105 stages, the 24 flow-loop ones change back (`EXIT` returns, no snapshot), the other 81 are byte for byte
  as before. Shader tests as before. Not rerun in the game.

## AMD Vega: a helper pixel leaves the flow loop after 4096 passes (issue 17, test build 8, 2026-11-24; not released)

- **Reporter on build 7** (`report-20261123-233124`): the lines are "still present but barely visible" (the best so
  far), but every start ended in a lost device after 20-68 s (title screen, models right), with AMD's tool reporting
  a driver timeout: a GPU hang.
- **Mechanism:** a helper lane (outside the triangle, run only for its quad's derivatives) runs at its own pc in the
  converging loop, on extrapolated inputs, and a loop on them may never end. Before build 7, the kill or return inside
  the loop made AMD's compiler drop the quad's helpers (the masking PR 512 is about), which ended them by accident.
  Build 7 moved the kill after the loop, so nothing ends them: the wave spins until Windows resets the GPU. Upstream
  ScarletNX hit the same after PR 512 and fixed it in `d48364cc` (2026-11-13, an RTX 4090 hang in another game).
- **Change** (`spirv_translate.c`, on build 7): a fragment program with flow counts its lane's passes of the flow
  loop (`helper_rounds`); a helper leaves the loop after 4096 (`helper_loop_exit`, ORed into the loop's exit).
  Lanes that are not helpers run as written. A helper's writes are dropped already (`lane_dead`), so only its quad's
  derivatives after the loop can differ. Upstream takes 65536: 4096 here because each pass runs one block, and a
  wave waits for its slowest helper on a GPU as small as Vega's. `shader_direct.c` checks a loop that may not end
  (`@P0 BRA` to itself) gets the helper ask and the 4096.
- **Measured (RTX 3080):** the shop's 105 shaders translate, 66 differ from build 7 (all 29 loop stages carry the
  exit), amdllpc compiles all 210. Shop route cold and warm: exit 0, saves unchanged, 0 failed pipelines, dots 1,806
  and 1,822 (build 7 1,826), the character as build 7's. Fight route at 90, two warm runs: 1% low 60.9-61.6, 0.1%
  low 24.5-26.2, GPU 5.63-5.74 ms a frame (build 7 57.8-61.6, 22.4-23.5, 5.39-5.84). The cold run recompiled 90
  fragment libraries (median 95 ms, 3 over 1 s, as build 7's first run had): once per shader change, before play
  for players who run Prepare shaders. Package `release/B3-Express-0.3.6-fix-issue17-8.zip`, runtime `550dd316`.
- **Not checked:** Vega (the 3080 never hung on build 7 either), `spirv-val`, hand play. Whether 4096 passes is
  short enough for Vega's 2 s timeout when many quads have such helpers.
- **Reporter on build 8** (2026-11-24, `report-20261124-005915`, 4 pictures): no crash, "opens and loads
  flawlessly"; Bayonetta, demons and others "a little cleaner", the lines "thinner"; the level ("overworld") still
  wrong. Frame rate below 0.3.6, but about 15 FPS above the earlier test builds in open areas. The pictures still show
  thin dashed lines along triangle edges on the shop's characters (fur, clothes), Bayonetta's hair and faintly her
  face (Equipment), and on static scenery in the street level (road, buildings, cars). Their log's GPU time a frame:
  median 27 ms over 21 windows (build 5's report: 18 ms), scenes not matched.
- **Read:** the dashes on plain scenery aren't from loop helpers alone. Already in place and ruled out: position is
  `Invariant` and FMUL/FFMA carry `NoContraction` (a depth prepass mismatch on AMD's compiler). Lead: our flow loop
  lets each lane leave on its own (`finished` per lane), so a pixel that finished stops answering its quad's
  derivatives while the others go on. Witch Time's dispatcher (PR 98, clean on the same driver) keeps every lane in
  the loop until the wave is done (`subgroupAny(pc != 0)`). Built as test build 9 (next section).

### AMD Vega: a converging pixel stage leaves its flow loop only when the subgroup is done (issue 17, test build 9)

- **Change:** at the flow loop's end, a converging pixel stage (one with derivatives, quad reads or votes) tests
  `subgroupUMin(pc) == UINT32_MAX` instead of its own pc. A lane that reached EXIT (or KIL) takes the wait case until
  the others are done, so it stays in the loop and keeps answering its quad's derivatives. Helpers still leave after
  4096 passes (build 8). Other pixel stages keep the per-lane exit.
- **Mechanism (unconfirmed on Vega):** a lane that left the structured loop is inactive, and a derivative or quad
  read taken by its quad mates on a later pass reads an undefined value. NVIDIA keeps the old register, AMD may not,
  which fits dashes exactly along triangle edges, where quads are partly covered.
- **Files:** `source/src/nx/gpu/shader/spirv_translate.c` (dispatch_merge), `source/tests/shader_direct.c` (a
  looping pixel stage with a quad read has two UMin: the pass's pc and the exit).
- **Measured (RTX 3080):** shop route dots 1825 (build 8: 1822), picture clean. Fight route at 90, warm: 1% low
  61.8/61.7 vs build 8's 61.5, GPU 5.48/5.45 ms vs 5.35. Shop corpus: 105/105 translated, 26 changed (the 26
  converging pixel stages), amdllpc gfx1100 105/105.
- **Packaged:** `release/B3-Express-0.3.6-fix-issue17-9.zip` (runtime bdb75cf2). Uncommitted.
- **Not checked:** Vega (only the reporter can), `spirv-val`, hand play, Vega's frame rate.
- **Reporter on build 9** (2026-11-24, `report-20261124-114426`): device lost again, at the title or at stage select,
  as build 7 (`FATAL: vulkan: the GPU device was lost` at 16-31 s, a 2.2 s `nvnSyncWait` before it).

### AMD Vega: helpers don't hold the flow loop's exit (issue 17, test build 10)

- **Mechanism:** build 9's exit vote counted helper lanes. A helper looping on extrapolated inputs keeps its pc below
  `UINT32_MAX` up to its 4096 passes, so the whole wave ran 4096 passes for every edge quad with such a helper, past
  Vega's timeout. In build 8 the real lanes left on their own, and the helpers alone (cheaper, or masked off by AMD
  once no real lane needed them) ran out.
- **Change:** the exit vote takes `UINT32_MAX` for a helper (`subgroupUMin(helper ? UINT32_MAX : pc)`), so the real
  pixels decide and the helpers leave with them. The 4096 cap stays for a helper that outlives them.
- **Files:** `spirv_translate.c` (dispatch_merge), `tests/shader_direct.c` (comment).
- **Measured (RTX 3080):** shop dots 1810 (build 8: 1822), picture clean; fight at 90 warm: 1% low 61.1/60.8, GPU
  5.56/5.39 ms (build 8: 61.5, 5.35). Shop corpus 105/105, 26 changed vs build 9, amdllpc gfx1100 105/105.
- **Packaged:** `release/B3-Express-0.3.6-fix-issue17-10.zip` (runtime 154ba1b2). Uncommitted.
- **Not checked:** Vega, hand play. The selector's UMin still counts helpers (as in builds 4-8, which didn't hang).
- **Reporter on build 10** (2026-11-24, `report-20261124-124948`, 5 pictures): no crash. Lines unchanged from build 8:
  dashed edges on the street, the Equipment close-up (face, armour), Character Select (coat, Bayonetta, Jeanne), and
  one picture where a dark model shows every triangle edge in red, interior edges included. Frame rate "around 15 FPS
  less than usual" (log: median 27 ms a frame, 1% low 27 FPS). The wave-wide exit didn't help: the per-lane exit is
  not the cause. Interior edges, on every kind of model, point at quads that straddle an edge (helper lanes) in any
  shader, not only the 26 converging ones.

### AMD Vega: pixel stages with loops run their blocks in order (Witch Time PR 272 and 276, issue 17, test build 11; fixed, committed `1a88671`)

- **Change:** a pixel stage whose jumps back carry no quad read or vote (SHFL, VOTE, FSWZADD) no longer uses the
  `switch (pc)` dispatcher. Its blocks run in order as in the forward-only path, and each stretch a jump back covers
  (overlapping stretches merged) becomes a structured `OpLoopMerge` loop that goes round while pc points back into it.
  A helper still leaves after 4096 passes. BRA, SYNC, BRK, CONT and BRX back are taken; PCNT/CONT no longer rule
  out the in-order path. `SNX_SHADER_LOOPS=0` gives the dispatcher back (output byte for byte build 10's).
- **Why:** Witch Time moved the same shaders out of their dispatcher (PR 272: the volume light from 0.43 to 0.02 ms on
  RDNA4; PR 276: the shuffle shaders whose shuffles lie outside loops). Derivatives then sit in structured flow, with
  no subgroup minimum per pass. Unconfirmed on Vega.
- **Files:** `spirv_translate.c` (region_end, the loop around the blocks), `shader.c/.h` (`g_shader_linear_loops`),
  `device.c` and `tools/snx_shaderc.c` (`SNX_SHADER_LOOPS`), `tests/shader_direct.c` (a loop with no quad read has no
  OpSwitch; the UMin case puts its SHFL inside the loop).
- **Measured (RTX 3080):** shop corpus 105/105, all 29 looping pixel stages now without OpSwitch, `spirv-val` 105/105,
  amdllpc gfx1100 105/105. Shop dots 1795 (build 8: 1822), clean. Fight at 90 alternated with build 10: 1% low
  57.8/59.3 vs 56.0/57.9, GPU 5.49/5.73 vs 5.56/5.85 ms. A first A/B was void: another program held the GPU at 77%.
- **Packaged:** `release/B3-Express-0.3.6-fix-issue17-11.zip` (runtime 26b4cc58). Committed 2026-11-25 as `1a88671`.
- **Reporter's result (2026-11-24, Ryzen 7 4800H Vega, driver 00800117):** "The black polygonal lines are completely
  gone", performance noticeably better, also at 1920x1080. Log: a 48-minute session and a 10-minute one, both exit
  cleanly, no warnings or errors; frames on the display median 22-26 ms (build 10: 26-27 ms). Shader preparation
  never runs there (the driver has no pipeline libraries, `has_gpl`), same as build 10. The clouds still look wrong
  to them (separate, not looked at).
- **Removed before the release:** the diagnostic test switch (`main.c` diag_variant, the window title's TEST tag,
  SHFL's test 2). Translator output after removal: byte for byte build 11's on the shop corpus.
- **Not checked:** hand play here, other scenes' loop shaders (only the shop corpus) except through the reporter.

## Upstream ScarletNX for less work a frame: optimized pipeline links tried, no gain, reverted (2026-11-24)

- **What upstream has** (to `04e68563`, 2026-11-23): most of its per-draw CPU work (`e23e7b9f` vertex streams,
  `cff638b3` constant buffers, `15b6a611` texture rechecks, `f54dbe1f`, `9c770735`) is in `render.c`, `buffer.c` and
  `texture.c`'s draw path. Bayonetta's draws don't go through it: they go through our own replay (`nvn_res.c`), which
  upstream doesn't have (theirs is `gpu/nvn/`). `4f9f4b67` (command buffers in batches) fixes waits our render
  thread doesn't have (submits wait 0.00 ms). What we share is the translator, `pipeline.c`, `device.c`.
- **Tried:** upstream's optimized links (`pipe_optimize` and `5ef73739`). Our pipelines are fast links of four
  libraries, never optimized. The port: libraries keep link-time optimization info; a new pipeline first asks the
  pipeline cache for its optimized link (`FAIL_ON_PIPELINE_COMPILE_REQUIRED`); else the fast link (now outside the
  cache) serves the draws and one low-priority worker makes the optimized link and swaps it in.
- **Measured** (RTX 3080, fight route at 90, the A/B of the build 7 section, the last 3 s left out as the timed exit):
  it works (in warm runs 1,000+ new pipelines a run came optimized from the cache, 0-5 made behind), but it changes
  nothing. Three warm runs: 1% low 59.8-60.9, 0.1% low 21.3-22.8, GPU 5.40-5.66 ms a frame, against build 7's five
  59.5-62.6, 22.5-23.7, 5.33-5.64. NVIDIA's fast link already runs as fast as the optimized one. The Vega reporter's
  driver (`0x00800117`) has no pipeline libraries at all (their logs have no library line), so it would change
  nothing there either.
- **Reverted:** the tree is build 7 again (the rebuilt exe differs from `f3413dbc` in its timestamp only). The port
  is kept as `diagnostics/pipe-optimize-20261124/pipe-optimize.patch` (`SNX_NVN_PIPE_OPTIMIZE=0` turns it off) for
  a test on a newer AMD card, where fast links are known to cost GPU time. Not measured there.

## AMD Vega: a pixel is killed after the flow loop, not in it (issue 17, test build 7, 2026-11-24; not released)

- **Mechanism:** upstream ScarletNX PR 512 (section below). A kill or demote inside the flow loop makes AMD's Windows
  compiler narrow `exec` to the live lanes' quads for the rest of the loop, so a quad's helpers stop running and
  its derivatives read stale values: dots and lines along edges, and alpha-tested meshes (the sky, hair, fur).
- **Change** (`spirv_translate.c`, on build 4's tree): fragment programs with flow get a `discarded` bool. KIL and a
  failed alpha test set it, EXIT sets pc to UINT32_MAX (and leaves the predicated-exit block) instead of returning,
  and one `OpKill` runs after the loop or after the in-order blocks. `SR_THREAD_KILL` and the helper guard on stores
  read `lane_dead`, helper or marked. Programs without flow are unchanged (kill and demote where they were).
  `shader_direct.c` checks a KIL-and-EXIT program: no demote, one kill, one return.
- **Measured (RTX 3080):** the shop's 105 shaders all translate, 66 differ from build 4 (the 29 loop stages and
  the in-order flow stages); the 29 loop stages each have 0 demotes, 1 kill and 1 return. amdllpc compiles all
  210 stages. Shop route, cold and warm: exit 0, saves unchanged, 0 failed pipelines, dots 1,825 and 1,826 (build 5
  1,849, build 6 1,835), and the character crop matches build 6's picture by eye. Package
  `release/B3-Express-0.3.6-fix-issue17-7.zip`, runtime `f3413dbc`.
- **Stability and frame times at 90 (RTX 3080, 2026-11-24):** fight route in the isolated copy, `frame_rate=90`,
  `resolution=1.6`, no screenshots, RTSS limit 0. A/B against the same runtime with PR 512 off (`discarded` never
  made, `832fb430`), so only the change differs. Stats after the first 30 s, from `SNX_PRESENT_TIMES`; 1% low is the
  FPS of the slowest 1% of frames' average. Every run exit 0, saves unchanged, no Vulkan error, lost device or
  failed pipeline.

  | Warm runs | Avg FPS | Median / p99 ms | 1% low | 0.1% low | Over 19 ms | Over 33 ms | Worst ms | GPU ms a frame |
  |---|---|---|---|---|---|---|---|---|
  | Build 7 (3) | 89.7-89.8 | 11.13 / 12.21-12.80 | 57.9-60.9 | 23.1-24.2 | 10-12 | 3-4 | 68-70 | 5.33-5.64 |
  | Without PR 512 (3) | 89.6-89.7 | 11.13 / 12.43-12.78 | 54.6-58.2 | 19.4-24.3 | 12-13 | 3-4 | 66-95 | 5.61-5.72 |

  No change beyond run-to-run noise, so nothing to optimize. Build 7's first full run (`ab-b7-1`; its cold warm-up
  missed the presses and never reached the fight) compiled the changed shaders' pipelines in play: 9 fragment
  libraries over 100 ms, frames of 580-640 ms, 1% low 15.5. That is once per shader change on each machine, the cost
  any translator change has, and the in-game preparation screen covers it for players. Runs `ab-*` in
  `diagnostics/desktop-launch-20261028/`.
- **Not checked:** Vega itself (the 3080 never showed the bug), `spirv-val` (not on this machine), the costume route,
  hand play, frame times on AMD. The build carries the uncommitted mouse camera and 90 FPS work, as builds 5 and 6 did.

## Issue 17 review: what the RX 6700 XT, Witch Time and upstream ScarletNX did (2026-11-24; nothing built)

- **The RX 6700 XT fixes (0.3.0):** `55a53f3` and `a43b413`. The characters' pixel stages OR a light-list word over
  their 2x2 quad with three SHFL, then loop over those lights. At a triangle edge a helper's word was garbage, so
  the quad's lists differed, its lanes looped apart, and the quad's derivatives broke (dots and lines along edges).
  Fix: ballot out helper sources, then `OpGroupNonUniformQuadSwap` inside the quad (helpers take part in quad
  operations). That newer driver was clean after it. Build 4 replaced those quad swaps with fine derivatives, and
  that hasn't run on the RX 6700 XT.
- **Vega's driver** (from the reporter's logs and Witch Time PR 91): Vulkan 1.3.260, build `0x00800117` (2.0.279),
  the last one AMD made for Polaris and Vega, with the older "AMD proprietary shader compiler". Vega (here Renoir's
  iGPU) runs waves of 64 only, 16 quads a wave. Pixel shaders run the quad together ("whole quad mode") for
  derivatives and implicit-LOD samples, and switch to the live pixels only for stores and exports.
- **Upstream ScarletNX PR 512** (`ec9c208c`, merged 2026-10-21; our base `5c97949` is from 2026-10-18, so we
  never had it, and upstream main still has it): on AMD's Windows driver (RX 6600, Radeon GPU Detective), after a
  demote or an alpha test's kill inside the flow loop the compiler masks `exec` with the quad mask of the live
  lanes (`s_wqm_b64`, `s_and_b64`). It changes which lanes of a quad run for the rest of the loop, and there it hung
  the GPU. Fix: in fragment programs with flow, KIL and a failed alpha test set a bool, EXIT sets pc to the end
  instead of returning, and the lane is killed after the loop; stores and `SR_THREAD_KILL` treat a marked lane as a
  helper.
- **Witch Time** (`runtime/mx_translate.c` at PR 98, its issue 26): no return or discard inside the dispatcher;
  outputs, the alpha test and KIL's discard happen once, after it. Its loop exit is wave-uniform
  (`subgroupAny`), and the snapshot is taken under `subgroupOr`, both scalar conditions. Its v0.5.2 (PRs 74, 88, 91,
  95, 98 and the issue 26 rule) was confirmed clean on an RX 580, the same `0x00800117` driver as Vega.
- **Ours, build 4's tree:** of the shop's 29 flow-loop pixel stages, 26 have an `OpKill` inside the loop (the
  alpha test at EXIT); KIL demotes inside the loop; EXIT returns inside the loop. None of builds 1 to 6 moved the
  kill or demote out of the loop: build 5 turned the alpha test's kill into a demote, which PR 512 found compiles
  to the same masking. Build 5's loop exit and snapshot condition came from derivatives (a per-lane value), not
  from `subgroupAny`/`subgroupOr` as in Witch Time.
- **Fits the symptom:** the faces build 4 fixed are straight-line stages (kill only at the end); what's left is
  alpha-tested hair, lace, fur and cloth, and a sky mesh the user says should be invisible (every pixel killed),
  all drawn by stages that kill inside the loop. Not proven: no Vega here, and no capture of the sky's stage.
- **Other upstream commits we lack** (since `5c97949`, translator): `3963d78b` SHFL in a quad as a quad op (we have
  our own), `683efe7c` structured flow where the device has `VK_KHR_shader_maximal_reconvergence` (Vega's 2023
  driver likely doesn't, unchecked), `d48364cc` a helper's loop capped (a hang, not our picture), `1087ecac`,
  `286b7121`.
- **Next test build (proposed):** PR 512's change on build 4's tree. It adds no per-pass work, unlike build 6.
  Upstream PR 512 was also the fix for AMD device-lost resets in the flow loop, which may bear on issue 16.

## The web launcher on a zip unpacked by Windows (issue 17's log, 2026-11-23; not released)

- **What's reported:** the issue 17 reporter's `launcher-errors.log` has the same error on all 78 starts since
  2026-11-20: pywebview's WinForms backend fails at `import clr` with `Failed to resolve
  Python.Runtime.Loader.Initialize`, and `web_launcher.py` falls back to the old window.
- **Mechanism:** Windows' zip extraction gives every file the download mark (a `Zone.Identifier` stream, ZoneId 3),
  and .NET Framework won't load an assembly that carries it. Only pythonnet's `Python.Runtime.dll` matters:
  reproduced with a copy of 0.3.6's `launcher/python`: clean, `import clr` works; every file marked, the
  reporter's error; every file marked but that DLL, it works.
- **Change:** `launcher/web_launcher.py` `main` deletes that DLL's `Zone.Identifier` stream before importing
  webview (an `OSError` when there is none is ignored).
- **Measured:** the same code in the marked copy's Python: the mark goes, `import clr` works, and the next start
  finds no mark (FileNotFoundError, ignored). `py_compile` passes.
- **Not checked:** a whole launcher start from a marked package, and the reporter's machine. It reaches players
  with the next release.

## AMD Vega: a pixel's quad place from its position (issue 17, 2026-11-21; reverted 2026-11-22)

- **Outcome:** reverted. Vega's driver crashes compiling a shuffle whose lane comes from FragCoord, and the part it
  compiles doesn't change the dots (issue 17 row under Open). The notes below are the history.

- **What's reported:** a Ryzen 7 4800H (Vega iGPU, AMD's maintenance driver branch) shows the dots and lines on
  characters that 0.3.0 fixed on the RX 6700 XT, on 0.3.6, in the shop and on the equipment screen.
- **Mechanism (likely, not confirmed on Vega):** pixel stages took a pixel's place in its 2x2 quad from the subgroup
  lane number: the game's own S2R LANEID (the costume shaders build SHFL's index from it), SHFL's source lane and
  FSWZADD's per-pixel mode. AMD's Windows compilers can count that number over live lanes only once helpers are
  switched off, so one helper shifts every lane after it and the pixel reads the wrong neighbour (Witch Time PR 74).
  Ruled out: render scale (clean at 0.8-1.0x on the 3080) and wave size (the 6700 XT ran 69 of 81 pixel shaders at
  wave64 too, from `diagnostics/amd-rgp-20261115/fight-f3000.rgp`).
- **Change:** `lane_number` in `gpu/shader/spirv_translate.c`: in a pixel stage with subgroup operations the lane
  number's low 2 bits come from FragCoord (x & 1, plus 2 for an odd row), the quad layout Vulkan and Maxwell share.
  `gpu/vk/device.c` logs the device's subgroup support (`vulkan: subgroups of ...`), so a player's log says whether
  quad reads are on.
- **Measured** (RTX 3080, shop route, runs `quad-10`, `quad-08` against 0.3.6's `scale-10`, `scale-08`): exit 0,
  saves unchanged, 0 pipelines failed; dark-dot counts 1,877 / 1,798 against 1,882 / 1,790, the same picture, as
  expected where the lane number is right. The new log line reads `subgroups of 32, stages 3fff, operations 7ff; in
  pixel stages yes, quad reads yes`. The shop's 26 changed pixel shaders (of 105) compile with AMD's `amdllpc`
  (RGA 2026-10, gfx1100) at wave64 and wave32. Runs used the build before `clang-format` of the new lines.
- **Not checked:** Vega itself (no card; the offline compiler has no Vega or RDNA2 target). SHFL reads outside the
  quad still go by the lane number.
- **Test fix for the reporter** (2026-11-21): `release/B3-Express-0.3.6-fix-issue17.zip`, 17.6 MB: the runtime
  `a654eb41` (this change and the issue 16 markers on 0.3.6, same imports and `SNX_ABI`) as `Bayo3_120.exe`, and a
  README with install, undo and what to attach. Run `quad-final` with that exe on 0.3.6's game code: exit 0, saves
  unchanged, dots 1,880 against 1,882.

## GPU markers for a lost device (issue 16, 2026-11-19; not released)

- **What's reported:** an RX 6750 XT loses the device at two places in the prologue (the monster landing on the
  platform, the first heavy in New York) after a GPU usage spike. A single draw running past the driver's watchdog
  is the usual cause. The test PC has the RTX 3080, and the log didn't say which work the GPU was in.
- **Mechanism of the tool:** `SNX_GPU_MARKERS=1` enables `VK_AMD_buffer_marker` and `VK_EXT_device_fault` where
  offered. Each replayed draw and dispatch writes its number as it starts and as it ends; on a lost device the log
  lists the unfinished ones (frame, draw, stages' SPIR-V hashes) and the driver's fault description.
- **Files:** `gpu/vk/device.c` (`vk_device_lost_report`), `gpu/vk/nvn_res.c` (`mk_begin`, `mk_end`,
  `vk_nvn_markers_report`), `vk.h`, `nvn_res.h`, the device-lost sites in `cmd.c`, `compute.c`, `vic.c`.
- **Measured** (`diagnostics/gpu-markers-20261119/`): the RTX 3080 offers both; fight route with markers 12 frames
  over 33 ms, worst-window 1% low 22.7, the same as without. The progress readback works (called once mid-run in a
  throwaway build).
- **Not done:** the crash itself. That needs the reporter's log with the variable set, or the RX 6700 XT.

## Release 0.3.6 (2026-11-19)

- **Contents:** the present slot's slack, 250 to 50 us: fewer frames late for the screen at 90 (section below).
- **Files:** code commit `d874ad0`; runtime `447f4090`; zip `ab1740a6`, 84.2 MB, 1,016 files; notes
  `release/RELEASE-NOTES-0.3.6.md`.
- **Measured:** fight route at 90, 1% low 57.2-57.7 to 58.8-60.4 FPS, about 2 ms more wait before the present (section
  below). New-player test on the unzipped package (`test-fresh.py --web`, RTX 3080): Install from an extracted dump in
  209 s, no error, next step Prepare shaders, the game drew 600 frames and exited 0.
- **Not checked:** played by hand; an update's recompile (the game-code ABI is unchanged from 0.3.5); AMD and Intel.

## Release 0.3.5 (2026-11-19)

- **Contents:** the replay on its own thread, 90 FPS step 1 with the double-tap window in game time, cheats (issue 15),
  prompts by button map (issue 14), and two recording cuts for a steadier 90: a stage's descriptor set reused when
  every write matches the draw before's, and textures' write checks scanning each 4 MB block once a window instead of
  once a texture (`buffer.c` `vk_tex_written_since`). Launcher: a 90 FPS switch on the Graphics page (`frame_rate` 90
  when on, 60 when off; a rate above 60 set by hand stays while it is on).
- **Files:** code commit `43e6f1c`; runtime `7e51999c`; zip `377fc30a`, 84.2 MB, 1,016 files.
- **Measured:** the recording cuts, fight route at 90 (`diagnostics/fps90-20261118/README.md`): lists 0.3-0.7 ms
  shorter in heavy frames, screenshots matching earlier builds apart from animation timing. New-player test on the
  unzipped package (`test-fresh.py --web`, RTX 3080): Install from an extracted dump in 243 s, no error, next step
  Prepare shaders, the game ran and exited 0. With the code stamp removed, as an update does, the game code compiled
  again in 14.0 s (83 blocks, the cache from the install). `test_launcher.py`, `snx-test-cheat`, `runtime_present`,
  `runtime_nro` and `code_patches` pass. 13 ctest cases fail on this Windows host for causes outside this release's
  diff (ELF linking, test include paths, `g_frames` in `vi_service`, `window_icon`, `tex_upload_rows`).
- **Not checked:** the recording cuts and the double-tap fix by hand; the launcher's 90 FPS switch in its window (logic
  tested, layout checked in a page preview); AMD and Intel.

## 90 FPS: 1% low, the present slot's slack (2026-11-19; 0.3.6)

- **Mechanism:** each frame's present slot came 250 us earlier than one period after the last, so the slot crept up on
  the frames until 1.5% of them came after it, each then 12.5-16 ms on the display. At 50 us it creeps a fifth as fast
  and sits behind more of them.
- **Files:** `gpu/vk/renderer.c` (`PT_SLACK`).
- **Measured** (fight route at 90, `resolution=1.6`, no screenshots, two runs each): 1% low 57.2-57.7 to 58.8-60.4
  FPS, 80-83 against 74-76 in steady play; frames of 12.5-16 ms 87-89 to 40-44; frames wait 1.0 to 2.7-3.3 ms on
  average before the present (latency). The GPU sits at 50% load, so it isn't the limit.
- **Not done:** what's left in the slowest 1% is the game loading at the area's title card and the result screen
  (frames of about 68 ms) and new effects' shader translation and pipelines on their first draw (20-39 ms); not
  played by hand; not on AMD. Notes: `diagnostics/fps90-20261118/README.md`.

## 90 FPS: the fight holds it (measured 2026-11-19; no code change)

- **Mechanism:** none new. 0.3.5's code, measured on the heavy fight at 90 to find recording's next cost.
- **Files:** none. A packed render-target key array (`nvn_res.c`) was tried and taken out: no measurable effect.
- **Measured** (fight route at 90, isolated copy, `resolution=1.6`, three runs): frames on the display have an
  11.25 ms median and a 12.5-15 ms p99 in every window; 1-5 of 600 are over 19 ms, about what the screenshots cost;
  the pacer still holds 5-6 ms a frame in the heaviest windows. Recording varies about 0.6 ms between two runs of one
  build. One window a frame; the driver is about 0.4 ms a frame.
- **Not done:** step 2 (frame-counted game logic 1.5x fast at 90); not played by hand; not on AMD. Notes:
  `diagnostics/fps90-20261118/README.md`.

## 90 FPS, step 1: clock floor and pacing (2026-11-18; 0.3.5)

The user wants 90 FPS first, then higher. The game's clock (`main+0xc1f600`) never lets a step drop under 1.0 (a
60 Hz frame), so above 60 it would play fast.

- **Mechanism:** `frame_rate` (60 to 120) lowers that floor to 60 / frame_rate in `hle/scene60.c`, and vi's vsync, the
  game's own pace and the present schedule tick at the same rate (`g_vi_hz`). Cutscenes run at it too, as with
  `cutscene_fps=60`. An unknown game stays at 60.
- **Files:** `hle/scene60.c`, `hle/services/vi.c` and `vi.h`, `runtime/present.c`, `runtime/main.c`,
  `runtime/options.c` and `options.h`, `tests/runtime_present.c`.
- **Measured** (fight route, isolated copy, `resolution=1`): at 90, frames on the display have an 11.25 ms median,
  the pacer still holds 1.5-6.7 ms in play, and frames over 33.4 ms are 1-5 of 600, as at 60. 9000 frames took
  111.7 s against 6000 in 113.7 s at 60, and the screenshots at the same times match: the game keeps real time.
  Cutscenes, the title's in-engine loop: 90 FPS (11.25 ms median), and the scene's own clock ran 6.69-6.73 s in
  6.70-6.73 s of real time per 600 frames (logged by `scene60.c`).
- **RivaTuner:** the user's RTSS profile for `Bayo3_120.exe` capped every run at 60 (`Limit=60`, now 90). Earlier
  "uncapped" benches were probably capped by it.
- **Not done:** step 2, what the game counts in frames (input windows, timers, hit-stops) runs 1.5x faster at 90;
  not played by hand; rides not run at 90; not on AMD; no launcher control yet. Notes:
  `diagnostics/fps90-20261118/README.md`.

## Cheats in AtmosphÃ¨re's format (issue 15, 2026-11-18; 0.3.5)

A player asked for cheats. Switch cheats are AtmosphÃ¨re cheat files (`cheats/<build id>.txt`): small programs the
console's `dmnt` runs beside the game, writing memory. The ones shared for our build (`B47CC79B88B7091E`) are 17,
and 11 of them write ARM64 instructions into the game's code, which does nothing to a recompiled game: its code is
compiled already. So cheats go two ways.

- **The change** (not committed):
  - `source/src/nx/loader/cheat.c`, `cheat.h` (new, C90, in `snx_patch` with `patch.c`): the file's parser (`[Name]`
    cheats, `{Name}` master codes, the ones on named in `cheats/enabled.txt`), the machine itself with every
    instruction of AtmosphÃ¨re's `cheats.md` (writes, loads, conditions, loops, arithmetic, held keys, static registers,
    the debug log), and `snx_cheat_fixed`: the writes into code a run always makes (outside conditions and loops).
  - `patch.c` `snx_patch_cheats`: those code writes applied as patch records, into the patch digest like an `.ips`.
    The recompiler (`loader/loader.c` `apply_cheats`) and the runtime (`runtime/loader.c` `load_cheats`) apply them
    in the same order (a mod's cheats, then its `exefs/`), so the code is compiled with them and the game refuses code
    built with other cheats, as for code patches.
  - `source/src/nx/runtime/cheats.c` (new): the cheats on run once per frame shown (`present.c`), as `dmnt` runs them:
    reads and writes only where the game has mapped readable or writable memory (`mem_query`), so a pointer into
    nothing is ignored instead of crashing; held buttons from player 1 (`hid_held`, new in `hid.c`); `0xFFF` into the
    log when a value changes. A write into code it can't do (under a condition) is warned about once.
  - `fs.c`: a cheats-only mod isn't called "left out" any more.
  - Launcher: a mod may hold `cheats/` (imported from `atmosphere/contents/<title>/cheats/` too, without a download's
    own `enabled.txt`); the Mods page lists each mod's cheats with a tick box, all off at first; the cheat files and
    `enabled.txt` count as code mods, so a change compiles again at the next Play (seconds with `code_cache/`).
- **Measured** (`diagnostics/cheats-20261118/README.md`): `snx-test-cheat` (53 checks), `snx-test-patch`,
  `snx-test-runtime-nro` and `test_launcher.py` pass. In the isolated copy with the shared file: Invincible, Halos,
  Seeds and Orbs on compiled through the launcher's `build_code` (16 code bytes patched), the game ran the fight route
  (exit 0) and accepted the code. A probe cheat logging the data cheats' addresses saw live values change with the
  fight.
- **Not checked:** any cheat's effect in play (the route takes no damage; Halos' address held 0 to 2, maybe not the
  menu total), a cheat file from another source, the launcher's Mods page in its window (logic tested, not clicked),
  turning cheats on or off while the game runs (only at start). A data-only cheat toggled still compiles again
  (`shortcut:` in `code_mods`). Our own built-in cheat list (the second half of the request): not started.

## Prompts follow the button map (issue 14, 2026-11-17; 0.3.5)

An Xbox player mapped each button to its own letter (A bottom, B right, X left, Y top) and the prompts still showed
the buttons by position, so every face prompt pointed at the wrong button. The Xbox and PlayStation pictures were
already there (since 0.1.x, from Witch Time), but they were placed by position only. "Swap A and B" moved A and B
and left X and Y, and a map made in the game's controller menu wasn't read at all. B2recomp does it with a
`layout` setting (by position or by printed letter) and prompt pictures from a mod the player installs.

- **The change** (`source/src/nx/gpu/vk/prompts.c`, `prompts.h`, `source/src/nx/runtime/platform/backend/gamepad.c`,
  `gamepad.h`; not committed):
  - `gamepad_prompts` returns the map of the pad in use (its player's map, then "Swap A and B"), the one its input
    goes through.
  - Each of the ten prompt cells (A, B, X, Y, L, R, ZL, ZR, Plus, Minus) shows the control that sends its button. A
    control bound to it from elsewhere comes first, so RB set to B shows RB though the bottom button still sends B.
  - Xbox face letters and every Switch picture are copied from the game's own cells. The rest are drawn: PlayStation
    shapes, LB/RB/LT/RT and L1/R1/L2/R2, View, Menu, stick presses (LS/RS, L3/R3) and a D-pad arm.
  - The Switch set is painted the same way now, so a Pro Controller's remap shows too.
  - The set is painted again whenever the map changes (its signature is the map).
- **Measured** (`diagnostics/prompts-20261117/README.md`): isolated copy, test save, the route's first 2100 frames,
  five settings. Main menu and chapter select footers:

  | Setting | Confirm, Back | Throw |
  |---|---|---|
  | Xbox, by position (default) | B, A | B X |
  | Xbox, map `south:A east:B west:X north:Y` | A, B | A Y |
  | Xbox, map `rb:B` | B, RB | B X |
  | PlayStation, by position | Circle, Cross | Circle Square |
  | Switch, map `rb:B` | A, R | A Y |
  | PlayStation, map `south:A east:B west:X north:Y` (2026-11-18) | Cross, Circle | Cross Triangle |
  | PlayStation, map `rb:B` (2026-11-18) | Circle, R1 | Circle Square |

  All runs exited 0 with saves unchanged. `snx-test-gamepad` passes (204 checks).
- **Not checked:** a real Xbox pad pressed by hand (the scripted presses skip the map), a change of map in the game's
  menu while it runs, the launcher's controller page (unchanged: it has "Swap A and B" and "Button prompts", no
  map). The four dots that show where a button sits only know A on the right or on the bottom. `tests/tex_upload_rows.c`
  doesn't compile against the current `texture.c` (older than this change).

## The replay on its own thread (2026-11-17; 0.3.5)

The game submits one command list a frame from its draw thread (`MainDraw`), and our hook replayed it into Vulkan
inside that call: about 8 ms of a heavy fight, half of that thread's frame. The replay also held the lock every
recorded command takes, so the game's other threads recording lists waited for it.

- **The change** (`source/src/nx/hle/nvn/cmdlist.c`, `source/src/nx/gpu/gpu.c`, not committed):
  - Recording has its own lock.
  - A render thread replays each submit's lists, then the present's bookkeeping, in the game's order. The game's
    thread decodes the state objects the lists bind, since that runs the game's own getters, and goes on. A submit
    waits only for the frame before, so the picture is at most one frame behind.
  - The GPU thread runs each GPFIFO submission and present only after the replay queued before it. The fences the
    game waits on therefore still come after the work.
  - `SNX_NVN_RENDER_THREAD=0` turns it off.
- **Measured** (`diagnostics/rt-20261117/README.md`): RTX 3080, game code `f13c034c` in every run. The time the game's
  draw thread has to spare at 60 FPS in the heavy fight (the route's last 20 s, same draws in every run). Fewer cores
  means the game limited to the first 4, 3 or 2 cores of the 5700X3D:

  | Cores | 0.3.0 | Render thread |
  |---|---|---|
  | 8 | 0.7-2.0 ms (3 runs) | 8.3-10.2 ms (3 runs) |
  | 4 | 0.6-1.0 ms | 9.5-9.6 ms |
  | 3 | 7.2-8.0 ms | 8.7-9.2 ms |
  | 2 | 0.6-1.2 ms | 7.7-8.3 ms |

  | Whole route | 0.3.0 | Render thread |
  |---|---|---|
  | 1% low, worst 10 s window | 8.9-12.0 FPS (8 cores), 10.4-10.9 (4 to 2) | 18.9-23.3 FPS (8 cores), 15.2-22.7 (4 to 2) |
  | Frames over 33 ms | 10-14 | 10-13 |
  | Hitches of 50 ms or more | 3-4, the same loading frames | 3-4, the same loading frames |

  10 runs with the render thread, all exited 0 with saves unchanged. No crash, lost device or Vulkan error. The last
  picture matched 0.3.0's.
- **Not checked:** hand play, AMD, a CPU with slower cores (fewer fast cores is only a stand-in). 0.3.0 had 7-8 ms to
  spare in one of 5 runs, and the cause wasn't looked into. The bench's "longest hitch" reads about 77 ms against
  62-66: the loading frames take the same time, but the pacer's hold now falls in another frame's line.

## 0.3.0 stability on the RTX 3080 (2026-11-17)

0.3.0's runtime (`73d8c4c7`) with game code it compiled itself (170 s, `f13c034c`), in the isolated copy with the test
save: a new player's shader preparation (our cache empty, the driver's warm), then 8 runs of the fight route
(`diagnostics/bench-20261112/bench-stab030*.out`), about 1,000 s of play. Driver 621.24.

- **Stability:** every run exited on its timer with code 0. No crash, lost device, Vulkan error or svcBreak in any log.
  The user's saves were unchanged.
- **Measured:**

  | | runs 1-2, 4-8 | run 3 |
  |---|---|---|
  | Frames over 33 ms | 10-13 (18 in the first after preparation) | 253 |
  | Hitches of 50 ms or more; longest | 3-4 (8 in the first); 59-76 ms | 239; 205 ms |
  | 1% low, median window | 47.6-50.0 FPS | 45.5 FPS |
  | Pipeline waits | 74-283 ms | 81 ms |
  | Video memory peak | 2,868-3,338 MiB | 3,020 MiB |

  Shader preparation: 5,797 stages in 16.7 s.
- **Run 3:** from 118 s to the end (about 20 s, frames 5500-6000), power fell from about 117 W to 44 W and GPU time
  doubled (14.5 against 6 ms a frame), with no error. The power matches the card's P5 state (memory at 810 MHz). Three
  more runs with clocks logged every second did not repeat it. Cause not known: the driver's power state is the lead.
- **Not tested:** hand play, other chapters, runs longer than 140 s, the AMD card.

## FSR 3.1: what it needs (plan, 2026-11-17; not started)

Today the port has FSR 1 only (`gpu/vk/fsr1.c`, `fsr=` in the INI): the scene is drawn at `resolution / fsr`, the
game's final draw goes to an image of that size, and EASU + RCAS take it to the window (`nvn_res.c`
`rp_final_draw`). That is a spatial upscale of the finished picture, after the tonemap, with no history.

Witch Time added FSR 3.1.4 (their `docs/STATUS.md` "FSR 3.1", `runtime/nvn_vk_fsr.c`, 1,043 lines, read 2026-11-17 in
`E:\download\WitchTime-v0.4.5-windows-x86_64`). Their way, adapted to ours:

- **The shaders.** AMD's FidelityFX SDK v1.1.4 (MIT): the FSR 3.1.4 upscaler's GLSL passes and GPU headers, 27 files,
  8 compute pipelines. They compile them at first use with their glslang. We would compile them offline into a
  header, as `source/scripts/build-fsr1-shader.py` does for FSR 1, so the runtime needs no shader compiler. One line of
  AMD's changes in theirs: `rw_luma_history` declared `rgba16f`, the format the host gives it.
- **The host.** AMD's `ffx_fsr3upscaler.cpp` (constants, images, clears, the order of the passes) written in C. Theirs
  is GPL-3.0-or-later like ours, and `hle/aspect.c` already ports their code with credit, so it can be ported the same
  way (credit in `source/docs/CREDITS.md`). New file `gpu/vk/fsr3.c` beside `fsr1.c`.
- **Jitter.** The engine has no jitter. Hook the camera upload (`main+0xcb9440`, called at `main+0xcbb4d4`: their
  `h_cam_upload`) and shift the scene camera's projection (+0x40) and view-projection (+0x80) by a Halton(2,3) step
  each frame, then put them back. Our hooks: `rt_hook_set` / `launcher/code-tablecalls.txt`, as `aspect.c` and
  `model_lod.c` do. The same build of the game, so their addresses are ours less `0x7100000000`. Shadows, the
  full-screen filters and the HUD use other cameras and stay unjittered.
- **Motion vectors.** The engine draws none: its velocity shaders are loaded but never bound (their finding). Step 1,
  camera only: one compute pass at the scene's size from the depth and this and last frame's view-projection without
  jitter, read from constant buffer 3 of the scene's draws (rows 32..47). Our depth is D24S8 on NVIDIA and D32 on AMD
  (`54cf29f`): the pass reads either. Step 2, objects: the translator (`gpu/shader/spirv_translate.c`) also outputs
  last frame's vertex position from last frame's bone matrices (constant buffer 14), the renderer keeps those for one
  frame, a hook on the game's palette writer (`main+0xd17d44`) says which slot was last frame's, and the moving
  objects are drawn again into the motion-vector image with depth test EQUAL. Without step 2, hair and fast limbs
  smear a little.
- **Where it runs.** On the scene's HDR colour (R11G11B10F) after the transparents, at the first bloom downsample;
  the result is copied back, so bloom, the tonemap, grain, FXAA and the HUD run as now. In the upscale modes the
  scene is drawn at the internal resolution / AMD's ratio (Quality 1.5, Balanced 1.7, Performance 2.0, Ultra
  Performance 3.0), and the first tonemap reads FSR's output. That replaces FSR 1's place after the tonemap.
- **The fade dither.** With `dither=3` (Transparent, the default) the game's screen-door fades blend instead
  (`gpu/vk/dither.c`), so FSR sees no pattern there. The passes that keep a pattern (`fade_cut`, `b3ae4c4`) need it
  to move between frames, or FSR's history keeps the holes: Witch Time cycles 13 phases of the game's pattern
  (`runtime/fsr_dither.h`).
- **Resolution steps.** Each step of the game's dynamic resolution (8 sets, 1600x896 .. 640x360) makes FSR's images
  again and resets its history. The step fix of 0.2.7 keeps our targets; FSR's history would still reset.
- **What broke for them.** Device lost on an RTX 4090 after about 1,100 frames: AMD's samplers `s_PointClamp` /
  `s_LinearClamp` were immutable samplers in push layouts never pushed. Push them as ordinary samplers from the start.
- **Their cost** (RX 9070 XT, Linux, picture 3840x2150): off 4.79 ms GPU a frame, Native AA 6.70, Quality 4.43,
  Performance 3.66; frame times the same in every mode (CPU-bound). The gain is anti-aliasing and GPU headroom, not
  FPS. Our RX 6700 XT runs about 5-6 ms GPU a frame at fixed clocks (below), so Native AA adds about a third.

Order: shaders and host with Native AA and camera vectors, checked on the fight route and in the shop; then the
quality modes; then object motion vectors. Not decided: whether FSR 1 stays as a choice.

## Game code compile cache (2026-11-16; 0.3.0)

Each compiled block of game code is kept in `Bayo3_120\code_cache\` beside the game, named by a BLAKE3 hash of its
LLVM bitcode, the LLVM version, `-O`, the triple, the CPU and its features (`loader/module_llvm.c` `unit_key`,
`--cache DIR` in `runtime/driver/recompile.c`). A compile after an update or a code mod builds only the blocks whose
bitcode changed; unused blocks are pruned after a successful build. The launcher passes the folder and runs the
compiler on every thread at below-normal priority (`launcher/bayonetta_launcher.py`).

- **Measured** (Ryzen 7 5700X3D, through the launcher's `build_code`, `diagnostics/compile-speed-20261116/`):

  | | before | after |
  |---|---|---|
  | New player's compile | 207 s | 188 s |
  | Compile after an update, same generated code | 207 s | 13 s |
  | Disk used by `code_cache/` | | 331 MB |

  The DLL comes out byte for byte the same with and without the cache (`f13c034c...`). 82 blocks of about 90,000
  guest instructions; one block is about 15 s of passes (half InstCombine) and 15 s of codegen (half instruction
  selection), single-threaded.
- **Not done:** ordering blocks by size (the best order saves about 15 s, and our predictor is poor), a lighter pass
  pipeline (changes the game code). Not measured: a code mod's compile, an update that changes the generated code.

## AMD shader preparation is AMD's compiler (2026-11-16; measured, unchanged)

A new AMD player's preparation pass takes 104-113 s against 26 s on the RTX 3080. Each stage takes 190-300 ms, of
which 0.7 ms is our translation and 10-17 ms the wait for `vk_lock` (`gpu/vk/nvn_res.c`, the progress line says so
since `a9605d6`). 8, 14 or 16 workers land within 10 % (113.5 / 103.8-108.3 / 108.8 s), the CPU is 55-69 % busy,
and leaving our pipeline cache out changes nothing (109.4 s). With AMD's own driver cache warm the pass takes about
10 s. Nothing on the runtime's side moved it.

## AMD: dots and lines on characters, grid on the arena floor (2026-11-16; 0.3.0)

Character shaders OR a light-list word over each 2x2 pixel quad with SHFL. A general shuffle on AMD reads garbage
from the quad's helper pixels at triangle edges, and a helper with a different word loops apart from its quad and
breaks its derivatives.

- **First fix** (`55a53f3`): SHFL in pixel shaders keeps its own value when the source lane is a helper or inactive
  (`gpu/shader/spirv_translate.c`).
- **Second fix** (`a43b413`): SHFL within the quad reads with `OpGroupNonUniformQuadSwap`, which includes helpers, so
  all four pixels get the same word as on the Switch (`spirv_translate.c`, `spirv_emit.c`, `gpu/vk/device.c`
  `g_shader_quad`).
- **Measured** (RX 6700 XT, headless; dots counted by `diagnostics/amd-dots-20261115/tools/dots.py`):

  | | 0.2.8 | first fix | both fixes |
  |---|---|---|---|
  | Shop: dark dots on Bayonetta | 1,269 | 45 | 57 |
  | Costume screen, 7 costumes | lines and speckles | lines and speckles | clean |
  | Arena floor grid, fight frame 5500 | yes | no | no |
  | Warm fight: frames over 33 ms | 20, 22 | 19, 19 | 22, 21 |
  | Warm fight: hitches of 50 ms or more | 15, 15 | 15, 14 | 14, 15 |

- **Played:** by the user on the RX 6700 XT (2026-11-16) and on an NVIDIA card (2026-11-17), no graphics bugs seen.
- **Not checked:** the cave floor from the user's screenshot.

## AMD: frame times and shader preparation (2026-11-14 to 15; 0.2.8)

After the move to the RX 6700 XT: preparation 111 s, pipeline waits 5x the 3080's, cached pipeline creation about
0.6 ms each against 0.1 ms. Changes: vertex stages are also prepared with depth clamp, libraries built by the workers
are kept by a SPIR-V hash, and depth clamp is dynamic state on cards other than NVIDIA. Checked with RGP: barriers and
the `GENERAL` layout cost nothing on AMD; the extra GPU time is AMD's clock policy at a 60 FPS cap.

| | 0.2.7 | 0.2.8 |
|---|---|---|
| New player: shader preparation | 102 s | 90 s |
| New player, first fight: over 33 ms, hitches, longest | 33, 21, 261 ms | 10, 5, 69 ms |
| New player, first fight: pipeline waits | 1,895 ms | 94 ms |
| Warm fight: over 33 ms | 19-20 | 9-10 |
| Warm fight: hitches; longest | 9-10; 87-92 ms | 4; 70-76 ms |
| Warm fight: pipeline waits | 492-510 ms | 36-52 ms |
| Warm fight: 1% low, worst window | 14.1-14.5 FPS | 18.2-19.6 FPS |
| Video memory peak | 3.0-3.1 GB | 3.0 GB |

| warm fight | RTX 3080 (0.2.6) | RX 6700 XT (0.2.8) |
|---|---|---|
| Shader preparation, new player | 26 s | 90 s (10 s with AMD's cache warm) |
| GPU time a frame, normal clocks | 5.5-5.9 ms | 10.2-11.6 ms |
| GPU time a frame, AMD held at 1825 MHz | | 5.2-6.2 ms |
| Mean 1% low of the fight's windows | 46.6-48.1 FPS | 43.7-46.1 FPS |
| Video memory peak (game's own count) | 3,013 MiB | 3,002 MiB |

The hitches left in a warm fight are loading screens (frames 949, about 1090, 4623): the game's own thread parsing
text in menus and transitions. Hitches on loading or black screens are left alone.

## Resolution steps, pipeline cache saves, keyboard and mouse (2026-11-12 to 13; 0.2.7)

- **Resolution steps** (`gpu/vk/nvn_res.c` `tex_get`): the 50-65 ms hitches came at the game's dynamic-resolution
  steps, which placed scene objects choose (not timing). The next frame built 9-27 scene targets of the new size from
  guest memory that held nothing drawn. Now a descriptor the replay has its own image for uses it
  (`SNX_NVN_TARGET_TEX=1` = the old way). Texture prefetch is on by default (`SNX_NVN_PREFETCH_MS`, about 700 MiB
  more video memory).
- **Pipeline cache** (`gpu/vk/pipeline.c` `vk_pipeline_save`): the 5 s save copied 613 MB out of the driver and held
  its lock about 100 ms. Now saved only when it grew, after 20 s without new pipelines, and at exit (header check and
  retry from upstream ScarletNX).
- **Keyboard and mouse** (`runtime/platform/backend/sdl3.c`, `[Keyboard and mouse]` in the INI, a launcher page):
  bindings, mouse camera, Esc opens the game's menu. Keyboard prompts (`gpu/vk/prompts.c`, family 4).

| fight, RTX 3080 | before | after |
|---|---|---|
| Frames over 33 ms | 19-22 | 13-14 |
| Resolution-step hitches (41-61 ms) | 3 in every run | 1 (45 ms) in 4 runs |
| Pipeline cache save | 613 MB every 5 s, 50-114 ms stalls | only when it grew, none |

Not done: the sticks' and D-pad's prompts still show the controller (done 2026-11-23, section above).

## Chapter 14 smear and crashes (2026-11-10 to 11; 0.2.6)

- **Smear while panning** (`b3ae4c4`): `dither=3` took the fade discard out of every fading pixel stage, but only
  pipelines with one colour target blend. The slab debris is drawn in a depth pre-pass, then a colour pass with depth
  EQUAL: the pre-pass wrote the faded chunk's whole depth, the ground behind failed EQUAL and kept last frame's colour.
  Passes that cannot blend or use depth EQUAL now get a twin "cut" module with the even-pattern discard
  (`NvnPipeDesc.fade_cut`). The user confirmed it gone.
- **Verse 7 movie crash** (nvmap handle reuse), Kraken slab, debris: fixed. Waits for the user's replay of that verse.
- **Gallery**: 91 ms a frame, 7.4 FPS 1% low, to 17 ms, 50 FPS. F6 captures.

## Mods, detail options, widescreen (2026-11-07 to 09; 0.2.5)

- **Mods** (`hle/filesystem/fs.c`, launcher Mods page; `diagnostics/mods-20261107/`): `mods/<name>/romfs/` replaces or
  adds files, `mods.txt` orders them, as yuzu and Ryujinx lay them out. Code patches too.
- **Detail** (ported from Witch Time; `diagnostics/detail-20261107/`): `texture_lod_bias` (`gpu/vk/texture.c`),
  `model_lod` (the game's level selector `main+0xbd563c`, `hle/model_lod.c`), `film_grain` (the grain constant read as
  0 in the four tonemap stages).
- **Widescreen** (`hle/aspect.c`, ported from Witch Time; `diagnostics/widescreen-20261107/`): the 3D view takes the
  window's shape (camera perspective `main+0xcba90c`, Vert+ below 16:9), the interface stays a 16:9 block in the
  middle, with its details moved (gauges, markers, capture effects). 16:9 / 21:9 / 32:9 on the RTX 3080: frames over
  33 ms 13 / 13 / 11-12, GPU load 38 / 39 / 44 %. Not done: at 32:9 a dialog's band and the character select's right
  edge stay short.

## Launcher window (2026-11-06; 0.2.0)

`B3-Express.exe` (`launcher/`): a web page in its own window, Install, Play, Update, Graphics, Controls and Mods
pages. Install takes an extracted dump (no extractor, never keys), compiles the game code and runs the shader
preparation pass.

## Frame pacing (2026-11-05; 0.1.7)

The pacer held the game at the frame's start, and the frame was shown after a varying amount of work, so a late frame
was followed by an early one (`diagnostics/pacing-20261105/`). A present thread now waits for each frame's 16.67 ms
slot. Recorded session: frames over 19 ms 326 to 177.

## Barriers in uniform control flow (issue 2, 2026-11-05; 0.1.7)

A `BAR` inside the translator's flow loop became a barrier in non-uniform control flow: Intel hung (device lost),
NVIDIA got through but rotated the dartboard room's colour grading. Now the barrier sits outside the loop with a phase
loop (`spirv_translate.c` `bar_phases`; `diagnostics/barrier-20261105/`). Released, not tested on Intel.

## Object fade, depth on AMD, compute in video memory, ASTC (2026-11-03 to 05; 0.1.1 to 0.1.6)

- **Object fade** (`gpu/vk/dither.c`, `dither=0..3`; `diagnostics/dither-20261104/`): 429 of the game's 3,755 pixel
  stages discard against a 64 x 64 pattern at `(FragCoord & 63)`. Off, even pattern, or blended (3, the default).
- **AMD depth** (issue 3, `54cf29f`): D24 depth AMD cannot render to falls back to D32.
- **Written storage buffers in video memory** (`nvn_res.c` `MIR_GPU`, ported from Witch Time;
  `diagnostics/vram-cb-20261103/`): the verse-exit drop was game compute reading guest memory across PCIe. Compute
  up to 24 ms a frame to 0.6 ms, 1% low about 30 to 46-48 FPS.
- **ASTC at area changes** (`gpu/vk/astc.c`; `diagnostics/scene-hitch-20261103/`, `lows-20261104/`): decodes batched
  across textures, and the top two levels of large textures spread over the next frames. The area-change frame 264 to
  101 ms.
- **Compute stages prepared** (0.1.5), fast renderer by default (0.1.4), launcher fix for non-English Windows (issue 1,
  0.1.2).

## Video memory and first-play freezes (2026-11-01; 0.1.0)

ASTC stored as BC3 encoded on the GPU, sparse pool copies, pipeline libraries, and a shader preparation pass before
play (`diagnostics/vram-20261101/`, `freezes-20261101/`; RTX 3080):

| fight | peak MiB | pipeline waits | worst frame |
|---|---|---|---|
| RGBA8 textures, whole pool copies | 8,819 | | |
| ASTC as BC3, sparse copies | 3,753 | | |
| First run, before shader preparation | | 29.5-32.7 s | 4,430-5,110 ms |
| First run after the preparation pass | | 0.51 s | 412 ms |

Cutscenes at 60 (`hle/scene60.c`, ported from Witch Time; `diagnostics/cutscene60-20261102/`): in-engine scenes 29.9
to 54.8 FPS.

## Against Witch Time (chapter 12, 2026-11-02; RTX 3080, hand-played, build f5)

| | Witch Time v0.4.5 | this port |
|---|---|---|
| Gameplay FPS | 59.1 | 55.6 |
| Combat at 1,300+ draws | 58.9 | 48.8 |
| Longest frame in play | 132 ms | 2,428 ms |
| Peak video memory | 5,834 MiB | 4,881 MiB |

Before 0.1.1. Not measured again on the current build (`diagnostics/chapter12-20261102/`).

## Releases

| version | date | what changed |
|---|---|---|
| 0.3.5 | 2026-11-19 | 90 FPS (experimental, launcher switch), replay on its own thread, steadier recording, cheats (issue 15), prompts by button map (issue 14) |
| 0.3.0 | 2026-11-16 | AMD dots, lines and floor grid fixed, game code compile cache (seconds after an update), compiler on every thread |
| 0.2.8 | 2026-11-15 | AMD slow frames, shorter shader preparation, one-button update, Graphics and Controls pages, keyboard prompts, GPU name on any card, safe save restore, issue 13 message |
| 0.2.7 | 2026-11-13 | keyboard and mouse, resolution steps without hitches, pipeline cache saved only when it grew |
| 0.2.6 | 2026-11-11 | chapter 14 fixes (verse 7 movie crash, Kraken slab, debris, fade smear), gallery speed, F6 captures |
| 0.2.5 | 2026-11-09 | mods (RomFS and code patches), detail options, widescreen stages 1-3, issue 10 |
| 0.2.0 | 2026-11-06 | new launcher window (`B3-Express.exe`) |
| 0.1.7 | 2026-11-06 | present thread (frames on even 16.7 ms slots), Intel barrier fix (issue 2) |
| 0.1.6 | 2026-11-05 | 32-bit depth on AMD (issue 3), no pool copies on integrated GPUs |
| 0.1.5 | 2026-11-04 | compute stages prepared, ASTC at area entry spread over frames, Save settings button |
| 0.1.2-0.1.4 | 2026-11-04 | launcher fix for non-English Windows (issue 1), object fade, fast renderer by default |
| 0.1.1 | 2026-11-03 | GPU-written buffers kept in video memory: no FPS drop entering or leaving a verse |
| 0.1.0 | 2026-11-02 | first public release |

## Open

| issue | state |
|---|---|
| FSR 3.1 | planned above, not started |
| Frame rates above 60 | 90 released in 0.3.5 (experimental); step 2's frame counters installed, not played by hand (sections above) |
| Replay on its own thread | measured 2026-11-17 (section above); needs hand play and AMD before a release |
| Chapter 14 verse 7 crash (nvmap handle reuse) | fixed 2026-11-10, waits for the user's replay of that verse |
| New AMD player's first fight end | three shaders compile at frames 5949-5950 (34-75 ms) |
| New AMD player's shader preparation, 104-113 s | AMD's compiler (section above) |
| Intel Iris Xe device lost (issue 2) | fix released, not tested on Intel |
| Vega iGPU (Ryzen 7 4800H) dots and lines on models (issue 17) | Same artifact as the RDNA2 dots, on 0.3.6, which has both SHFL fixes (55a53f3, a43b413): shop and equipment screens, drivers 2.0.213 to 2.0.279, render targets 0.5-0.9x. The same shop scene on 0.3.6 at 1.0x, 0.9x and 0.8x on the RTX 3080 (runs `scale-10`, `scale-09`, `scale-08`, exit 0, saves unchanged) is clean at all three: the scale doesn't cause it. Wave size ruled out too: the 6700 XT ran 69 of 81 pixel shaders at wave64 (`diagnostics/amd-rgp-20261115/fight-f3000.rgp`). Lead: fragment code takes a pixel's quad place from the subgroup lane number (S2R LANEID, which the costume shaders feed into SHFL's index; SHFL's host lane and live bit; FSWZADD). Witch Time PR 74 found AMD's Windows compiler can number lanes over live lanes only; Vega's driver has been on AMD's frozen maintenance branch since 23.9. Not confirmed: no Vega card, and the offline compiler (RGA 2026-10) only targets RDNA3+. Fix built (section above). The test exe (a654eb41) crashed on the reporter's Vega 6 times of 6, about 12 s in, at the title screen: exception c0000005 in a library on a runtime thread, the same offset (+1fd4 from a 64 KiB boundary) every run, a page-aligned address, then heap corruption (c0000374) at shutdown. 0.3.6 on the same machine passes that point and builds 31 pipelines there, so it is most likely AMD's Vega driver (Vulkan 1.3.260) crashing while it compiles a changed pixel stage. Test build 2 (release/B3-Express-0.3.6-fix-issue17-2.zip, runtime 87bf7e6e, 2026-11-22): same fix, and a crash in a library now names it and its offset (crash.c, GetModuleHandleExA), and on a runtime thread the shader module, prepared stage or pipeline the driver was making, by the stages' SPIR-V hashes (nvn_res.c `making`, `vk_nvn_crash_note`). Checked with a throwaway build that crashes in memset on a pipeline worker: `in ucrtbase.dll at +edf5d`, `a pipeline (vertex, fragment) f160bd24 0e9667c4` (run libcrash). The candidate on the shop route at 1.0x (run quad2-10): exit 0, saves unchanged, 0 failed pipelines, 1,887 dots vs 1,880 for the first test exe. Reporter's log from test build 2 (3 crashes of 3): `in amdvlk64.dll at +21d1fd4`, making the pipeline of vertex 4e003c72 and fragment afec9287, so AMD's Vega driver crashes compiling it. Hashes don't carry across GPUs (Vega declares DenormFlushToZero, and descriptor limits shape the bindings; patching the header alone matched 0 of 367). By position: on 0.3.6 the reporter's first pipeline after the "storage buffers" line was 4e003c72 with 36f9c3a1 (integer target, int rt 100). On the 3080 the same moment builds the fullscreen vertex 795f08ec with f5c3430d, an integer-target pass with 12 SHFL, 36 quad swaps and 12 ballots that already read FragCoord. The fix only changes its shuffle indices and lane numbers to come from FragCoord's low bits. It compiles for gfx11 offline, and there is no gfx9 compiler here. Test build 3 (release/B3-Express-0.3.6-fix-issue17-3.zip, runtime ea4ed742, 2026-11-22): `g_shader_quad_parts` (shader.h QP_*) splits where the quad place applies: the shuffle's host index, the lane SHFL picks its quad swap and range by, and LANEID/FSWZADD. `quad_test` in device.c (test only, not for a release) takes the next of 4 variants each launch from <name>.quadtest: 1 select+other (the shuffle's index as 0.3.6, the candidate), 2 index only, 3 select only, 4 other only; SNX_QUAD_PARTS=n overrides. Checked on the 3080: parts 7 gives test build 2's SPIR-V byte for byte (59 of 59 modules both runs made, f5c3430d among them); each variant on the shop route at 1.0x (runs var-6, var-1, var-2, var-4) exits 0, saves unchanged, no crash or failed pipeline, 1,884-1,919 dots vs 1,882 on 0.3.6; each variant's f5c3430d differs and compiles with amdllpc gfx1100 at wave 64 and 32; two runs without the override logged variants 1 then 2. Reporter's test build 3 results (2026-11-22, logs in the scratchpad's i17d): variant 1 crashed (`amdvlk64.dll +21d1fd4`, pipeline 4e003c72/6280c45e), variant 2 crashed (4e003c72/d1c239c9), variant 3 reached the title with the dots still on the train and models; variant 4 not run, and start 3's log wasn't in the zip. In variants 1 and 2 the host shuffle's lane depends on FragCoord (1 through LANEID, which the game feeds into SHFL's index; 2 directly); in 3 it doesn't, and 3's SHFL code is the same as 1's. So the driver crashes when a shuffle's lane comes from FragCoord. Variant 3 picks quad swaps by FragCoord and changes nothing on screen, which fits GCN packing pixel quads into lanes 4i..4i+3: the lane-compaction lead doesn't hold on Vega. Reverted 2026-11-22: spirv_translate.c and shader.h are back to 0.3.6, and quad_test is gone from device.c. Kept: the crash report naming the library and the pipeline (crash.c, nvn_res.c), and the `vulkan: subgroups of` line. Shop route at 1.0x (run rev-10, runtime 62bbb3be): exit 0, saves unchanged, no crash, 1,878 dots vs 1,882 on 0.3.6. The reporter plays with object fade 3 (blend), the default, in every log but three from 2026-11-21 (0 there): asking them to try object fade Game on 0.3.6 would tell whether dither.c's blended fades are involved. Their answer (2026-11-22, scratchpad i17e): the picture is the same in every setting, and the log has a 0.3.6 session with `object fade 0 (game)` (154 s, normal exit), where dither.c changes no stage. So the fade rewrite isn't the cause. Their launcher-errors.log also shows the web launcher failing on every start since 2026-11-20 (pythonnet: `Failed to resolve Python.Runtime.Loader.Initialize`), so they get the old window. Open, no lead, needs Vega hardware |
| RX 6750 XT device lost in the prologue (issue 16) | GPU markers built (section above). Log of 2026-11-21 is from 0.3.0 (no markers): lost at 134.5 s, 3.3 s after a cutscene hands back control (frame 6531), nothing slow logged before it. Needs a release with the markers and a run with `SNX_GPU_MARKERS=1` |
| Widescreen 32:9 | a dialog's band and the character select's right edge stay short |
| Chapter select nodes flicker | seen once on 2026-11-06, not reproduced |

## Not tested

- Other chapters on the current build; Intel GPUs; a weaker card.
- A dump extracted by another tool.
- A real update with the launcher's window: its steps were run headless on 2026-11-22 (section above).
- BC3 up close on faces and metal; FSR 1 in heavy scenes.

## Game and source

- Title ID `01004A4010FEA000`, version 1.2.0, main build ID `B47CC79B88B7091EA514B2B725C50AE01C6F00DF`.
- ScarletNX base commit `5c97949`. No game files, saves or compiled game code ship with a release.
