# Shaders in order (2026-11-03)

Witch Time's change (issues 92, 100, 107, `docs/STATUS.md` "The goo / golden light / clouds of erasure and the
dispatcher"): a shader whose every jump goes forward runs its blocks once, in order, instead of one block per pass of
the `switch (pc)` loop. Our code, written from that description: `source/src/nx/gpu/shader/spirv_translate.c`
(`linear`), switch `SNX_SHADER_LINEAR=0` (runtime and `snx-shaderc`). Each block is `if (pc == start)`; a jump only
sets pc. Lanes at a block's start run it together in one pass, so they reconverge there as our converging loop makes
them, without its subgroup minimum on every pass. Kept in the loop: backward jumps (real loops), CAL/RET, PEXIT, a
dynamic stack, unknown BRX tables, and BAR (a barrier must not sit in a branch only some invocations take).

## Builds

- `Bayo3_120-s2.exe`, sha256 `ba207167a48323787bb06edf2b60c4d3f5d9d811857e46130b6120f388212948`: c7 + button prompts
  (p1) + shaders in order (step 2). NOT installed.

## Checks

- Corpus `nvn-port-20261029/corpus-nvn-7` (5,616 shaders) through `snx-shaderc`: all translate, on and off. Shaders
  in the loop, off -> on: vertex 198 -> 7, compute 62 -> 15, fragment 2,818 -> 1,297 (step 1, without reconverging
  ones: vertex 198 -> 7, compute 62 -> 55, fragment 2,818 -> 2,808). No SPIR-V validator ran (`spirv-val` isn't
  installed; a downloaded one was not run).
- Scripted route (`desktop-launch-20261028/lin-*`, 4800 frames): screenshots the same within noise (the title picks
  a random scene). Steady windows p99 23-24 ms off, 20-22 on; 1% low 41.7-43.5 FPS off, 45.5-50 on (two runs each).
  The first on run after step 2 had 21 draws waiting for pipelines: new SPIR-V misses the driver's cache once.
- Replay of `recordings/20261103-122329` with `SNX_NVN_PASSTIME=30` (`range-lin-*-r1/r2`, 4500 frames, RTX 3080):
  GPU per frame median 8.5 / 9.3 ms off, 9.8 / 9.1 on; the 2304x1306 target 8.77 / 9.27 us a draw off, 9.15 / 9.11 on.
  No difference beyond run-to-run noise on NVIDIA.

## Reading

On the RTX 3080 the loop wasn't the cost: NVIDIA's compiler handles it, and the combat gap (16.4 against Witch Time's
10.3 ms) is elsewhere. Witch Time measured its wins on AMD (RADV, and AMD on Windows), where this should matter; not
tested here (no AMD GPU). Keeping it on costs nothing measurable on NVIDIA.
