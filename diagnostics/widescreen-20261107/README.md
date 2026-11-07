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

## Next

1. Find the camera's projection: the function writing the projection matrix the 3D passes read (a uniform buffer
   with 1/tan terms). Its x scale is where 16:9 enters. Candidates: the other 16/9 sites (`0xc30668`, `0xc81288`,
   `0xc826e8`, `0xca8dd4`, `0xca921c`), disassembled with `tools/disasm.py`.
2. The game's own window and scene targets (1920x1080 window, 1600x900 top dynamic step): either the game is patched
   to make them wider, or the runtime widens the screen-sized targets on x only, with viewports, scissors and
   pixel-space shaders (FSR, post effects) following.
3. The HUD: keep its 1280x720 canvas 16:9 and centered (pillarboxed HUD), the usual ultrawide choice, with the
   `0xc2abe8` mapping widened so markers stay on their targets.
4. An `aspect` option (16:9, 16:10, 21:9, auto from the window) and the launcher setting.

Tools: `scarletnx --dump-modules <data dir> <out>` needs `libLLVM-23.dll` beside it (the game folder's exe works);
`tools/disasm.py` with `SNX_SCARLETNX` set to it and `deps/llvm-sdk/clang64/bin` on PATH.
