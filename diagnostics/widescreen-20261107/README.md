# Widescreen 16:10 and 21:9: first look (2026-11-07)

The user chose real widescreen (the 3D view wider, not stretched or with black bars). Today a 16:10 or 21:9 window
gets the 16:9 picture letterboxed (`gpu/vk/renderer.c`), as B2recomp does; Witch Time has nothing on aspect ratios.

## Found

- `main` (build `B47CC79B88B7091EA514B2B725C50AE01C6F00DF`, v1.2.0) never stores 16/9 as data. Code builds it in a
  register (`mov w8,#0x8e39; movk w8,#0x3fe3,lsl#16`) at 8 places: `main+0x706b70`, `0x774b4c`, `0xc2abe8`,
  `0xc30668`, `0xc81288`, `0xc826e8`, `0xca8dd4`, `0xca921c` (scan: scratchpad `scan169.py` over the
  `--dump-modules` image).
- `main+0x706b70` is a function returning 16/9. Its one caller (`main+0xc9cb28`) sizes a dynamic-resolution buffer
  from a pixel count: height = sqrt(area / aspect), width = height x aspect, packed w | h << 16. It sits behind
  flag bit 12 of `[x19+0xb2c]`.
- `main+0xc2abe8`: height = 2 x tan(fov/2) x distance, width = height x 16/9, then 720 / height and 1280 / width: the
  mapping from 3D onto the 1280x720 HUD canvas (lock-on markers and the like). It has to change with the camera.
- 1280.0 and 720.0 are built in code at about 25 places each, clustered in `main+0xc15b78`..`0xc19ea4`: the HUD's
  canvas.
- The NVN renderer scales every 16:9-shaped target (w/h 1.70 to 1.80) by `resolution`, the same on both axes
  (`nvn_res.c` `rp_scale_of`).

## Ruled out

- **x1** (`x1-display-width.exe`, `x1-display-width.diff`, reverted in the source): am GetDefaultDisplayResolution
  and vi's display mode, logical resolution and display list report 2560x1080 with `SNX_TEST_DISPLAY_W=2560`. The
  game ignored it: the same 1600x896 targets and 1600x900 frame as without (`desktop-launch-20261028/wide-x1-2560`
  against `mods-m1-off`).
- **p1**: a code-patch mod making the getter at `main+0x706b70` return 21/9 (`00706B70 A8AA8A52`, `00706B74
  A802A872`, compiled in with the new code-patch mods): applied ("8 bytes patched"), but the title's targets were
  the same sizes (`wide-p1-21x9`). That path isn't the title's, and probably not gameplay's either.

## The camera (2026-11-07, second pass)

- The projection, from `SNX_NVN_DRAWLOG`'s `C,` lines (the first depth-writing scene draw's vertex uniform buffer 0:
  a view matrix, then the projection, row-major) at the title: P[0][0] 1.31241, P[1][1] 2.33317 (ratio 1.7778, so
  16:9), vertical fov about 46 degrees, P[3][2] -1. Run `desktop-launch-20261028/cam-title-a`.
- The 37 callers of `tanf` (`main+0x1321480`) with divisions after them are the camera's zoom limits and audio
  biquad filters (pi, sqrt 2), not the projection: it is built without `tanf`, likely by the SDK's inline matrix
  helpers.
- The game knows a screen size: `main+0xd02d70` returns the width and `main+0xd02d80` the height, both read from the
  object at `*(0xa4b1cc0)` (+0x130, +0x134). Three places correct by (16/9) / (w/h): `main+0xca8dd4` and `0xca921c`
  (the screen getters, stored at `[x20+0x38]`) and `0xc826e8` (a render target's own size). So the game was written
  for screens of other shapes as long as that size says so.

- **p2** (code patch: the width getter returns 2560): the projection stays exactly 16:9 in every title shot, so the
  screen size isn't where the camera's aspect comes from (`wide-p2-w2560`; the title picks different shots per run,
  so fov can't be compared between runs, only the ratio).
- **The scene's size is set in the graphics setup at `main+0x69c698`:** 1600x896 docked or 1280x720 (chosen by
  `bl 0x8ce8f10(0x20000000)`), passed to `0x86a2090(1600, 900, 1600, 900 or 1024)`, `0x86a2344(.., 1600, 900)` and
  `0x86a2e80(1600, 896, w, h)`. `0x86a2090` builds a settings block (pool sizes such as 0x10400000, a 1280x720
  default, a 1024 limit) for `0x86a4c44`.
- **p3** (1600 -> 2096 there) and **p4** (896/900 -> 688 there, 1600x688): both crash 1.2 s in, at the same place
  (`main+0x6705ac`, a null pointer from an init call, under nnMain), with memory_gb 0 or 6. So it isn't memory:
  something down `0x86a2090`'s chain refuses any size but its own (`wide-p3-t2096`, `wide-p3-t2096-m6`,
  `wide-p4-h688`). p4 did make a 1600x688 target before failing.

## Decision: port Witch Time's (2026-11-07, the user)

