# Desktop memory-range candidate, 2026-10-29

Paused at the user's request. Nothing from this experiment is deployed to `output/`.

## Results at pause

The paired ABBA comparison completed: control runs 2/3 and candidate runs 2/3 all reached 4,800 frames, exited 0, and preserved production saves. The last two 600-present street intervals averaged **17.555 FPS control** and **19.2175 FPS candidate**, approximately **9.47% higher FPS**. Draw counts were approximately 542,700-543,100 per interval. See `comparison.json`. This is a measured improvement in this stationary scene, not a 60 FPS solution or an A/V synchronization fix.

The direct installed-runtime check (`range-installed-1`) failed at about 20 seconds with 0xc0000005. Windows Application Error event 1000 identifies the owned PID 7716 and game-DLL RVA **0xc225a4**, the same instruction as the previously diagnosed skipped-stack-guard crash. See `installed-crash-event.json`. No new debugger memory-region capture was taken, so the prior mechanism plus matching site is the evidence; do not claim a new independent guard-region measurement.

`range-chapter-profile-1` was stopped early at the user's request to summarize now. Treat it as interrupted, regardless of its process exit code; it is not a completed validation pass. The earlier 60-second profiled candidate boot passed, as did three normal candidate game runs (one 3,600-frame and two 4,800-frame runs). The new stack regression fails against the old source and passes against the fix.

`deploy.py` is prepared but **has not been run**. Resume with a complete profiled gameplay validation, review the installed-crash evidence and rollback plan, then decide deployment. The candidate executable is `candidate/Bayo3_120.exe`; control is `control/Bayo3_120.exe`. Current production runtime remains `da49319f...`, and the Ryzen O0 game DLL remains `03dcc7fd...`. No save, INI, production runtime or production dependency was changed.

## Changes

- `source/src/nx/gpu/maxwell/maxwell3d.c`: dirty tracking walks each live contiguous GMMU mapping with `gmmu_run`, instead of calling `gmmu_translate` once per page and joining the results. Unmapped gaps are skipped one page at a time; subsequent mappings and aliases are still checked. No mappings are cached across calls and no synchronization/readback requirement is removed.
- `source/src/nx/runtime/platform/plat_win32.c`: explicitly sized thread stacks are committed with CreateThread's normal size semantics, rather than reserving 32 MiB but initially committing only a small default. This addresses the previously captured unprobed large-frame crash. Threads requesting size zero retain the executable's default. Extra commit charge for guest threads is intentional.
- Regression coverage: dirty aliases, holes, discontinuous mappings, remapping, partial pages and empty ranges in `render_enable`; real GMMU run extents in `nvdrv_service`; new Windows stack commitment test. Existing instance-array and draw-texture fixtures implement the range translator too.

## Build isolation

`before.json` records the installed executable/module/settings/save hashes. Original changed runtime sources are preserved as `before-*.c`.

Both paired executables use `debug-build/writeback-masked/libscarletnx-masked.a`, the deployed runtime lineage, with only `plat_win32.c.obj` and `maxwell3d.c.obj` replaced. Both contain the same stack repair. Control uses the original page-walking maxwell3d source; candidate uses the contiguous-range source. They link against the same freshly built CMake dependencies. This is an isolated archive-based rebuild, not a claim that all other experimental edits in the source tree are ready to deploy.

`build.json` records exact source/binary hashes; each variant retains its link arguments. `build.py` compiles the replacement objects using the runtime's CMake flags, including `-fno-strict-aliasing`, and builds the needed dependencies through Ninja. Builds and timing-sensitive game runs must remain serial.

Local tools are under `source/deps/`; no system packages were installed. The project's bootstrap downloaded its pinned native build tools. `../desktop-launch-20261028/setup_llvm_sdk.py` extracted LLVM development packages; `setup_posix_tools.py` extracted a compatible MSYS make/Perl/shell toolset. Package names and verified repository SHA-256 values are saved in each toolset's `packages.json`. Git's minimal Perl and mixed native/MSYS shells were unsuitable for the OpenSSL build; the final build uses the self-contained POSIX toolset.

The successful CMake configuration is `source/build/desktop-isolated`, with `SNX_DEPLOY=OFF`, GUI/GNUABI/3DS off, and:

