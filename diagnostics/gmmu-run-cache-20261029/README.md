# gmmu_run memo (fix-sequence step 1), 2026-10-29

Not deployed. Production runtime `da49319f...`, module `03dcc7fd...`, INI and all saves verified unchanged after every run.

## Mechanism

After the contiguous-range change, `gmmu_run` was the top address-translation cost (2,663 samples in
`range-chapter-profile-2`). `gpu_mem_dirty_since` must translate a range to its guest pages to catch writes made
through aliasing GPU addresses, even when the range is clean by GPU address. The renderer asks about the same
buffers every draw (`cb_get` per stage and slot, `buf_get`, textures). B2recomp's coarse block stamps already
exist here as `WritePages`' leaf and 256 KiB group maxima, so the remaining repeated work was the translation.

`source/src/nx/hle/services/nvdrv.c`: `gmmu_run` now answers from a 512-slot per-thread memo keyed by (gva, max).
Every page-table change (only `gmmu_map`/`gmmu_unmap`, through `gmmu_change_begin`/`end`) holds `g_gmmu_busy` and
bumps `g_gmmu_seq` before its first store and after its last. An answer is stored only if the sequence did not move
around its walk and no change was in progress, and used only while the sequence is unchanged. No visibility,
landing or dirty-tracking rule changed. The walk itself is the old `gmmu_run`, renamed `gmmu_walk`.

## Verification

- `tests/nvdrv_service.c`: repeated queries (memo hits), and stale-after-change checks across UNMAP_BUFFER,
  FREE_SPACE and REMAP. All six targeted tests pass. With the sequence bumps removed, `nvdrv_service` fails
  (mutation check), so the test detects a missing invalidation.
- Object provenance: the deployed-lineage archive's `nvdrv.c.obj` and a fresh compile of the unmodified source
  have identical section sizes and symbol tables. A control relink of the validated candidate differs from
  `23ed2b7f` only in the 4 bytes of PE link timestamp. The pair differs only in `nvdrv.c.obj`.
- Two ABBA sets, 4,800 frames, street, controller ignored, isolated save (`comparison.json`,
  `comparison-abba1.json`, runs `../desktop-launch-20261028/range-memo-*`). All exit 0, 4,800 frames, saves unchanged.
  `range-memo-control-1` is excluded: 15,735 compute dispatches vs ~22,850 in all other runs, a different workload.
- **Matched result: control 19.47 FPS (19.26-19.72, n=6 intervals), candidate 20.03 FPS (19.91-20.18, n=8), +2.85%.**
  The ranges do not overlap. Combined with the range/stack candidate's ~9.5%, this is cumulative over production's
  ~17.6 FPS street baseline, but the stack of both has only been measured against the range candidate.
- Not yet done: a profiled run of this build to confirm the `gmmu_run` samples drop, and a chapter-12 run.

## Known unrelated issue seen while checking

Some periodic screenshots have mean colour (43,55,30) instead of (30,43,55): channels rotated. It occurs in control
runs and older runtimes too (`range-control-3`, `range-candidate-3`, `range-chapter-profile-2`). It is a capture
artifact predating this change, not a rendering regression.

## Build

`build.py` (relinks control and candidate from `../dirty-ranges-20261029/candidate/libscarletnx.a`), `abba.py`
(serial ABBA runs, takes an order such as `control:3 candidate:3 candidate:4 control:4`). Candidate:
`candidate/Bayo3_120.exe`, SHA-256 in `build.json`.
