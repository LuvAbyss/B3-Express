# Issue 18: "Broken Fog and Dithering" (GTX 1660), study of 2026-11-26

Reporter: launcher 0.4.0, Windows 11, Ryzen 5700X, GTX 1660, driver 610.88. Two pictures of a Tokyo temple roof
(dark 8-10 px squares, some with an X inside, scattered over the sky; blocky teal smoke), and an older one
(2026-11-11) of the butterfly wings with a visible checker. "No settings affect how the dithering is rendered."
Log in the session scratchpad (`i18/session.log`). No fix made. The user paused the game runs: they will test by hand.

## What the reporter's log says

- `object fade 2 (even)` and `prepared_98960000_5.bin` (scaling 1 + 2 x 2): their setting is applied.
- `render targets at 1.60x`, swapchain 1280x720 (the same as ours), pictures 2559x1439: the 720p output is shown
  twice its size on their screen, so one game pixel is about 2 screen pixels.
- `texture LOD bias -0.50`: the player's texture sharpness option (`renderer.c:72`), not the GPU.
- The only lines our 3080 log lacks: 8 `no texture from TIC 00000000...` (empty slots), at 46 s.

## Why no fade setting changes it (code)

`dither.c` changes a pixel stage only when it finds the 64 x 64 pattern fetched at `FragCoord & 63`, compared with an
alpha, and the result reaching a branch to a discard (`nmatch > 0`). Any other use of the pattern is left as the game
has it in all four modes. The 2026-11-04 survey found 435 stages fetching the pattern, 429 discarding on it: 6
unexplained. At a scale our translator gives FragCoord in the game's pixels (`ShaderInfo.host_scale`), so in such
stages the pattern stays one game pixel wide.

## Witch Time (github.com/Zexyen/Witch-Time)

- **#126 / PR 191** (merged 2026-11-12, `fd1e52a9`): fog walls fade through the same 64 x 64 pattern but also read the
  mist's layers, so their own-pixels rule skipped them: dots of one game pixel. Fix in `mx_translate.c` (`mx_tf`): a
  fetch at exactly the game's pixel position masked by a power-of-two size takes the image's pixel. Second part,
  `nvn_vk_draw.c`: the mist faded against a half-size depth kept at the game's size, so its cut-out around
  characters and buildings stepped in 2 game pixels. They now sample the scaled linear depth with a min-reduction
  sampler (`WT_VK_MISTDEPTH=0` off). Their reading: the game's own look enlarged, not a GPU bug.
- **#207 / PR 219** (2026-11-15): (a) the cloud near the player is tessellated patches whose evaluation stages take
  noise through SHFL from "the first lane of the group"; on the console a vertex is its own group. Run as a subgroup
  shuffle, vertices took a neighbour's noise and the patches split, on AMD and NVIDIA. Fix: tessellation stages as
  one lane. (b) the cloud is cut out in its own 520x360 pass (2.8 scene pixels a texel); above scale 1.0 that pass is
  now drawn in the scene's pixels (`rs_extra_scale`), 7.7x its pixels.
- **#258**: they keep the fade (no off switch); temporal AA blends it.
- Ours: a SHFL in a vertex, tessellation or geometry stage turns on real subgroup shuffles
  (`spirv_translate.c:2993`, `lane_model` at `:3014`), the same as Witch Time before PR 219. The reporter's log
  loads tessellation stages. Our map runs bind 520x360 images too. Neither checked against the reporter's scene.

## GTX 1660 (TU116, Turing)

- Compute capability 7.5, 22 SMs, 32 warps per SM (3080: 8.6, 68, 48). No RT or tensor cores, separate FP16 units,
  FP32 and INT32 at once. Subgroups of 32, the same as Ampere.
- gpuinfo.org compare, report 52431 (GTX 1660, 610.88, the reporter's driver) with 52986 (RTX 3080, 621.1): no core
  feature or image format differs. Every differing feature is an extension the renderer doesn't use (ray tracing,
  cooperative matrix, micromaps, device fault). The 1660 has no Resizable BAR: a third, 224 MB device-local
  host-visible heap. That changes where uploads can go, not shading.
