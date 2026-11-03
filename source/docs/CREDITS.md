<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Licenses and credits

ScarletNX is [GPL-3.0-or-later](../LICENSE).
Generated modules include runtime ABI code. Game assets, keys and firmware are excluded.

| Component | License |
| --- | --- |
| FFmpeg | LGPL-2.1-or-later |
| hactool / cJSON / ConvertUTF | ISC / MIT / Unicode license |
| LibHac storage code, ported into hactool by `cmake/hactool-libhac.patch` | MIT |
| OpenSSL | Apache 2.0 |
| Open Sans 1.101 (the shared fonts' stand-in, `cmake/shared_fonts.cmake`) | Apache 2.0 ([text](../dist/licenses/OpenSans/LICENSE)) |
| yuzu's controller button glyphs (`font_nintendo_extended.cpp`, as Eden carries it) | GPL-2.0-or-later |
| LZ4 (the library, `lib/`) | BSD-2-Clause |
| Vulkan-Headers | Apache 2.0 |
| volk / Vulkan Memory Allocator | MIT |
| SDL3 | zlib |
| lunasvg / plutovg (the splash's SVGs) | MIT / MIT, with FreeType's rasterizer and stroker (FreeType License) and stb (MIT or public domain) |
| GUI VGA BIOS font (SeaBIOS `vgafont16`, originally Joseph Gil's font collection) | Public domain |

The GUI font's original notice and pinned source are preserved in
[`src/gui/FONT-NOTICE.txt`](../src/gui/FONT-NOTICE.txt).

Code ported into the repository, and assets taken from other projects. In these places, the files whose header names
a source keep that source's copyright lines and license, most with the revision they came from; the drawables of
`dist/controls/` have theirs in [its README](../dist/controls/README.md):

| Where | From | License |
| --- | --- | --- |
| `cmake/hactool-libhac.patch` (the storages it adds to hactool) | LibHac, as Ryubing maintains it | MIT |
| `src/3ds/gpu/`, `src/3ds/audio/`, `src/3ds/services/dsp.c` | Azahar (Citra) | GPL-2.0-or-later |
| `src/3ds/gpu/shader.c` and `shader.h` (the instruction fields) | nihstro | BSD-3-Clause |
| `src/common/cpu/` (the recompiler's instruction semantics: `a64.c`, `a64_templates.inc`, the LLVM emitters and the libgccjit emitters derived from them, the A32/T32 block compiler) | QEMU's `target/arm/tcg/` | LGPL-2.1-or-later |
| `src/common/cpu/aarch64/` (`cpu.h`, `cpu_helpers.c` and `helpers/`, the generated code's FP, SIMD, integer and crypto helpers) | QEMU's `target/arm/` and its softfloat, with the SoftFloat-2a notice | LGPL-2.1-or-later, SoftFloat-2a, BSD |
| `src/nx/gpu/gpu.c`, `src/nx/gpu/maxwell/` (the DMA pusher and puller, the 3D, macro, DMA, 2D and compute engines) | Eden and yuzu (`video_core/`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/shader/` (`shader.c`, `decode.c`, and `maxwell_opcodes.def`, the encodings `tools/shaderdec` generates the decoder from) | Eden and yuzu (`shader_recompiler/frontend/maxwell/`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/` (`host1x.c`, `nvdec.c`, `vic.c`) | Eden and yuzu (`video_core/host1x/`, the nvdrv host1x devices), with Ryujinx's CDMA pusher as Eden carries it | GPL-3.0-or-later, GPL-2.0-or-later, MIT |
| `src/nx/loader/loader.c` (the defaults for a program without an NPDM) | Eden and yuzu (`file_sys/program_metadata.cpp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/kernel/`, `src/nx/hle/svc/`, `src/nx/memory/`, `src/nx/hle/services/`, `src/nx/hle/filesystem/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/hle/services/misc.c` (the six default Miis, from Eden's `mii/types/raw_data.cpp`) | Ryujinx, as Eden carries it | MIT ([text](../dist/licenses/Ryujinx/LICENSE.txt)) |
| `src/common/network/`, `src/nx/hle/services/network/` | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/video/vp9_rebuild.cpp`, `src/nx/gpu/video/vic.comp` (compiled into `vic_spv.h`) | Eden and yuzu | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/vk/shaders/ffx/` and the constants in `src/nx/gpu/vk/fsr1.c` (FSR 1, compiled into `fsr1_spv.h`) | AMD FidelityFX Super Resolution 1, GPUOpen-Effects/FidelityFX-FSR | MIT |
| `src/nx/gpu/texture/astc.comp` (the ASTC decoder shader, compiled into `astc_spv.h`) | Eden and yuzu (`astc_decoder.comp`) | GPL-3.0-or-later, GPL-2.0-or-later |
| `src/nx/gpu/vk/texture.c` `bl_row_bytes` (block-linear rows with the texture header's tile width spacing) | Eden and yuzu (`texture_cache/util.cpp`, `StrideAlignment` and `NumGobs`), with Ryujinx's size calculator as they carry it | GPL-3.0-or-later, GPL-2.0-or-later, MIT |
| `src/nx/gpu/texture/astc.c` and `astc.h` (the CPU ASTC decoder the tests check the shader against) | Eden and yuzu (`textures/astc.cpp`), with the University of North Carolina at Chapel Hill's FasTC decoder in it | GPL-3.0-or-later, GPL-2.0-or-later, Apache 2.0 |
| `tests/ipc_message.c` (the HIPC, CMIF and TIPC request builders) | libnx | ISC |
| `dist/controls/*.xml`, and `controls.png` made from them | yuzu's Android overlay, as Eden and rEden carry it | GPL-3.0-or-later |

Dynarmic (0BSD) and its GPL-3.0-or-later fork were consulted for the recompiler's instruction semantics. No code was
ported from them.

Citron's yuzu-derived `shader_recompiler/frontend/maxwell/translate/impl/surface_load_store.cpp`
(Copyright 2021 yuzu Emulator Project, GPL-2.0-or-later) was consulted for the 2D-array SUST
coordinate layout. The SPIR-V implementation uses ScarletNX's existing emitter and layered image paths.

[Nexium](https://nexium-emu.org/) (MIT) is credited for its research and development, which SwitchRecomp, the
project ScarletNX started from, took as its basis.

Installed license texts: `share/licenses/scarletnx/`. AppImage system-library licenses
and package manifest: `share/licenses/`. Component license files contain the full terms.

Community credits: [README](../README.md#special-thanks).

Wimsy ([SVG metadata](../dist/wimsy.svg)): drawn by Liz, GPL-3.0-or-later.
The splash's Fuddles ([SVG metadata](../dist/splash/wimsy-fuddles.svg)): Wimsy with Fuddles' tails, circuits and
colours, by AO55 after Liz's Wimsy, GPL-3.0-or-later.

`src/nx/hle/scene60.c`: how Bayonetta 3's 30 FPS scenes are timed, and the design of running them at 60, come from
[Witch Time](https://github.com/Zexyen/Witch-Time) (GPL-3.0). The code is written from its notes.

`src/nx/gpu/vk/nvn_res.c`, storage buffers the GPU writes kept in video memory (`mir_own`, `own_merge`, `own_land_now`,
`own_idle`): the design, keeping a compute-written buffer on the GPU, landing it in guest memory only when something
needs the bytes there, merging against the snapshot taken when it was filled so a word the game wrote meanwhile stays
the game's, and landing idle buffers without a wait, comes from [Witch Time](https://github.com/Zexyen/Witch-Time)
(`runtime/nvn_vk_draw.c`, its "CB" compute buffers, GPL-3.0-or-later). The code is written for this renderer's video
memory copies.
