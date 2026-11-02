# NVN route: release-only picture fixed, re-measured (2026-10-30)

Follows `../nvn-phase4-20261030/README.md` step 4.3. Runs are in `../desktop-launch-20261028/range-port-<label>`.
Build `Bayo3_120-phase5.exe` here (`e0c75462...`), **installed at the user's request after this write-up** (`deployment.json`,
rollback in `preserved-installed/`; `Run-Bayonetta3-Host.cmd` runs the host path). Until then `output/` and the saves equaled
`session-start-hashes.txt`. All changes are off unless `SNX_NVN_FRONTEND=2` (or the other host-path switches).

## What was wrong: four host-path bugs, none of them "what the stream provides"

The skipped stream provided nothing the host needed. Its renderer drew the same effects correctly, and in the modes
that kept it, that hid the host's own faults.

| bug | effect | fix (`gpu/vk/nvn_res.c`) |
| --- | --- | --- |
| host images matched by address only, in `rp_target_vk` | `6398ef000` is an 800x448 target and a 720x408 storage image; stores went to the stale 800x448 image, and the sampler rejected it on size and read unwritten guest memory. **Sky ribbon, glow** | an image is its address and size; no retire on a size change |
| stencil-reading TICs sampled through a depth view | a shader that reads the depth target's stencil (module 21, then the classification after it) got depth. **Warm light on the menus' map, title's dark phase** | `rp_stage_set`: stencil aspect when `vk_tic_reads_stencil` |
| attachment-less passes rendered 1x1 (`rp_begin`) | store-only full-screen draws (module 27, per-tile flags) ran one fragment | render area is the device limit; viewport and scissor choose the fragments |
| fragment storage-image store kind not in the key | `vk_nvn_tex_key` lacked the renderer's `rt_int_mask` bit `8+i`, so a float image got the integer store | same rule as `fs_get_keyed`. No effect measured on its own: kept for parity |

Host compute stages are not keyed for store kind. A warning (`needs a store kind`) reports it if it happens: 0 on the route.

## How it was found

`SNX_NVN_STREAM_KEEP=<mask>` (kept, off by default) runs chosen engines in release-only mode: 2 3D, 4 compute, 8 inline,
16 2D, 32 copy. Temporary aids used and removed: late slowdown, no-draw 3D, a log of what the renderer lands in guest
memory, a dump of guest memory at a frame, a log of storage image users.

| test | run | result |
| --- | --- | --- |
| 3D engine only | `k3d` | ribbon there, 15.5 FPS |
| every engine but 3D | `krest` | ribbon missing, 30 FPS |
| constant uploads only / 3D state, no draws | `kconst`, `kstate` | missing |
| slowed to 18 FPS from frame 3400 | `slow1` | missing: **frame rate ruled out** |
| what 3D-only lands in guest memory | `land3d` | 720x408 at `6398ef000` each frame: first bug |
| chapter select time series, both modes | `tsh`, `tsf` | a steady 58 against 74, so not an animated light |
| same frame, one dual run, host against renderer | `td7`, `f2000.png` | host lacks the warm light in all modes |

**A trap:** in a mode that keeps the stream, the screenshot can be the renderer's picture while the host's image is
unchanged. `tk3` read 74 on screen, but its host image dump read 58. Judge the host from saved host images, or in
release-only mode, where the renderer has nothing to show.

**Another trap:** `screen.split(maxsplit=4)` on a PPM eats a first pixel byte that is whitespace, which rotates the
channels by one byte: frames look green (`bisect1.png`, `fix1.png`). Fixed in `ppm2png.py`, `sheet.py`, `tint.py`. Sheets
made before this fix may show it. It was never in the game.

## Result

Chapter select, mean R, G, B: host 74.5, 81.0, 83.4; fallback 74.6, 81.1, 83.4. Map scene equal at 30.1, 36.3, 37.7.
Title dark phase about 4 before, now 10-16 like the fallback (animated, so compared by range). `final-route.png`: top
host only, bottom fallback. The left column is two moments of the title's animation. The gun at frame 3900 in host-only runs
is the known scripted-timing effect (loading ends sooner, the script is by frames).

Street, 4,800 frames, one run at a time, in this order, saves unchanged in all. Last two 600-present intervals:

| build, mode | run | FPS | draws in the last 600 |
| --- | --- | --- | --- |
| this build, fallback | `f5fb1` | 22.15, 21.80 | 548 k |
| this build, host only | `f5host1` | 27.19, 27.36 | 0 |
| installed `e24658fe...` | `f5inst1` | 21.20, 21.56 | 548 k |
| this build, host only | `f5host2` | 26.87, 27.15 | 0 |
| this build, fallback | `f5fb2` | 21.49, 21.54 | 548 k |
| installed | `f5inst2` | 18.02, 18.12 | 548 k |
| this build, host only | `f5host3` | 27.60, 27.80 | 0 |

**About +26% over the installed build (27.4 against 21.7)**, on a picture that matches. `f5inst2` is excluded from that
figure: it ran 204 s against 165 s with the same draws, cause not established (`p5inst1/2`, same exe, measured 21.9 and
21.8). Correct lighting cost about 2 FPS against the earlier provisional 29.4. Chapter route at the end: host 24.10, 25.84,
fallback 19.40, 23.58.

Fallback unchanged in class (21.8 against 21.7); the 8 targeted tests pass. `SNX_NVN_FRONTEND` paths only, apart from
`g_gpu_stream_keep`'s check in `gpu.c`.

## Still open

- Images aren't retired on a size change any more, only at `nvnTextureFinalize`: 94 on the street route, 130 on the
  chapter route, against the limit of 256 (`RP_TARGETS`). Past it, a new target fails. Watch this in longer play.
