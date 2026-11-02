**Superseded:** the zero blocks were a misread layout, not game data. See `../tile-width-spacing-20261030/README.md`.

# Magenta UI rectangles: all-zero ASTC blocks (2026-10-30)

The user's play test of the installed chunked-submit build (`screenshot-for-debug/`, 4 screenshots) showed solid
magenta rectangles on 2D UI: the HUD portrait and gauge, menu panels, the verse-result banner. The 3D scene was right.

## Mechanism

- Magenta `0xFFFF00FF` is only made in one place: `gpu/texture/astc.comp`'s `ERROR_COLOR`, the ASTC decoder's color
  for error blocks (the specification's LDR error color). Eden's decoder uses transparent black.
- A diagnostic in `vk_astc_upload` (runs `range-port-astc-diag1`, `-diag2`, chapter route) sorted every uploaded
  ASTC texture's blocks by what `IsError` rejects. Only two textures had any, both UI atlases (6x6 blocks):
  `5a378b000` 1026x2052, 5,774 of 58,482 blocks; `5a287c000` 1026x1026, 2,183 of 29,241. **Every rejected block was
  all zeros**; no illegal modes, no HDR. The counts were identical in both runs, so the zeros are the game's data
  (the empty parts of the sprites), not memory read before it was written.
- An all-zero block has block mode 0, reserved, so the decoder filled it with magenta, and each sprite's empty area
  became a solid rectangle. The console draws them as transparent, as Eden does.
- A first guess (LDR void extents with unusual coordinates, also made magenta by a check Eden doesn't have) was tested
  in `range-port-astc-void1`: no such block exists in this game. That change was removed.

## Fix

`vk_astc_upload` (`gpu/vk/astc.c`, `zero_blocks_transparent`): each all-zero block is staged as an LDR void extent of
0, 0, 0, 0 (coordinates all ones), which `astc.comp` decodes to transparent black. Guest memory is not changed; the
source blocks are read, the staging copy written. The cleaner fix is `ERROR_COLOR = 0` in `astc.comp`, but its SPIR-V
is a checked-in header (`astc_spv.h`) that needs glslang 16.6.0 to rebuild, and running a downloaded glslang was not
allowed in this session.

## Verification

- Builds with no warnings (`-Wall -Wextra`); 8 targeted tests pass.
- Runs `range-port-astc-zero1` to `-zero4` (chapter route, all exit 0, saves unchanged): 0 magenta pixels in every
  periodic and captured frame. The earlier HUD frames (`range-port-replay33`, `-astc-void1` at frame 4000) had 2,678
  magenta pixels each, but none of the fixed runs' captures happened to show the HUD (the route's timing differs run
  to run), so the HUD itself has not been seen fixed yet: the user's play test is the check.
- Candidate: `Bayo3_120.exe` here (`f3ffd709...`): today's full source: the installed build's lineage plus this fix, the
  phase 3 NVN code (off unless `SNX_NVN_*` is set) and the shader translator's SAM/RAM and FSWZADD support (two
  fragment shaders that were skipped now draw: e370, see `nvn-port-20261029/README.md` part 2b). **Installed 2026-10-30 at the user's request** (rollback in `preserved-installed/`).
