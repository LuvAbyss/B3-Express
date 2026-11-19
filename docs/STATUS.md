# Bayonetta 3 PC port status

Bayonetta 3 v1.2.0 on Windows. ScarletNX recompiles the game's ARM64 code to a native DLL (`Bayo3_120_code.dll`), and
an NVN-level renderer on Vulkan (`source/src/nx/gpu/vk/nvn_res.c`) draws it. The launcher (`B3-Express.exe`) installs
from the player's own extracted dump, compiles the game code and prepares the shaders. Goal: a smooth 60 FPS and a
launcher anyone can use. Undergraduate graduate project, started 2026-10-19.

Test PC: Ryzen 7 5700X3D with an RTX 3080, except 2026-11-14 to 16 with an RX 6700 XT (sections say which). Numbers come from that PC at `resolution=1.6`
(2560x1440 from the game's 1600x900) unless noted. Unattended runs use the scripted fight route
(`diagnostics/nvn-phase5-20261030/route-remnant9.txt`) in the isolated copy (`diagnostics/desktop-launch-20261028/isolated`),
never the user's save. The notes behind each section are in the `diagnostics/*/README.md` it names.

## Now (2026-11-17)

| | |
|---|---|
| Latest release | 0.3.0 (2026-11-16), packaged and pushed (`5c8c9d2`); the user publishes it on GitHub |
| Installed in `output/` | The render thread build (runtime `0b969bfb`, game code `f13c034c`), 2026-11-17. Rollback to 0.2.8 with the AMD fixes: `diagnostics/install-20261117/` |
| Played | the AMD fixes on the RX 6700 XT (2026-11-16) and on an NVIDIA card (2026-11-17): no graphics bugs seen |
| Stability | 0.3.0: 8 fight routes and a preparation pass on the RTX 3080, no crash (section below) |
| In progress | The replay on its own thread: built, measured (section below) and installed; not committed, not played. Prompts that follow the button map (issue 14): built, checked in the isolated copy (Xbox, PlayStation, Switch) and installed 2026-11-18 (`diagnostics/install-20261118/`); not committed, not played. Cheats in Atmosphère's format (issue 15): built, tested in the isolated copy, installed 2026-11-18 (`diagnostics/install-20261118b/`); not committed, not played. 90 FPS step 1 (`frame_rate`): built, runs at 90 in real time in the isolated copy, installed 2026-11-18 with `frame_rate=90` (`diagnostics/install-20261118c/`); not committed, not played |
| Next | 90 FPS step 2, the game's frame counters; FSR 3.1: plan below, not started |

## 90 FPS, step 1: clock floor and pacing (2026-11-18; installed, not released)

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

## Cheats in Atmosphère's format (issue 15, 2026-11-18; installed, not released)

A player asked for cheats. Switch cheats are Atmosphère cheat files (`cheats/<build id>.txt`): small programs the
console's `dmnt` runs beside the game, writing memory. The ones shared for our build (`B47CC79B88B7091E`) are 17,
and 11 of them write ARM64 instructions into the game's code, which does nothing to a recompiled game: its code is
compiled already. So cheats go two ways.

- **The change** (not committed):
  - `source/src/nx/loader/cheat.c`, `cheat.h` (new, C90, in `snx_patch` with `patch.c`): the file's parser (`[Name]`
    cheats, `{Name}` master codes, the ones on named in `cheats/enabled.txt`), the machine itself with every
    instruction of Atmosphère's `cheats.md` (writes, loads, conditions, loops, arithmetic, held keys, static registers,
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

## Prompts follow the button map (issue 14, 2026-11-17; installed 2026-11-18, not released)

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

## The replay on its own thread (2026-11-17; not released)

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

Not done: the sticks' and D-pad's prompts still show the controller.

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
| Frame rates above 60 | studied 2026-11-17 (`diagnostics/fps120-20261117/`); decided to stay at 60 |
| Replay on its own thread | measured 2026-11-17 (section above); needs hand play and AMD before a release |
| Chapter 14 verse 7 crash (nvmap handle reuse) | fixed 2026-11-10, waits for the user's replay of that verse |
| New AMD player's first fight end | three shaders compile at frames 5949-5950 (34-75 ms) |
| New AMD player's shader preparation, 104-113 s | AMD's compiler (section above) |
| Intel Iris Xe device lost (issue 2) | fix released, not tested on Intel |
| Widescreen 32:9 | a dialog's band and the character select's right edge stay short |
| Keyboard prompts | sticks and D-pad still show the controller's |
| Chapter select nodes flicker | seen once on 2026-11-06, not reproduced |

## Not tested

- Other chapters on the current build; Intel GPUs; a weaker card.
- A dump extracted by another tool.
- A real update from one release to the next with the launcher's Update button.
- BC3 up close on faces and metal; FSR 1 in heavy scenes.

## Game and source

- Title ID `01004A4010FEA000`, version 1.2.0, main build ID `B47CC79B88B7091EA514B2B725C50AE01C6F00DF`.
- ScarletNX base commit `5c97949`. No game files, saves or compiled game code ship with a release.