- Dual mode's intermittent black medallions: not seen in release-only, not fixed.
- Step 4.3's other gaps stand: CPU reads of render targets, functions outside the 49 kept, other chapters and cutscenes
  not run. Not yet played by hand.

# Phase 4 closed, phase 5 started (2026-10-31)

The user played Phenomenal Remnant 11 on the installed build through `Run-Bayonetta3-Host.cmd` (`play-remnant11.log`, 7,200
frames, exit clean, saves unchanged): "smoother and lower in latency", but intense scenes drop FPS and some freeze. By
600-present interval: 19-57 FPS, up to 1,689 draws and 179 MiB staging a frame, 1,196 pipelines made (47 in one frame),
images 210, 5 draws left out, the compute store-kind warning fired.

## Phase 4: what was left, now closed

| item | result |
| --- | --- |
| host compute not keyed for store kinds or integer textures | fired in play. `dispatch_pipeline` translates a variant by the same rule as `compute.c` |
| image table limit (210 of 256 in play) | evicts the image unused longest. A size change never retires one |
| draws left out, "made no texture" | the reason is logged with its TIC (`no texture from TIC`) |
| functions outside the kept 49, `nvnTextureReadTexels` | warn once each in release-only mode (`isn't kept`) |
| 4.1 exit, dual mode under 2/255 | `dual2`: title 0.02, chapter select 0.00, street 0.16-0.24 (was 2.6, 2.0). Chapter select was intermittent, so one good run, not proof of that one |

Not exercised on the route: the compute store-kind case, which needs Remnant 11. The warning, now `has the other store
kind`, tells if it still happens. CPU reads of render targets are not implemented, only reported.

## Phase 5: measured, in order of effect

Numbers from one evening vary with the machine: the same binary measured 27.4 one evening and 23-29 a few hours later in the same state, so only compare runs taken in one
alternating batch. The route has two states, and the older notes' description was wrong: **the slow state has about 4.3 more
dispatches a frame** (2,550 per 600 presents, against 0), draws are about equal. A run must be judged in its state.

| change | switch | effect |
| --- | --- | --- |
| chunked submits inside the host's window | `SNX_NVN_SUBMIT_EVERY`, default 100, 0 off | street, heavy state: 28.9 to 39.3 FPS (`x0a/b`, `x25`, `x50`, `x100`), GPU 56% to 78%. Every 25, 50, 100 alike, 100 best |
| constant buffers bound at the game's range | always with real writes | staging 175,805 to 1,960 MiB on the route (-99%), recording 12.3 to 7.4 ms a frame. Reads past the range are zero through robust buffer access, as the zeros of a copy. Should matter most in heavy scenes, where staging peaked; not measured there |
| statistics off in live frames (`check_list`'s references, `draw_descriptors`) | always in live frames | part of the recording drop above |
| pipelines on two worker threads, a draw waits within a budget | `SNX_NVN_PIPE_BUDGET_MS`, default 3, 0 off | cold cache: time stalled 641 to 385 ms, worst frame in it 61 to 17 ms. A draw that stores always waits; others are left out until ready: a few hundred on the route, no damage seen |
| ASTC first uploads limited per present | `SNX_NVN_TEX_BUDGET` in Mpx, default 48, 0 off | decode no longer lands at once (228-545 Mpx were followed by 220-370 ms frames). About 3,000 uploads put off on the route; a null texture until then, none seen in the shots, but the fine detail of pop-in was not looked at frame by frame |

Final build `Bayo3_120-phase5b.exe` here (`5af278db...`, the measured `d81044ed...` plus logging of slow frames), **installed at the user's request** (`deployment-5b.json`, rollback in `preserved-installed-5b/`). Alternating street runs, 4,800 frames, saves
unchanged in all, the last present interval:

| build, mode | runs | FPS | state |
| --- | --- | --- | --- |
| this build, host only | `z1new`, `z2new`, `z3new` | 40.13, 39.60, 39.45 | heavy (2,550 dispatches per 600) |
| installed `e0c75462...`, host only | `z2old` | 28.97 | heavy (2,600) |
| installed, host only | `z1old` | 44.68 | light (0): not comparable |
| this build, fallback | `z1fb` | 26.11 | |

**+37% over the installed host path in the same state (39.7 against 29.0).** The new build did not land in the light state
in these runs, so its light-state FPS is unmeasured. Chapter route at the end: 34.74, 44.25 (`zroute`), cold cache 35.61,
38.27 (`zcold`). 0 draws left out, picture matches the fallback by eye: `final-b.png`, top this build, bottom fallback
(title is two moments of its animation). The worst frames fell from 317-371 ms to about 190-207 ms, not to nothing:

**Still open, the top item:** frames of about 200 ms remain at scene changes. `tl3` splits them: the time comes after the
frame's last window, before its present, and the GPU is idle at the next frame's start. Frames of that shape with almost no
draws (`f949`: 157 ms, 19 draws; `f1130`: 115 ms, 45 draws) can't be the GPU, so at least those are the game's own work at
transitions; `f3135` (208 ms, 462 draws) has the same shape, likely the same, not proven. No heavy combat scene is scripted, so
what its freezes are made of is unmeasured: `nvn-live: frame N took ...` logs every frame over 50 ms with windows, lookups,
pipeline waits, ASTC, time before and after, so the next play session's log says. Other open items: pipelines translate stage variants on the draw thread (only
`vkCreate` moved), every barrier in the host path is ALL_COMMANDS, and the renderer's fallback path has none of this.

# Play test of `5af278db...` (2026-10-31)

The user played Remnant 10, Remnant 3 and the Prologue: gameplay up to 60 FPS, cutscenes 30, a few FPS lost in intense scenes,
input like citron-neo. Reported: RTSS shows no overlay, audio desync in cutscenes when they go under 30 FPS, some frames
darken for about a second in the Prologue. `play-5b.log` holds the last launch. Findings and next steps are in "Compact
context" in `RESUME-ON-DESKTOP.md`; nothing was changed for them yet.

