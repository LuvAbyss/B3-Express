# Chunked submits: overlap translation with GPU work (2026-10-29)

**Measured +12% in matched street runs.** Not deployed. Build `default-50/Bayo3_120.exe` (`48dc2e09...`), a full
build of current source (also contains the gmmu_run memo, deferred image landing, range/stack changes and the NVN
trace, off by default). Production runtime, module, INI and saves unchanged in every run.

## Mechanism

`../nvn-coverage-20261029`: the game's main thread spent 65 s of 163 s blocked in `nvnSyncWait` (>= 1 ms timeouts)
and its draw thread polled a fence ~640,000 times a second. Fences signal when the GPU thread reaches them and the
GPU finishes the work before them. The GPU thread was ~65% busy and the GPU ~37%: about 33 ms of translation, then
about 19 ms of GPU execution, one after the other, because work was only submitted at waits, barriers, stream ends
and presents. `vk_frame_check` (start of each draw and dispatch, before any setup) now submits every 50 of them
(`SNX_VK_SUBMIT_EVERY`, 0 restores the old behaviour), so the GPU runs a frame's early work while the rest is
translated. `source-changes.patch` is the whole change (cmd.c).

## Results (street, 4,800 frames, `scan-*.json`, runs `../desktop-launch-20261028/range-chunk-*`)

The route reaches one of two workload states run to run (normal ~543k draws/interval and ~23.1k dispatches, or
~557k draws and ~16k dispatches), independent of this change; runs are compared within a state.

| every | normal state FPS (last two intervals per run) | mean |
| ---: | --- | ---: |
| off | 19.24 19.33, 19.39 19.39, 18.89 18.99 | 19.21 |
| 400 | 19.33 19.60 | 19.47 |
| 150 | 21.61 21.78, 19.65 20.91 | 20.99 |
| 100 | 21.44 21.91 | 21.68 |
| **50** | 21.90 21.96, 21.28 21.41, 21.36 21.37 | **21.55 (+12.2%)** |
| 25 | 21.04 21.44 | 21.24 |

Lighter state: off 24.24 24.45 / 23.58 23.48 (`range-memo-control-1`, no chunking) mean 23.94; every 50: 26.80 27.03,
+12.4%. GPU utilisation stays ~37% while FPS rises: the same work, overlapped. ABBA set `abba-off-1, abba-n50-1,
abba-n50-2, abba-off-2` confirms; periodic screenshots match per index between arms. The default build (variable
unset) passed the chapter route with movement inputs: exit 0, 4,800 frames, saves unchanged, correct rendering.
Seven targeted tests pass.

## Next

Remaining serial cost is the GPU thread's own translation (~33 ms/frame). The fence wait data suggest the frame is
now bounded by it. Lowering it (or replacing it with an NVN-level front end, `../nvn-coverage-20261029`) is the next
lever. A deployment of this build is a new lineage and needs the user's decision.

## Deployed (2026-10-30)

Installed into `output/Bayo3_120` at the user's request for play testing: `Bayo3_120.exe` `48dc2e09...` and
`libLLVM-23.dll` `b75a2051...` (from `source/deps/llvm-sdk`, the one the isolated runs used). Saves, game DLL and INI
unchanged. Rollback files in `preserved-installed/`, record in `deployment.json`.
