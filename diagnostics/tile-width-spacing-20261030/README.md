# Shifted UI and menu backgrounds: the texture header's tile width spacing (2026-10-30)

Supersedes `../astc-zero-20261030/` (its zero-block workaround was built on a wrong reading and is removed).

## Symptom

The user's play tests (`screenshot-for-debug/`, compared with Citron): magenta or wrongly colored rectangles on 2D
UI (HUD portrait, menu backgrounds, the verse-result banner), a white box in the Skills menu, missing faces in the
menu circles. The user noticed the backgrounds looked shifted to the right.

## Mechanism

- Every wrong rectangle came from all-zero ASTC blocks: painting them opaque green (run `range-port-menu-green1`)
  turned exactly those rectangles green (`equipment-before-zero-blocks-green.png`, against `equipment-before.png`).
- Those zeros were not in the game's data. The guest memory of the three affected UI atlases (ASTC 6x6, 1026x2052,
  1026x1026, 2052x2052) never changed (`range-port-menu-watch1`, counted every 300 presents), and the decoded images
  held more zero blocks than the data could (5,774 decoded, at most 2,178 possible in the 1026x2052 one, if its
  padding was zero): the untiling read the wrong bytes.
- Maxwell's texture header word 3 bits 10-12 are the **tile width spacing**. The runtime ignored them. With spacing
  s, a block-linear level's rows are aligned to 64 << s bytes (unless the level is smaller than such a GOB). Eden:
  `StrideAlignment` and `NumGobs` in `video_core/texture_cache/util.cpp`. A 1026-texel ASTC 6x6 row is 171 blocks
  (2,736 bytes); with the spacing the game sets it is 176 (2,816). Read at 171, every row started further off, so
  the sprites came out shifted and the padding between rows appeared as zero blocks, which the decoder draws as its
  error color (magenta), and after the zero-block workaround as transparent rectangles over the wrong background.
  A logged header of another texture (512x1024) has word 3 `00071018`: spacing 4.

## Fix

`gpu/vk/texture.c`: `bl_row_bytes` (Eden's rule, credited in `docs/CREDITS.md`), used by `bl_level_size` (so texture
sizes, layer strides and level offsets include it) and by `level_rows` (the untiling). The spacing is kept per
texture (`Surf.tws`, compared when a texture is looked up). Render targets keep spacing 0. `astc.c` is back to its
original state (the zero-block workaround and all diagnostics removed); the decoder still shows real error blocks as
magenta, which is how this bug was visible.

## Verification

- `-Wall -Wextra` clean on the changed files (one older warning at `texture.c` 736, not this change); the ASTC
  shader header check passes; 8 targeted tests pass.
- `range-port-menu-tws1` (diagnostics still in): **0 zero blocks in any uploaded ASTC texture** (was 5,774, 2,183
  and 11,548), Equipment and Items menus match Citron: `equipment-fixed.png`.
- `range-port-menu-tws2` (the final build, chapter route plus the menu): exit 0, saves unchanged, 0 magenta pixels
  in all 11 screenshots. The scripted presses did not reach the Demons and Skills pages, so the faces and the
  white box are for the user's play test.
- Build: `Bayo3_120.exe` here (`e24658fe...`). Installed 2026-10-30 at the user's request (rollback in `preserved-installed/`, record `deployment.json`).
