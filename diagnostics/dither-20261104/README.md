# The game's dither (2026-11-04)

The user asked whether Bayonetta 3's checkerboard dither can be removed or switched in the launcher, and what removing
it would change. They chose to investigate first, then asked for all three variants to test: see "Built" below.

## What it is

The game fades objects with a screen-door pattern instead of blending: the pixel stage fetches a texel of a **64 x 64
pattern texture** at `(int(FragCoord.x) & 63, int(FragCoord.y) & 63)` and discards the pixel depending on its value.
Witch Time describes the same thing (objects near the camera fading, "the faded demon in Shibuya"; their
`nvn_vk_draw.c` `g_fs_small`, textures of 64 x 64 or less).

## Found offline, in all of the game's pixel programs

Corpus `diagnostics/nvn-port-20261029/corpus-nvn-7` (3,755 pixel stages), translated with
`source/build/desktop-isolated/snx-shaderc.exe <corpus> --out <dir>` (5 s), then scanned in SPIR-V.

| | stages |
|---|---|
| pixel stages | 3,755 |
| read screen x/y | 3,238 |
| fetch a texel at `(screen position & 63)` | 435 |
| ... and a discard's branch depends on that texel | **429** |

Mask constants seen on those positions: 63 (870 uses, x and y), a handful of others (0x80000000, 1023: not the pattern).
The other ~3,000 screen-position reads are `position x (1 / screen size)` samples of screen-sized buffers (lighting,
shadows), not the dither.

Scripts here: `scan.py` (taint from FragCoord.xy to sample coordinates to a discard, in program order: registers are
reused, so a store replaces what a register held; order-free taint matched 86% of stages, wrongly), `slice.py`
(what each screen read's coordinate is made of, counted as signatures), `confirm.py` (the pattern fetch, its mask, and
whether the discard depends on it). Run with the bundled Python, `-I`, on the `--out` directory.

## Why it can look worse here than on the Switch

- **Smooth 60** draws the scene at 1600 x 900 and FSR 1 upscales and sharpens it: the per-pixel pattern is enlarged
  and sharpened with it.
- At a scale that isn't a whole number (Balanced, Sharp at 1440p) FragCoord is given in the game's pixels
  (`shader.h` `host_scale`), so neighbouring image pixels read the pattern unevenly: Witch Time saw a coarser grid of
  crosses there and fixed it by giving such draws the image's own pixels.

## Ways to remove or soften it (not built)

1. **Swap the pattern texture** at bind time for a constant one: no shader or pipeline changes, no new shader
   preparation, no performance cost. Fading objects then switch from shown to hidden at one fade value: objects near
   the camera stay solid and can block the view; fades pop.
2. **Patch the 429 programs** in translation (skip the discard when a switch says so): the same look as 1, more code.
3. **Even pattern** (Witch Time's fix): pattern at the output image's pixels. Fixes the crosses at uneven scales,
   keeps fades, doesn't help Smooth 60's FSR enlargement.
4. **Real transparency**: blend instead of discard. Smooth fades, most work, order errors possible.

## Not known yet

- Which objects use the 429 programs in play, and the texture's contents (a checkerboard, Bayer or noise pattern):
  needs a run that logs the bound 64 x 64 texture on those draws.
- Answered (`path.py`): the discard happens when the object's alpha (vertex alpha x material alpha x a constant
  buffer value) is below the pattern texel, so alpha 1 keeps every pixel.
- The 6 stages that fetch the pattern but don't discard on it.

## Built: dither=0..3 (build d1, `953aef25...`, not installed)

The user asked to test all three variants. `gpu/vk/dither.c` patches the translated SPIR-V of a pixel stage (called from
`nvn_res.c` `stage_spirv`), setting `dither` in the INI (in-game menu "Object fade", launcher Graphics page "Object
fade"): 0 the game's, 1 off (the discard's branch never taken), 2 even (the pattern's `& 63` input recomputed from raw
FragCoord, the image's own pixels), 3 transparent (no discard, alpha clamped to 0..1 written as a second blend source at
location 0 index 1, and `pipe_key` blends target 0 by SRC1_ALPHA where the game draws it unblended into one target).
Without dual-source blending, 3 falls back to 1. `SNX_DITHER_FADE=a` (test only) forces every fade's alpha to a.
Shader preparation is keyed by `scaling + 2 x dither` (`prep_mode`, launcher `prep_mode`), so 0 keeps old caches.

The recognition (`recog.py` mirrors it): pattern fetch at `(FragCoord & 63)`; a `<`/`>` compare of its texel with an
alpha such that the discard happens when alpha < pattern; the result reaching a branch to a discard block through
LogicalAnd, Bitcast and register stores only (FSETP's complement predicate is ignored). Corpus: 407 stages with one
such compare and 22 with two, 429 stages, 451 compares, the same in Python and C (`harness.c` in the session
scratchpad). Every patched module of every mode passes `spirv-val --target-env vulkan1.3` (SPIRV-Tools v2026.4,
extracted from the package in `diagnostics/spirv-tools/` into the scratchpad).

### Runs (isolated copy, 2560x1440 native, `run_fade.py`)

- Chapter select room, all modes: exit 0, no failed pipelines, saves unchanged. Only one fade stage is drawn there.
- Fight route (`route-remnant9.txt`), six runs: exit 0, no failed pipelines, 5 to 9 of 600 sampled frames over 33.4 ms in
  every mode. Combat drifts between runs (two runs died before frame 6800), so frames are compared by content.
- Frame 6700, a sphere demon with a fading figure behind it: the game's natural fade shows a checkerboard with small
  crosses (the uneven-scale effect Witch Time describes); with dither=3 the same kind of fading figure is smoothly
  see-through. With every alpha forced to 0.5: game shows a coarse checkerboard over the whole demon, off draws it
  solid, even draws a much finer pattern that reads as see-through, transparent shows no pattern (the demon blends
  over its own back faces, so it stays dark red).

### Not known yet

- How the user judges them in play. Transparent's known risks: objects drawn after a faded one and behind it are
  hidden by its depth; a fading object that the game draws with its own blending or into several targets stays
  without a pattern and is drawn solid.
- Smooth 60 (FSR from 1600x900): even looks the same as game there by design.

### Cost A/B (2026-11-04, after the user felt a worse 1% low on d1)

Fight route, isolated copy, 2560x1440 native, frames 0-6000, two runs per mode, no forced alpha: frames over 33.4 ms
game 517 and 502 of 5999, off 505 and 492, transparent 511 and 496. The 1% low per 10 s window matches window for
window; the one combat hitch window ranges 27 to 40 FPS in every mode. No cost measurable; run-to-run spread is larger
than mode-to-mode. Game mode runs exactly a2's code (dither_stage returns at once). The user's own d1 sessions (Off
256 s, 1.58% over 33 ms; Transparent 188 s, 2.02%; a2 yesterday 343 s, 1.18%) were different scenes and lengths, and
the Transparent one includes an area load decoding 277 Mpx of ASTC (a 27.8 FPS window).