- An NVIDIA forum thread about a TU116 "not exposing FP16" is about OpenCL `cl_khr_fp16`, not Vulkan.
- Nothing found that would draw these pictures on Turing only.

## Runs (RTX 3080, `isolated-cam/` with the installed 0.4.0 game code, put back afterwards)

All exit 0, production saves unchanged. `run_cam.py` is `run_candidate.py` pointed at `isolated-cam/`;
`process_evidence.py` copy survives an `nvidia-smi` timeout (one killed the first runner, game left to finish).

- `prep0..3`: shader preparation of the four fade modes at 1.6 (prepared 1, 3, 5, 7), `left 0` each, ~7 min each.
  The first try (`map1`) stayed on the title: presses during preparation are lost.
- Chapter select with `desktop-launch-20261028/preserved/save` (map at Chapter 12): D-pad Left steps 12, 11, 10,
  Side 3 and stops; Down steps Remnant 10, Side 3, 9, 8, 7, Side 2 and stops; L, Up and the left stick don't change
  the chapter. Earlier chapters weren't reached that way.
- NEW GAME route to the Prologue (`ng1`, `ng2`):
  `600f:1,800f:8000,960f:1,1320f:1000,1620f:1,1920f:1,2280f:2,2700f:1,3300f:1,3800f:1,4300f:1`. To frame 24000 it is
  the opening and the multiverse fight, not the temple. The reporter's scene is later or in another chapter.
- No capture of the temple in any mode: the four-mode comparison is not done.

## F6 captures of 2026-11-28 (the user, chapter 3 Verse 7, runtime 612c2fca with the one-lane fix, fade 3, scale 1)

Files in `capture-20261128/` (the log, `game.log`, holds both captures' draw lists; pictures made from the targets).

- The scene colour (image 89, 1280x720, before any post-processing) already has the dots: every other pixel in a
  2x2 checker is one flat colour (230, 195 in another area), the others show the scene. A surface faded to 50 %
  through a Bayer-like pattern. `chk89.png` maps it: on the teal mist and what is behind the ice spikes.
- No temporal pass after the scene: 89, effects (102), tone map with bloom and a 256x16 table (118), the 1600x900
  window. Whatever the scene holds is shown.
- Scale 1, so Witch Time PR 191's own-pixels part and PR 219 (b) can't be the cause of this capture.
- Scene draws that bind the 64x64 pattern (texture 559592000): `293a5492` 8 draws (no depth write, no blend: a
  screen-door sheet), `7e1f3a78` 6 (fog wall), `e3f05257` 2, `88bf1b93` 1. `dither.c` changed `7e1f3a78` and
  `88bf1b93` (kill `if (false)`, alpha to the second blend output). `293a5492` and `e3f05257` have no dump yet:
  chapter 3's stages only load in chapter 3. Lead: one of them fades through the pattern in a shape `dither.c` misses.
- A 32x32 single-level texture (597669000) is bound by 1047 scene draws (all 706 of the world stage `842f6da9`),
  with the cloud array (image 26). Not known what it does; `dither.c` only knows `& 63`.
- The clouds: their evaluation stages are `isolines, point_mode`. The 520x360 cloud targets hold 1-pixel points in
  rows (`r2_t26.png`, `r2_inputs.png`). The game never calls a point-size command (only ResetCounter and
  ReportCounter go unkept) and the stage doesn't write one, so 1-pixel points are the console's too. Levels come
  from `ec0e84ad` (control): vertex `.w` times c[2].x, at least 1. Whether the console's points are as sparse is
  not known (no reference picture of these targets).
- Image 27 (all R 0.5) and images 10/11 (0.5, 0.5) are the zero of a signed encoding, not a fault; 27 is not read.

## Leads, not confirmed

1. Fog-wall stages that fetch the pattern without our discard shape: Even can't reach them (Witch Time PR 191).
2. Mist cut-out from a half-size depth at the game's size: blocks of 2 game pixels (PR 191, second part).
3. The cloud's 520x360 cut-out pass (PR 219 b).
4. SHFL in tessellation stages as a real subgroup shuffle (PR 219 a).
5. The 720p window enlarged 2x on a 1440p screen doubles every one of these.
