# Barriers in uniform control flow (issue 2, 2026-11-05)

The reporter's 0.1.6 runs on Intel Iris Xe (driver 32.0.101.7092) still failed, so turning off the pool copies on integrated GPUs did not fix issue 2. The log confirmed it was active ("no video memory copies of pools (integrated GPU)").

- 6 of 9 starts: `VK_ERROR_DEVICE_LOST` about 4.5 s after frame 142-152, the first frame with the game's compute jobs. On Windows a GPU hang is reset after 2 s (TDR), so this reads as a GPU hang.
- 3 of 9 starts, the first three with an empty pipeline cache: `0xc0000005` in `igvk64.dll` at offset `0x14D102`, access to `0x170`, on a runtime thread, about 6.2 s in.
- The Sunshine and RTSS Vulkan layers are ruled out for the device loss: with them really unloaded it was lost on 3 of 3 starts.

## Cause

A program with flow runs as a loop over a `switch` on its pc, one block per pass. A `BAR` became an `OpControlBarrier` inside one case of that switch, so on a given pass only the invocations whose pc was at that block reached it, and the workgroup's other subgroups reached it on other passes or not at all. Vulkan requires every invocation of the workgroup to reach a workgroup barrier together (uniform control flow), and leaves anything else undefined. NVIDIA counts barrier arrivals per thread and got through. Intel hangs. The converge mode (lanes wait for the subgroup's lowest pc) lines up the lanes of one subgroup only, and a workgroup of 64 is 2 to 8 subgroups.

Three of the game's compute programs have a `BAR` inside flow: `614fd3a3`, `66b9a528` and `24e1f54e` (their SPIR-V hashes in `passtime-20261102/spv-c5/`). The first two are the compute chain measured at the verse exit, and frame 142 on the Iris Xe is the first frame logged with compute work. In the corpus `nvn-port-20261029/corpus-nvn-7`, 15 of 73 compute programs are of this kind.

## Fix

`source/src/nx/gpu/shader/spirv_translate.c`, `bar_phases` (compute programs with flow and a `BAR`):

- The instruction after a `BAR` starts a block. A `BAR` stores its pc past it and ends the invocation's pass through the inner loop. `EXIT` stores the finished pc instead of returning, so a finished invocation stays for the barriers.
- An outer loop around the inner one: after the inner loop every invocation is at a `BAR` or finished, and the workgroup meets at one barrier there, which is uniform. A shared word per pass parity says whether any invocation stopped at a `BAR`, read between two barriers and cleared after the second, so no invocation clears a word another has set for a later pass. The workgroup goes round again while one did.
- Exited invocations pass the barriers, as an exited thread does on Maxwell.

## Checks

- `snx-shaderc --validate` on `corpus-nvn-7`: 5616 programs, all translated, all pass `spirv-val --target-env vulkan1.3` (SPIRV-Tools from `diagnostics/spirv-tools/`, which rejects a broken module). The 15 barrier programs each have 3 barriers (the clear at the start and the two per phase), an outer and an inner loop, and no barrier inside the `switch`.
- RTX 3080, replay of `recordings/20261103-122329` in the isolated copy (`range-barrier-base` on 0.1.6, `range-barrier-b1` on the fix, 120 s each, cutscene 60, resolution 1.6): both exit 0, no failed pipelines, production saves unchanged. Compute at most 0.4 and 0.7 ms a frame, mean GPU 6.25 and 6.51 ms. A 60 s rerun with `SNX_NVN_SPV_DUMP` shows three compute programs with the phase loop ran (`6245b23d`, `a6e28a0a`, `bd8e7d8d`, the new hashes of the three barrier programs).
- **The colours.** Screenshots 0-3 match. From screenshot 4 on (the room with the dartboard and the map) the colours are rotated on 0.1.6: mean RGB (98, 93, 87) against (87, 98, 93) on the fix. `room-0.1.6.png` has a blue lantern and blue marks, `room-fix.png` a red lantern and red marks. The user confirmed the fix's colours are the game's. So the barrier programs build the colour grading, and on NVIDIA too the old translation got it wrong.

Build b1: `Bayo3_120-b1.exe` (`bcaee9a6...`) = 0.1.6 + this fix. Not installed. Test zip for the reporter: `release/B3-Express-0.1.6-intel-barrier-test.zip` (`18df193a...`, the exe and a README), for the user to attach to issue 2. Not tested on Intel: the reporter is needed for that. Whether the driver crash at `igvk64.dll+0x14D102` has the same cause is not known. It happens only with an empty pipeline cache, when shader preparation compiles compute stages on worker threads, which fits Intel's compiler failing on one of these programs, but 0.1.4 crashed the same way once. If it remains, `SNX_VK_GPL=0` (no pipeline libraries) is the next thing to try.