Witch Time v0.4.5 (GPL-3.0, `E:\download\WitchTime-v0.4.5-windows-x86_64`) has experimental widescreen, checked in
gameplay at 21:9, 32:9 and 9:10: `docs/STATUS.md` "Aspect ratios", `runtime/game_shim_eh.c` from line 535,
`runtime/nvn_vk_draw.c` `rs_xstretch`. Their addresses are `0x7100000000` + main's offset (ours `0x8005000` +).
It explains p3: the graphics setup's render-target heap (config + 0x90, 0x10400000) is sized for 16:9, and at 21:9
the 84th target didn't fit.

Port in stages, each tested in the isolated copy at 21:9 on the title and the fight route:

1. **The 3D view, HUD centred** (`hud_area = 16:9`): screen size getters `main+0x70a740` / `0x70a760` (width =
   height x ratio, even), heap at `main+0xd01380` (+ (ratio x 9/16 - 1) of it, 2 MB aligned, and the memory the game
   is told grows as much), camera perspective `main+0xcba90c` (a 16:9 width / height gets the screen's shape) and
   fov `main+0xcba97c` (narrower than 16:9 only), the UI's flat projection `main+0xd07610` (the 1280x720 in the
   middle, more around it). Renderer: 16:9-class scene targets widened on x by ratio x 9/16 (our `rp_scale_of`
   becomes per axis, with the FragCoord scale the translator already takes per axis), the screen filters' viewport,
   movies pillarboxed, the dither at the image's pixels. All addresses into `code-tablecalls.txt`.
2. **The interface's details:** clip rectangles (`0xd02d90`, `0xd02db0`, `0xd04414`), capture effects (`0xc025a4`,
   `0xc00b98`), elements placed over 3D points (`0xc1656c`, `0xbfcf70`, `0xbfcf80`, `0xa6f5a0`), the screen width
   for aim, arrows and picks (`0xd02d70`, about 30 call sites by return address).
3. **HUD at the screen's edges** (`hud_area = screen`): `0xc0ef04`, the layout's roots moved out by half the extra
   width, with their exceptions (rules, Game Over, event0015).
4. **Options:** `aspect_ratio` (16:9, auto from the window, 21:9, 32:9, 16:10) and `hud_area` in the `.ini` and the
   launcher.

## Stage 1 done (2026-11-07): the 3D view, HUD centred

`aspect_ratio=` (width / height, 0 the game's 16:9) in the `.ini`, and an Aspect ratio row in the launcher's Options
(16:9, 16:10, 21:9, 32:9, marked experimental). `source/src/nx/hle/aspect.c` hooks the six functions of stage 1
(addresses in `launcher/code-tablecalls.txt`), checking their words first; `mem_total` grows with the heap;
`nvn_res.c` `rp_xstretch` widens 16:9-class scene targets on x. Build w1 (`w1/Bayo3_120.exe`, `b530a720...`).

Runs in the isolated copy (`desktop-launch-20261028/ws-*`), game code compiled with the new list, copy restored after
each, every one exit 0 with saves unchanged:

| Run | Frame | Camera P11/P00 | Seen |
|---|---|---|---|
| `ws-21x9-title` (2.333) | 2100x900 | | title 3D across the width, text centred and unstretched |
| `ws-21x9-fight` (2.333, 1a: stretched) | 2100x900 | 2.3333 (56 of 56) | Giza: Bayonetta the same size as at 16:9, more of the arena at the sides; area title centred |
| `ws-21x9-fight-b` (2.333, 1b: widened) | 2100x900 | | the same, sharper: fine detail +7 to +40 % against 1a on the same frames, no faults seen |
| `ws-16x10-fight` (1.6) | 1440x900 | 1.6000 (60 of 60) | Giza, Vert+, title centred |
| `ws-32x9-title` (3.556) | 3200x900 | | heap 0x20a00000, title 3D across, text centred |

Not right yet (stages 2-3): the chapter select's bottom bar and panels end inside the screen; elements placed over
3D points, clipped gauges and the aim aren't moved; movies aren't pillarboxed. Not tried: a real ultrawide monitor,
cutscenes, the pause menu.

## Next (before the decision)

1. With the GDB stub (`runtime/gdbstub.c`) instead of a 3-minute compile per guess: break on the returns of the
   calls `main+0x69c698` makes, with p4 on, and find which one fails for 1600x688, then what it checks. Then the
   same for the projection: watch the camera's uniform data (`C,` lines) back to the code that writes P[0][0].
2. The game's own window and scene targets (1920x1080 window, 1600x900 top dynamic step): either the game is patched
   to make them wider, or the runtime widens the screen-sized targets on x only, with viewports, scissors and
   pixel-space shaders (FSR, post effects) following.
3. The HUD: keep its 1280x720 canvas 16:9 and centered (pillarboxed HUD), the usual ultrawide choice, with the
   `0xc2abe8` mapping widened so markers stay on their targets.
4. An `aspect` option (16:9, 16:10, 21:9, auto from the window) and the launcher setting.

Tools: `scarletnx --dump-modules <data dir> <out>` needs `libLLVM-23.dll` beside it (the game folder's exe works);
`tools/disasm.py` with `SNX_SCARLETNX` set to it and `deps/llvm-sdk/clang64/bin` on PATH.
