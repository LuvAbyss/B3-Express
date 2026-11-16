# Bayonetta 3 PC port status

Bayonetta 3 v1.2.0 on Windows: the game's ARM64 code is recompiled to a native DLL by ScarletNX, and an NVN-level renderer on Vulkan draws it. Goal: a smooth 60 FPS and a launcher anyone can use. Undergraduate graduate project, started 2026-10-19.

Test PC: Ryzen 7 5700X3D, RX 6700 XT since 2026-11-14 (RTX 3080 before). Numbers are from that PC, resolution 1.6, unless noted. The notes behind each number are in `diagnostics/*/README.md`.

## Now (2026-11-16)

| | |
|---|---|
| Latest release | 0.2.8 (2026-11-15) |
| Installed in `output/` | 0.2.8's runtime with both AMD shader fixes below (runtime `dd516ed7`, game code `799aab41`). Rollback in `diagnostics/install-20261116b/` |
| Not pushed | the AMD shader fixes (`55a53f3`, `a43b413`) and their docs commits |

## Since 0.2.8

### AMD: dots and lines on characters, grid on the arena floor (fixed, the user confirmed 2026-11-16)

On AMD, character shaders OR a light-list word over each 2x2 pixel quad with SHFL. A general shuffle reads garbage from the quad's helper pixels at triangle edges, and a helper with a different word loops apart from its quad and breaks its derivatives.

| change | where |
|---|---|
| SHFL in pixel shaders keeps its own value when the source lane is a helper or inactive (`55a53f3`) | `spirv_translate.c` |
| SHFL within the quad reads with `OpGroupNonUniformQuadSwap`, which includes helpers, so all four pixels get the same word, as on the Switch (`a43b413`) | `spirv_translate.c`, `spirv_emit.c`, `device.c` (`g_shader_quad`) |

| RX 6700 XT, headless | 0.2.8 | first fix | both fixes |
|---|---|---|---|
| Shop: dark dots on Bayonetta (`tools/dots.py`) | 1,269 | 45 | 57 |
| Costume screen, 7 costumes | lines and speckles | lines and speckles | clean |
| Arena floor grid, fight frame 5500 | yes | no | no |
| Warm fight: frames over 33 ms | 20, 22 | 19, 19 | 22, 21 |
| Warm fight: hitches of 50 ms or more | 15, 15 | 15, 14 | 14, 15 |

Not checked: an NVIDIA card, the cave floor from the user's screenshot.

## Releases

| version | date | what changed |
|---|---|---|
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

## Measurements

### 0.2.7 against 0.2.8 on the RX 6700 XT (2026-11-15)

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

The hitches left in a warm fight are loading screens (frames 949, about 1090, 4623), the game's own work.

### RX 6700 XT against RTX 3080

| warm fight | RTX 3080 (0.2.6) | RX 6700 XT (0.2.8) |
|---|---|---|
| Shader preparation, new player | 26 s | 90 s (AMD's compiler; 10 s with its cache warm) |
| GPU time a frame, normal clocks | 5.5-5.9 ms | 10.2-11.6 ms |
| GPU time a frame, AMD held at 1825 MHz | | 5.2-6.2 ms |
| Mean 1% low of the fight's windows | 46.6-48.1 FPS | 43.7-46.1 FPS |
| Video memory peak (game's own count) | 3,013 MiB | 3,002 MiB |

AMD's extra GPU time is its clock policy at a 60 FPS cap, not the work.

### Fight hitches on the RTX 3080 (0.2.6 to 0.2.7)

| | before | after |
|---|---|---|
| Frames over 33 ms | 19-22 | 13-14 |
| Resolution-step hitches (41-61 ms) | 3 in every run | 1 (45 ms) in 4 runs |
| Pipeline cache save | 613 MB copied every 5 s, 50-114 ms stalls | only when it grew, none |

### Other fixes

| fix | before | after |
|---|---|---|
| Gallery (0.2.6) | 91 ms a frame, 7.4 FPS 1% low | 17 ms, 50 FPS |
| Verse exit (0.1.1) | compute up to 24 ms a frame, 1% low about 30 FPS | 0.6 ms, 46-48 FPS |
| Area-change frame (0.1.2) | 264 ms | 101 ms |
| Present pacing, recorded session (0.1.7) | frames over 19 ms: 326 | 177 |
| Widescreen 16:9 / 21:9 / 32:9, RTX 3080 | frames over 33 ms 13 / 13 / 11-12 | no frame cost; GPU load 38 / 39 / 44 % |

### Video memory and first-play freezes (2026-11-01, RTX 3080)

| fight | peak MiB | pipeline waits | worst frame |
|---|---|---|---|
| RGBA8 textures, whole pool copies | 8,819 | | |
| ASTC as BC3, sparse copies | 3,753 | | |
| First run, before shader preparation | | 29.5-32.7 s | 4,430-5,110 ms |
| First run after the preparation pass | | 0.51 s | 412 ms |

### Chapter 12 against Witch Time v0.4.5 (2026-11-02, RTX 3080, hand-played, f5 build)

| | Witch Time | this port |
|---|---|---|
| Gameplay FPS | 59.1 | 55.6 |
| Combat at 1,300+ draws | 58.9 | 48.8 |
| Longest frame in play | 132 ms | 2,428 ms |
| Peak video memory | 5,834 MiB | 4,881 MiB |

Older numbers, before 0.1.1. Not measured again on the current build.

## Open

| issue | state |
|---|---|
| AMD shader fixes on NVIDIA | not run on an NVIDIA card |
| Chapter 14 verse 7 crash (nvmap handle reuse) | fixed 2026-11-10, waits for the user's replay of that verse |
| New AMD player's first fight end | three shaders compile at frames 5949-5950 (34-75 ms) |
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
