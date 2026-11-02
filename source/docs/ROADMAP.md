<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Status + roadmap

Recorded run, 2026-10-11: Catherine: Full Body USA (`0100BF00112C0000`, v2.0),
New Game cutscene, Vulkan, 60 FPS, no audio. Older traces use the Asian TID (`0100BC4010896000`).

Implemented: Clang/CMake, Linux/Windows CI, SDL3/Vulkan, shader caches, component libs,
`audout` PCM playback, viewport Y swizzles and framebuffer H/V flips.

## Next work

- **Regression:** ARM ISA, IPC, VM, GPU-cache tests. Headless smoke runs + reference images.
- **Diagnostics:** stricter warnings, shader vector-boundary tests at R254/RZ. Import thread-dump
  reader and Maxwell instruction scanner.
- **Audio:** `audren` splitters, voice biquads, delay/reverb/biquad effects. Validate hwopus playback.
- **GPU:** per-channel scheduling so a blocked channel cannot stall its fence producer.
  90-degree framebuffer rotation, attachment feedback, BCn fallback.
- **CPU:** ARM NaN propagation/default-NaN and directed FP rounding. Thread suspension at WFE.
  32-bit games run (A32 and Thumb, `src/nx/runtime/arm.c`), checked only with the synthetic program
  (`tests/nx_arm32_smoke.py`): a real one, ldr:ro's NROs in a 32-bit process, the Thumb32 DSP and
  saturating instructions (QADD16, SMULxy, SSAT), and TBB/TBH targets ahead of time.
- **Host:** vsync during Win32 window drags, native Windows/gamepad coverage.
- **Performance:** runtime/guest profiling, Vulkan measurements, broader title coverage.

Commands: [development](DEVELOPMENT.md).

## Design

NVN HLE remains exploratory: Catherine requests 468 exports and uses ~178. It could replace
command decoding while retaining shader translation and host1x.