# Darkening fixes and cutscene pacing (2026-10-31)

Build `Bayo3_120-phase5c.exe` here (`c75bf868...`), **not deployed**. Isolated runs only, production saves unchanged after each.

User's repro for the darkening: 3-4 frames in the cutscene where Gomorrah destroys the big enemy, maybe 1 frame in the cutscene
after Bayonetta saves Jeanne from the kraken. That is 1-4 frames, not the "about a second" first reported.

## Changed

| change | where | tested |
| --- | --- | --- |
| `nvnCommandBufferUpdateUniformBuffer` recorded with its bytes (x5, x4 bytes) and replayed in list order as a staging copy into the pool between barriers (`vk_nvn_replay_update`), as hardware writes in command order | `cmdlist.c` `ds_dynamic`, `nvn_res.c` | **not exercised**: none of the scripted routes calls it, Thule (`c1`) included. `commands kept: ... UpdateUniformBuffer:N` in a play log is the proof |
| a texture that isn't there (all-zero TIC, any other NULL) binds the declared type's null texture and draws, as the renderer's `stage_set` | `nvn_res.c` `tex_get` | routes pass, 0 left out; the zero TIC itself not reached by a route |
| lost commands counted per function | `cmdlist.c` `cmd_unkept`, `lost in release-only mode, N: name` | the old shared `WARN_LIMIT(32)` was used up by this one function, so **any other lost command after t=127 of `play-5b.log` was hidden** |
| what each frame lacked | `nvn-live: frame N lacks: ...` | fires on routes |
| where a frame goes | `nvn-live: pace, a frame on average: ...`, `g_pace_slept_ns`, `g_pace_restarts` in `present.c` | see below |
| slow-frame log limit 200 to 20000 | `nvn_res.c` | |

## Darkening: three suspects, not settled

Stale constants (the first suspect) are now fixed, but two phase 5 changes fit a 1-4 frame flash as well or better, and both
were active in the Prologue: a draw is left out while its pipeline is in the making (`SNX_NVN_PIPE_BUDGET_MS`, 4-350 draws per
interval there), and a new ASTC texture is a null texture until its upload budget allows (`SNX_NVN_TEX_BUDGET`). Route `u1`:
textures up to 16 Mpx stayed null for 8 frames in a row at a scene change. Match a dark frame's time to the `lacks` lines. If
they coincide, retest with `SNX_NVN_TEX_BUDGET=0` or a larger `SNX_NVN_PIPE_BUDGET_MS`.

## Cutscene dips: mean frame cost, not spikes and not the pacer

`play-5b.log` per 600 frames: t=1485 lost 0.91 s with no frame over 50 ms (worst 43.2), t=1442 lost 1.00 s with one (52.1). So
slow frames don't account for these dips. `present_pace` catches up within a refresh of lateness, so losing time means the mean
frame is over 33.3 ms. The dips sit at 1,700-1,900 draws, recording 11 ms a frame.

Same state, street, `pace` line (ms a frame):

| run | FPS | before first window | windows | after last | pacer | over 33.4 ms | restarts |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `pfree` | 40.0 | 13.1 | 7.4 | 4.4 | 0 | 1 of 600 | 224 |
| `p30` (`RUN_FPS_LIMIT=30`) | 30.0 | 20.5 | 8.2 | 4.7 | 15.2 | 261 of 600 | 0 |

The pacer holds exactly 30.0 with 44% of single frames over 33.4 ms, so it is not the cause. The host's windows run inside
`nvnQueueSubmitCommands`, on the calling game thread, so about 30% of an uncapped frame blocks that thread. Likely next step: cut
recording cost per draw (descriptor reuse: a `vkUpdateDescriptorSets` per stage per draw), or record off the game's thread, which
is harder because `check_list` calls driver getters on the guest CPU and reads guest memory at record time. Not proven on a real
cutscene: the next play log's `pace` lines say. Audio against frames as the desync's cause is still a hypothesis.

## RTSS: closed

No code problem. `VK_LOADER_DEBUG=layer` (`layer2`) shows `VK_LAYER_RTSS` inserted and `RTSSHooks64.dll` loaded. The user reports
the overlay showing. The notes' volk idea was wrong: a loader layer is under volk. RTSS's `InjectionDelay` is 15 s.

## Also found, not fixed

11,784 draws in `play-5b.log` had no pipeline (`a bound stage not translated`, 2 a frame on 5,892 frames): one tessellation
evaluation stage uses FSWZADD, which `spirv_translate.c` allows only in fragment stages. A steady loss, not a flash, and the old
renderer has the same gap.

# Phase 5 continued without a play test (2026-10-31)

Build `Bayo3_120-phase5d.exe` here (`96c28b66...`, phase5c plus redundant state skipped), **not deployed**. It supersedes phase5c.

## A scripted route with real combat

`route-remnant9.txt` here is the script, `route-run.py` runs it (`route-run.py <label> "<script>" 12000 400`, isolated, checks saves;
needs `SNX_NVN_FRONTEND`-free environment, it sets host mode). 3 Downs on the chapter map reach Phenomenal Remnant 9; bit 41 is
the left stick forward. Bits 40-47 are stick keys, so a script can walk. It reaches the Giza fight and a game over at about frame
11,400, the same on every build, so builds can be compared in it. Left on the map goes through Jeanne's side chapters; many Downs
run the free cursor off the map. The Prologue was not reached.

## This corrects the section above

`UpdateUniformBuffer` **now exercised**: 2,398 / 2,420 / 2,876 calls in `rem2`, `rem3`, `abf1`, exit 0, 0 left out, none lost. So it
needs combat, not a Remnant as such. Null textures for zero TICs **now exercised**: the same fight left out 5 draws on the installed
`5af278db...` (`rem2old`), 0 on this build. No other command was lost, so the old warning limit was not hiding one on this route.
Shots of `rem2old` and `rem2` match, but they are 600 frames apart, so **no dark flash was caught or ruled out**: the
Gomorrah and kraken cutscenes are still the test.