- `SNX_LLVM_CONFIG=source/deps/llvm-sdk/clang64/bin/llvm-config.exe` (absolute path)
- `PERL_EXECUTABLE=source/deps/posix-tools/usr/bin/perl.exe` (absolute path)
- `SNX_OPENSSL_MAKE=source/deps/posix-tools/usr/bin/make.exe` (absolute path)

The isolated test game uses the SDK's `libLLVM-23.dll`, while production retains its original dependency. Runtime/module/dependency differences from the installed build must be considered before deployment. The paired comparison shares these dependencies.

## Verification so far

- All six CTest targets pass: `render_enable`, `instance_array`, `draw_texture`, `write_pages`, `nvdrv_service`, `platform_win32_stack`.
- The stack regression test fails against `before-plat_win32.c` (expected exit 1, reserved page) and passes against the fixed source (committed page).
- `range-boot-profile-1`: candidate, profile enabled, 60-second boot, exit 0, title screenshot inspected, production saves unchanged.
- `range-control-1`: exited before the requested frame limit, no final unattended frame/hash marker, different camera position. Exclude from a matched speedup claim.
- `range-candidate-1`: reached 3,600 frames, exit 0, street screenshot inspected, production saves unchanged. Last interval mixes loading/street and is not a clean steady-scene comparison.
- Subsequent tests request 4,800 frames and use the connected controller's VID/PID in `SDL_GAMECONTROLLER_IGNORE_DEVICES` for the child process only. This avoids unscripted controller input without changing device/system settings.

Game artifacts remain under `../desktop-launch-20261028/range-*`. The runner restores only its isolated save from the preserved seed and hashes production saves before/after each run.

## Additional user report

The user reports audio desynchronization in cutscenes at poor frame rates. Source inspection shows audio is clocked every 5 ms of host time, independently of presented frames; video/render stalls could therefore cause drift. Existing logs do not establish the specific cause or measure A/V offset. The affected chapter/cutscene was requested asynchronously. No audio-clock change or synchronization fix has been applied or claimed. Do not hide the throughput problem by blindly slowing audio.

## Resumed validation, 2026-10-29 (later session)

- No game/build processes were left running. Production runtime `da49319f...`, module `03dcc7fd...`, INI `35c0db7f...` and all four save files matched `before.json` before testing.
- `range-chapter-profile-1` is confirmed interrupted: its log ends with a clean `Exit.` at 99 s (window closed), which is why it reports exit 0. It is excluded.
- **`range-chapter-profile-2`: candidate `23ed2b7f...`, profile on, same script as -1, full 180 s, exit 0, elapsed 182.5 s, production saves unchanged.** Screenshots show the Practice Mode combo screen and the final street scene rendering correctly.
- Skipped draws (9,126 by 153 s) are not a regression: control runs skip ~14.6k by run end too. They coincide with `shader a543d510e5ff71c4 (stage 4) not translated: unknown opcode e370000000000000`, a separate translator gap.
- Profile, runtime samples over ~165 s, stack-only page-walk build (`stack-chapter-profile-1`) vs candidate: `gmmu_translate` 3,813 -> 492, `gpu_mem_dirty_since` 1,780 -> (out of top list), `gmmu_run` 909 -> 2,663. Dirty-tracking cost roughly halves (~6.5k -> ~3.2k), consistent with the ABBA gain.
- `deploy.py` gated on the interrupted `range-chapter-profile-1`. It now requires `range-chapter-profile-2` and rejects runs that timed out or ran under 175 s. It has still **not** been run: deployment awaits the user's decision.

### Where the remaining time goes (street, 125-182 s of profile-2)

No thread is saturated: busiest thread 65% of one core, whole process 1.65 cores, GPU utilization ~36%. MainThread is parked in `NvOsEventWait` (nvhost event). Each 600-present interval does ~4,400 `vk_finish` calls (~7.3/frame) costing ~9.1 s of ~31 s. The game is serialization-bound: CPU and GPU alternate waiting. `vk_write_changed` (top runtime symbol, ~9.4k samples) is already SSE2-merged and memory-bound; vectorizing it further will not move FPS materially. The next lever is fewer per-frame GPU completion round trips (which finish callers, and whether their results can stay GPU-resident or be landed asynchronously), keeping matched gameplay runs as acceptance.
