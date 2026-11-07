# Detail options as Witch Time's (2026-11-07)

The user asked for "better detail like Witch Time" and chose three of its options. Build d1 (`Bayo3_120.exe`,
`d9fa42e1...`), not installed. Ported from Witch Time v0.4.5 (GPL-3.0-or-later), credited in
`source/docs/CREDITS.md`.

| `.ini` key | Values | What it does | Where |
|---|---|---|---|
| `texture_lod_bias` | -3 to 3, default 0 | added to the mip level of mipmapped, linearly filtered 3D surfaces (the samplers `anisotropy` raises); the HUD and shadow compares keep the game's | `gpu/vk/texture.c` |
| `model_lod` | 0 game, 1 highest | the game's level selector (`main+0xbd563c`) is followed by: a coarser level becomes the finest, a mesh hidden for distance is drawn at its finest when it has more than one level | `hle/model_lod.c`, `launcher/code-tablecalls.txt` (`8bda63c`) |
| `film_grain` | 1 game, 0 off | `c4[13]` (m_GrainIntensity) reads as 0 in the four tonemap pixel stages, known by Witch Time's hashes of their code | `gpu/vk/nvn_res.c` `grain_word`, `gpu/shader` `zero_cbuf` |

The launcher's Options page has Texture detail (As the game draws it, Sharper -0.5, Sharpest -1), Highest model
detail and Film grain, with Texture filtering. `model_lod` needs a game module compiled with the new
`code-tablecalls.txt`, which every Install of a version with it does.

## Tested

- Game code compiled with the new list in the scratchpad game folder (169 s), run in the isolated copy on the fight
  route (`desktop-launch-20261028/detail-on`, `detail-off`, 180 s each, the same inputs), copy restored after both.
- `detail-on` (bias -1, anisotropy 8, model_lod 1, film_grain 0): "models at their finest level", "a model drawn at
  its finest level instead of the game's" at 14 s, "texture LOD bias -1.00", "film grain off in tonemap stage"
  for `e3b5d8050915b977` and `44931dbda608639d` (Witch Time's hashes match ours). Exit 0, saves unchanged.
- Fine detail (mean absolute Laplacian, the same frames of both runs, Giza arena): far wall +11 % and +15 %, floor
  +20 % and +49 %, chains and snakes +19 % and +15 %. `far-wall-on-top-off-below.png`.
- Frame times the same within run-to-run noise on this setup (median 29-55 against 31-54 ms; the isolated setup is
  slow either way).
- `test_launcher.py` passes with the new options.

## Not tested

- The other two tonemap stages (`48c1d6be354413c1`, `e84083dddb26435c`): not reached on this route.
- A weaker GPU with model_lod=1 (Witch Time: about 0.2 ms more GPU a frame on an RX 9070 XT).