## Recording: redundant state skipped

Timers, street, ms a frame of 7.7 in windows: Vulkan recording 2.7, stage sets 1.4 (`vkUpdateDescriptorSets` 0.33), texture
readiness 0.5, chunks 0.35, pipelines 0.06, and 2.7 outside `vk_nvn_replay_draw` (`check_list`, `draw_pipeline`, not split).
`rp_dynamic` now sets only what changed within a pass, and the pipeline, index buffer and push constants likewise (`g_rpd`,
forgotten in `rp_end_rendering`: uploads, compute, the renderer and submissions all happen outside a pass). Recording 2.7 to 2.26,
windows 7.7 to 7.25; no FPS change beyond noise (`dyn1`, `dyn2`: 39.68, 39.19). Picture matches in `rem3`. So recording is not the
main cost, and moving it off the game's thread would be a large job for at most 7 ms: not the next step.

## Game code at O1: +9%

Module `../desktop-launch-20261028/ryzen-o1/Bayo3_120/Bayo3_120_code.dll` (`782891b7...`, 250 MB against 407 MB), compiled here in
188 s with this runtime source, `recompile.py 1 ryzen-o1 12` (third argument, the jobs, is new). Alternating street, same build,
same heavy state:

| module | runs | FPS | before first window | windows | after last |
| --- | --- | --- | --- | --- | --- |
| O1 | `abs1a`, `abs1b` | 43.04, 43.20 | 14.4 | 6.5 | 2.2 |
| O0 | `abs0a` | 39.63 | 13.8 | 7.1 | 4.3 |

`abs0b` (O0, 45.15) landed in the light state: 0 dispatches in its last interval, excluded. Fight, `abf1` O1 against `rem3` O0: frames
about 5% cheaper at the same places (18.1 against 19.1, 16.9 against 18.1 ms before the pacer), picture matches, exit 0, 0 left
out. The October O1 test found nothing because the old renderer was the limit. Only "after the last window" fell, so 14 ms
before the first window is not simply the game's CPU work: what fills it is the next thing to measure. O1 has run on the street
and Remnant 9 only, three and one runs.

## Next

Split "before the first window" (game work against waits). The 2.7 ms in `check_list`. Then, if the Gomorrah flash survives, the
pipeline and ASTC budgets.

# Video memory copies: the GPU was the limit (2026-10-31)

Build `Bayo3_120-phase5e.exe` here (`69b6376b...`), **not deployed**. It supersedes phase5c and phase5d. This corrects two things
above: the module `ryzen-o1` was compiled by `desktop-launch-20261028/runtime-only/scarletnx.exe`, not this session's source, and
the 14 ms "before the first window" is not simply the game's CPU: it shrank to about 6 ms once the GPU stopped lagging.

## What was wrong

Timestamps around the host's window (`GPU time of the host's windows`): **17-22 ms of GPU time a frame** on an RTX 3080, GPU 85%
busy, memory controller 1%, 2-4 GB/s arriving over PCIe. The game's threads poll `nvnSyncWait` (900+ calls a frame, 0.2-0.3 ms).
Every vertex, index, constant and storage buffer read crossed PCIe, because the imported pools (`VK_EXT_external_memory_host`) are
system memory.

## The change (`gpu/vk/nvn_res.c`, `vk_written_each` in `buffer.c`)

Pools with `GPU_CACHED` and neither shader code nor compressible (flags 0x22, 0x24: 2.4 GB) get a device-local buffer of the same
size and layout. The pool stays the truth, every write still goes to it, and a read uses the copy only when all its 64 KiB blocks
are `MIR_VALID` (`rp_read_buf`). Read paths: vertex, index of direct draws, uniform, read-only storage. A block goes back to the
pool on the CPU's writes (`vk_written_since` per window, only over 4 MiB chunks with blocks copied: 1.9 to 0.37 ms a frame) and on
the host's own (`rp_stamp`). Blocks read since their last write are copied again at the window, so a list reading what the CPU
just wrote finds it in video memory. Blocks the GPU writes in 4 windows stay in the pool (`hot`, halved every 600 frames). 64 MiB a
window at most. `SNX_NVN_VRAM=0` turns it off. A failed allocation or lost tracking falls back to the pool.

**Mistake worth keeping:** the watch reports 4 KiB pages, so one write to a block counted 16 and everything looked hot (`v1`,
21% of reads). Count once a window. A run label that already exists makes `run_candidate.py` stop and the old log stays: `chk1`
looked like a broken run. Use a fresh label and check `elapsed` first.

## Results

Same build, same heavy state (2,520-2,550 dispatches per 600), alternating, last interval:

| game code | copies off | copies on |
| --- | --- | --- |
| O0 | 40.13 (`a0off`) | 58.54 (`a0on`) |
| O1 | 43.70, 43.80 (`a1off`, `a1off2`) | 59.8 (`a1on2`, `f1street`: the 60 cap) |

GPU time 20.2 to 11.5 ms, 72.6% of reads from video memory, about 7 MiB copied a frame, video memory used 5.4 to 7.75 GB. Fight
6.2 ms GPU, Thule 7.3, Jeanne's chapter 5.5, all at the cap. O2 (`ryzen-o2`) gave nothing over O1: 44.41, 44.88 against 44.58.

Correctness: `SNX_NVN_VRAM_CHECK=1` reads blocks back every 300 frames and compares with guest memory. Fight, street, Thule,
Jeanne with chapter loads, fight again: **43,145 blocks, STALE 0**; those that differed were all written after the window's scan.
All runs exit 0, 0 left out, production saves unchanged, `UpdateUniformBuffer` 3,051 and 3,462 calls, 8 of 8 tests. Old renderer
21.1 FPS (`ffb`), its usual range. Shots match by eye, 600 frames apart.

## Slow frames

`afight`: 174 frames over 50 ms in 12,000, mean 74.5 ms, of which windows 4.7, pipeline waits 0.3, and 66.3 outside the host's
windows; mostly loads. 0 draws without a pipeline. So what remains is game-side, not unmeasured renderer work. Heavy combat by hand
is still not scripted.

## Not known

Whether the cutscene dips and desync are gone: a mean frame over 33 ms at 1,900 draws fits this GPU limit, but no scripted run
reached those cutscenes. The dark flash: untested. Kernel writes that bypass the page's dirty bit (unbuffered I/O into a pool) would
escape the watch: the check saw none on chapter loads. The texture cache has the same exposure.

## Next

Remaining 27% of reads from the pools, indirect draws and index buffers of them. 2.3 GB of video memory is spent for about 110 MiB
copied: a smaller allocation would need offsets remapped, not needed on 10 GB.

## Installed

phase5e (`69b6376b...`) with the O1 module (`782891b7...`), **installed at the user's request**, `deployment-5e.json`, rollback in
`preserved-installed-5e/`. INI, ABI stamp, libraries and saves unchanged by the install; the libraries match the isolated copy's.
Not launched from `output/`, not played yet.

# Dark flash caught: draws left out for a pipeline (2026-10-31)

Build `Bayo3_120-phase5f.exe` here (`78aacd98...`), **not deployed**. `output/Bayo3_120/Bayo3_120.log` at the start of this session was
byte-identical to `play-5b.log`: phase5e has still not been played. O1 module in every run below, production saves unchanged.

## How it was caught

`--shot_from` and `--shot_to` (new, `unattended.c`) with `--shot_every=1` save a window of frames named by frame; `luma.py` and `spell.py`
print brightness and make a sheet. A shot can skip a frame (frames move by two between looks), so check a frame exists before saying
it is clean. `flash-batch.py` runs labelled routes with budgets, windows, a source run's script and `RUN_COLD=1` (empty pipeline caches,
restored after). `budget-sum.py` sums slow and lacking frames.

## What it shows

| frame | seen | lacked | at 33 ms |
| --- | --- | --- | --- |
| fight f5948 | floor, walls and snake solid black for one frame, HUD and metal fine, brightness 43 to 18, in 3 of 3 runs | 98-107 draws for a pipeline, target 1 Mpx | 43 to 43, correct |
| Character Select f2510 | whole background black for one frame, 109 to 20.7 | 61 draws and 94 textures | 109 to 109 |

Both were warm: the cache held every pipeline, a cached `vkCreate` still takes about 2 ms, and 3 ms ran out. So phase 5's own
`SNX_NVN_PIPE_BUDGET_MS=3` makes flashes. **Texture spells are not shown to do it**: the 3-8 frame runs of null textures on the
fight, Thule and Jeanne routes are at brightness 0.0 or the first frames of a fade-in (2-6), behind the game's load fade. The Gomorrah
(3-4 frames) and kraken (1 frame) cutscenes themselves were not reached, so they are expected fixed, not seen fixed.

## Changed

| change | where | measured |
| --- | --- | --- |
| pipeline budget 3 to 33 ms | `nvn_res.c` | warm: nothing left out on Thule, Jeanne, street; one fight run lost 32 draws at f3790, in a black load spell. Cold, fight, 3 against 33: draws lost 27,130 to 6,036, slow time 22.6 to 21.8 s. 9999 (never leave out): 0 lost but 42.8 s, worst frame 5.3 s, because a waiting draw thread can't queue the next pipelines |
| pipeline workers 2 to 4 | `nvn_res.c` | cold at 33: worst 2.7 to 1.7 s, slow time 21.8 to 17.2 s, lost 6,036 to 3,942 |
| ASTC budget off by default (`SNX_NVN_TEX_BUDGET=48` brings it back) | `nvn_res.c` | put off 1,553 to 0. **Cost:** the three worst frames rise from about 100-125 ms to 220-270 ms (up to 360 on the street), slow time in the fight 5.1 to 5.8 s. Those frames are at load fades. A budget turns a bounded hitch into black materials on any cut without a fade |
| FSWZADD outside fragment stages | `spirv_translate.c` `stage_has`, `fswzadd_derivative` | corpus 5,616 of 5,616 (was 5,614). The tessellation evaluation stage is a compiled `textureGrad`: SAM, SHFLs broadcast a lane, FSWZADD 0xcc and 0xf0 (nouveau's QOP(MOV2, ADD, MOV2, ADD)), TEX, RAM. Lane form everywhere, as Eden and Ryujinx. No route reaches these 2 draws and `spirv-val` is not on this machine, so **not seen running**: `not loaded` in a play log says |

## Verified on phase5f

Fight with block check (0 stale, 9,848 blocks), Thule, Jeanne, street twice, cold fight: all exit 0, 0 left out at the end, 59.5-59.6 FPS
(the cap) at the last interval, except Jeanne's route, which ends in a 30 FPS scene. 8 of 8 tests. `tex_upload_rows` no longer compiles
(`bl_level_size` gained arguments in the tile width change): an earlier break, not this build's.

## Not known

Cold first encounters still lose draws (3,942 in the fight, about 60 ms a pipeline on empty caches). The real fix is building recorded
pipelines at start, as the yuzu family's disk cache does: not done. Cutscene dips and audio desync have no new evidence.

## Installed

phase5f (`78aacd98...`) with the O1 module (`782891b7...`), **installed at the user's request**, `deployment-5f.json`, rollback in
`preserved-installed-5f/`. Exe only; saves, module, INI, ABI stamp and libLLVM hashed equal around the install. Not launched from
`output/`, not played yet.

## Video memory, for weaker GPUs

Peak used by the card at 1600x900 internal, 720p window: street 8,650 MiB, fight 8,569, Thule 8,230, Jeanne 6,691, of which 1,355 was
already in use before the game. Copies off 5,964 from a 942 start, old renderer 5,171. The copies allocate 2.33 GiB (37,336 blocks) and
1,864 blocks, 116 MiB, are valid: 95% of that allocation holds nothing the GPU reads. Only one buffer per pool keeps the pools'
offsets, so shrinking it needs offsets remapped. The buffer is `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`: on a small card a failed allocation
falls back to the pool, but one VMA or the driver places in system memory would be a copy over PCIe for nothing: not tested. Nothing
here was run on a smaller GPU.

# Phase5g: resolution scale, mailbox, cutscene pacing (2026-10-31, evening)

Candidate `Bayo3_120-phase5g.exe` (`763485b5...`). **Not installed.** The user's play of phase5f: gameplay 1% lows over 40, dark flashes
gone, high-brightness flicker around the Gomorrah cutscene, audio falls behind whenever a real-time cutscene drops under 30.

## Play log findings

- The 30 FPS sections are the game's own: `vi: swap interval 2`. Not a dip.
- First section, frames 114361 to 118218 in 129.06 s: 14.8 frames (0.49 s) short of 30. 22 frames over budget (50-100 ms, about 555 ms
  over), and 22 schedule restarts. The hitches are the desync. Slow frames carry ASTC 70-120 Mpx at camera cuts, or are the frame after.
  Later sections' deficits in the first analysis are not valid: their edges fall inside mixed 30/60 stats windows.
- Cutscenes are frame-locked: Jeanne's second section is exactly 2,388 frames at scale 1 and 1.6. With `fps_limit=60` (`j60b`) the first
  section ran at 59.93 FPS and took 10.7 s instead of 25.2 s, 640 frames against 755 (the script's timed inputs likely cut it). Same as
  the emulator: uncapping speeds the scene up. A 60 FPS cutscene needs a patch to the game's timeline, not done. Audio not checked by ear.
- Flicker suspect, **not proven**: frames 113374-76 (t 1930.8) each hit `pipeline waits 33.0` and lacked 2-5 draws on a 921,600 px target,
  frame 113498 lacked 67 on 589,824 px, all about 15 s before the interval 2 section. No `lacks` line inside any 30 FPS section. Those
  pipelines are in the cache now, so replaying that scene on phase5f separates this from anything else.

## Changed

| change | where | state |
| --- | --- | --- |
| `resolution` works on the host path. Images keep the game's size as identity, `sw`, `sh` are their own. Scaled: 16:9 family (aspect 1.70-1.80, width 320+, one layer), which covers the dynamic resolution steps. Storage images stay at the game's size. Viewport, scissor, clears, feedback copies, readbacks and the present follow the pass | `nvn_res.c` `rp_scales`, `rp_target_vk`, `rp_begin`, `rp_scissor`, `rp_dynamic` | fight, street, Thule, Jeanne run, shots match. `stores to a scaled image` warning: 0 |
| FS at scale other than 1: FragCoord per axis, TLD, TLDS and TXQ through a per-slot image/game ratio at `SNX_VKB_RS` element 1 + slot. At scale 1 the SPIR-V is unchanged, so the pipeline cache stays valid | `spirv_translate.c`, `ShaderInfo.host_scale` | `g10a` at 1: 1 frame with `lacks`. Compute reading scaled images isn't corrected: none in the dumped frame |
| vsync: 1 mailbox else FIFO, 2 FIFO, 0 immediate else mailbox | `device.c`, `options.c` | log says `mailbox` |
| at swap interval over 1 the pacer keeps its schedule up to 250 ms behind and frames after a late one run unheld. Interval 1 unchanged | `present.c` | see below |
| a draw of 6 vertices or fewer always waits for its pipeline | `nvn_res.c` | **not exercised by a known scene** |
| `vi: swap interval N at frame F`, 1024 lines | `vi.c` | exact section lengths in play logs |

The game's final image is always 1600x900 (`655720000`, format 37). The scene is drawn at a dynamic step (1440x816 in the dumped fight)
and upscaled into it, so `resolution=1.6` is a 2560x1440 final image, and the scene is 2304x1306 at that step.

## Measured, O1 DLL, one run each

| route, scale | FPS at the end | GPU ms a frame | VRAM peak (desktop at start) |
| --- | ---: | ---: | ---: |
| street 1.0 | 59.8 | 12.1 | 9,355 (1,959) |
| street 1.2 | 54.2 | 15.0 | 9,487 (1,959) |
| street 1.4 | 46.7 | 17.8 | 9,510 (1,960) |
| street 1.6 | 41.0 | 20.8 | 9,609 (1,958) |
| fight 1.0 | 59.7 | 6.3 | 9,293 (2,019) |
| fight 1.6 | 57.3 | 9-12.6 | 9,510 (2,032) |
| Thule 1.6 | 59.4 | peak 20.6 | 9,164 (2,106) |
| Jeanne cutscene 1.6 | 30.0 | | 7,628 (2,105) |

Pacer, Jeanne's second section: phase5f 79.70 s, phase5g 79.66 s, for a 79.60 s scene. 0.10 s lost to 0.06 s: right direction, thin
evidence, because this scripted scene has few hitches where the user's lost 0.49 s. First runs at 1.6: 468 draws left out in the fight,
as shaders using FragCoord, TLD or TXQ are new pipelines. Tests 7 of 7 of the set built here. All runs exit 0, saves unchanged.

## Not known

Gomorrah and kraken cutscenes aren't reached by a route, so the flicker fix is unseen. Audio hasn't been judged by ear. The
2.2 GB of empty mirror remains the easy VRAM saving if a 10 GB card fills: game alone about 7.6 GB at 1.6.

## After phase5g

Installed at the user's request for testing, `deployment-5g.json`, rollback `preserved-installed-5g/`, `resolution=1` left in the INI. The
Gomorrah high-brightness flicker is not a bug: the user recorded it and it is the same in Citron. So the flicker suspect above is closed,
the 6-vertex wait stays as protection for full-screen passes.

Upscalers, from the fight dump (one frame, frame 5900): depth at scene size (image 51 D24S8) and linear depth (55, R32F). No target
identified as motion vectors: 54 (R8G8) samples depth and shadows, so it is a shadow mask. No full-size image is read before it is written,
so no full-size TAA history; history only at half size (56, 61). Temporal upscalers would need motion from elsewhere (reprojection from
depth and matrices covers camera motion only). User asked about DLSS and FSR, order proposed: FSR 1, then FSR 2/3 and DLSS if motion can be had.

## How the Bloodborne port does DLSS (read 2026-10-31, github.com/Supermedo/bloodborne_pc at b480526, not copied)

Files are GPL-2.0-or-later, so usable here with attribution; `gpu/dlss_bridge` is MIT and built as a separate `bbport_dlss.dll` against
NVIDIA's SDK, which isn't in the repo. Corrects the note above: per-object motion doesn't need per-game shaders.

- Camera motion: compute pass over scene depth with this and last frame's camera (`camera_motion.comp`). The camera comes from the game's
  864-byte scene constants, found by size: this is the Bloodborne-specific part. B3 needs its own.
- Object motion: a variant of the vertex shader saves clip positions to a history buffer, reads last frame's, and writes the difference
  to an extra target. Only draws with a bone palette get it (buffer size rules): everything cost 77 to 49 FPS, selected about 140-225 draws
  a frame kept 81-87. Keyed by streams, shader, indices, ranges, order. Others keep camera vectors, as do indirect draws, tessellation
  and geometry shaders, and transparency/particles aren't solved.
- Jitter: a shift added to clip positions of scene geometry only. HUD goes on after reconstruction and never enters history.

For B3, check first: which constant holds view and projection, how many skinned draws are indirect or tessellated, and what fraction of the
scene pixels those leave on camera vectors. Stage order: scene/HUD split with FSR 1, camera vectors and jitter, object vectors.

## Bloodborne port: temporal inputs, traced in its code (for phase 7)

- Camera vectors: `camera_motion.comp`, depth plus this and last frame's view and projection, written in pixels, previous minus current, with
  jitter taken out. RG16F at render size. Camera found by a constant buffer's size and signature; no camera in a frame invalidates history.
- Object vectors: VS variant stores unjittered clip position and loads last frame's by device address, `slot = vertex + instance * vertices`,
  budget 4 M positions a frame, 128 MiB. Frag writes dx, dy, valid, depth to an extra target. Compose: an object vector wins where valid and
  its stored depth matches final depth, else the camera's. Draw matched across frames by shader, streams, index address and hash, range,
  instances, order. Selected by bone palettes; small ones only when the palette hash changed.
- Jitter: Halton(2, 3) minus 0.5, phases `ceil(8 * ratio^2)`, 8 to 256. Clip x and y shifted by jitter * w after positions are recorded, only
  for draws on scene depth with more than 6 indices or instanced. They left viewport shifting: it exposed an edge row.
- Handed to DLSS: color, depth, motion, output, jitter, reset, frame ms, sharpness; flags MVLowRes, depth inverted, HDR with auto exposure.
- Reset: output or preset or upscaler change, a frame without the upscaler running, a frame with no camera. A camera-cut detector was not found.
- Reactive mask: difference of scene colour before and after blended draws, FSR only, opt-in, they found it worse on thin mist.

B3, dump frame 5900, scene passes (1440x816): elements 237, multi indirect 187, arrays 4, indirect 2. 44% of those calls are multi indirect, and
one call can be many draws. Their index-range method doesn't cover them, so this is the first thing to solve. One frame; pixels unmeasured.

## Phase5h: FSR 1 (2026-10-31, night)

`Bayo3_120-phase5h.exe` (`56e32214...`), **not installed**. Shaders built with official glslang 16.6.0 at the user's permission
(`source/.cache/tools/glslang-16.6.0`, the binary is `glslang.exe`), by `source/scripts/build-fsr1-shader.py`. O1 DLL, one run each:

| route | setting | FPS at the end | GPU ms | VRAM peak (start) |
| --- | --- | ---: | ---: | ---: |
| street | 1.6, fsr 1.6 | 59.4 | 12.3 | 9,499 (2,294) |
| street | 1.6, no FSR | 40.0 | 21.5 | 9,682 (2,163) |
| fight | 1.6, fsr 1.6 | 59.2 | 6.8 | 9,493 (2,177) |
| fight | 1.0, no FSR | 59.9 | 6.2 | 9,240 (2,140) |
| Jeanne cutscene | 1.6, fsr 1.6 | 30.0 | | 7,544 (2,155) |

Log: `FSR 1, 1600x896 to 2560x1440`, the log line is limited to 32. Shots against native 1.6 on street, fight and menu: HUD sharp, no offset, halo or
edge fault; 3D softer and a little crunchier on fine detail, as expected. Jeanne's 2,388-frame section took 79.67 s. All exit 0, saves unchanged, 7 of 7 tests.
Pass-scale fix: s16h and f10h match 5g. Not seen: full-screen movies, Thule on this build, Gomorrah and kraken, weak GPUs, other FSR ratios.

## Laptop results (2026-10-31, late): i5-12450H, RTX 4050 6 GB, phase5h2, portable-o1

Logs in `laptop gameplay logs/`: `Bayo3_120_1.log` (`Play.cmd`), `Bayo3_120_2_Low_Vram.log` (`Play-Low-VRAM.cmd`). Both Remnant 10 by hand, `resolution=1.2 fsr=1.2`, a
1280x720 window for the whole run (one `swapchain` line, written at every creation), so the 1920x1080 FSR result is scaled down. The user's dump installed with no error: 557 files, RomFS 14,350.5 MB.
**The runs are not matched: `Play.cmd` ran first on an empty pipeline cache, Low-VRAM loaded 30,821 KB.** Combat here means interval 1 windows over 1,000 draws a frame. "Typical" drops the frames over 50 ms
from a window's averages. "GPU" subtracts logged pipeline waits, which land inside the window's timestamp pair: raw 20-22 ms windows fall to 11-15 ms, and windows without a stall barely change.
CPU is before the first window, minus held, minus the game's GPU wait, plus windows and after. The pacer's hold is inside "before", as the present is stamped before it sleeps.

| combat | Play.cmd, cold | Low-VRAM, warm |
| --- | ---: | ---: |
| windows, time | 20, 261.6 s | 12, 174.6 s |
| FPS | 45.9 | 41.2 |
| typical frame | 18.8 ms, 53 FPS | 24.6 ms, 41 FPS |
| GPU | 12.7 ms | 20.8 ms |
| CPU | about 17 ms | about 18 ms |
| game's wait for the GPU | 1.2 ms | 5.7 ms |
| frames over 33.4 ms | 2.5% | 3.9% |
| frames over 50 ms | 2.08% | 0.31% |
| slowest 1% | 3.6 FPS | 19-24 FPS (22 logged, 50 bounded) |
| worst frame | 2,991 ms | 77 ms |
| pipeline waits | 33.3 s | 0.4 s |

- Stalls: pipelines made 1,850 and 1,733, so the same work, cold against warm. 242 of 249 slow combat frames in `Play.cmd` are pipeline waits. A draw of 6 vertices or fewer skips the budget (`nvn_res.c:2389`, phase5g), so waits reach 3.7 s where the budget is 33 ms. 253 frames lacked draws (2,402) against 15 (188).
- CPU limit with copies on: f12600-13200 has no frame over 33.4 ms, 1,517 draws, 48.1 FPS, 13.5 ms GPU, nothing held. CPU is about 14 ms at 1,070 draws and 20 ms at 1,500. Desktop at 1,000 draws: 11.2 ms, on the Ryzen module.
- GPU limit with copies off: every combat window is over 16 ms GPU and the game waits 5.7 ms.
- VRAM: **not measured**, the log has no budget line. All four copies (2,333 MiB) were made, nothing was reported lost, and GPU time a 1,000 draws holds 9.6 to 9.4 ms across the session. The copies save 8 ms, which they would not do from system memory. No sign that 6 GB limits this chapter, in this window.

### Warm `Play.cmd` runs: laptop results closed

`Bayo3_120_3.log`, `_4`, `_5`, all `Play.cmd`, cache 31.7 to 33.2 MB loaded, same 720p window. `vram.csv` is run 5, nvidia-smi at 1 Hz, CSV second is about log time + 14 (aligned on the exit).

| combat | run 3 | run 4 | run 5 | all three | Low-VRAM |
| --- | ---: | ---: | ---: | ---: | ---: |
| FPS | 52.8 | 55.4 | 53.6 | 54.0 | 41.2 |
| typical frame ms | 19.0 | 17.4 | 18.6 | | 24.6 |
| GPU ms | 13.2 | 12.2 | 12.3 | | 20.8 |
| CPU ms | 16.9 | 15.2 | 16.2 | | 18.4 |
| frames over 33.4 ms | 0.56% | 0.34% | 0.41% | 0.43% | 3.85% |
| frames over 50 ms | 8 | 10 | 9 | 0.09% | 0.31% |
| worst frame ms | 70 | 155 | 190 | 190 | 77 |

- Matched load, FPS of typical frames: 1,200-1,350 draws 54-56 against 42, 1,350-1,500 draws 51-52 against 37. Slowest 1% pooled over 29,400 frames: 24-33 FPS (27 logged, 98 between 33.4 and 50 ms, 169 unlogged and assumed 22-33.4 ms). Pipeline waits 0.5 s in 544 s, against 33.3 s in 262 s cold.
- Still CPU limited: 60 FPS up to about 1,200 draws, 46-50 at 1,500+. GPU utilisation median 72%, which fits 12.3 of about 18 ms, clock 2,385 MHz, 33 W.
- VRAM, game alone (0 MiB before launch, the laptop desktop is on another GPU): 3.05 GB in the 30 FPS section, peak 5,848 of 6,141 MiB (95%), combat median 5,538, range 4,652-5,848. Combat alone was 7.1 GB on the desktop at scale 1, so some data probably sits in system memory (inferred). No cost seen: GPU ms a 1,000 draws 9.0 to 10.5, no drift, in all warm runs. 14 steps of 150+ MiB in combat, 3 within 2.5 s of a slow frame, all at the 233 s load (ASTC 274 Mpx). Whether those falls are the game freeing or Windows moving data out is not known.
- Mirror shrink stays on hold: warm `Play.cmd` beats Low-VRAM and GPU ms a 1,000 draws stayed about 10. It comes back for other chapters that show a rise, or smaller cards. `Play-Low-VRAM.cmd` has no use on this laptop.
- Clock reason `0x400` during play is not in the nvidia-smi help. It sits above Display Clock in the bit order, where Board Limit and Reliability are listed, so likely one of those: unconfirmed, clock held 2,385-2,400 MHz. Temperature rose from 64 to 83 C through about 3 minutes of combat, no thermal reason set.

Not known: CPU temperature and clocks, sessions of 30 minutes or more, other chapters, fullscreen 1080p.
