# NVN route, phase 2: shadow (2026-10-29, first pass)

Plan: `../nvn-route-plan-20261029/PLAN.md`. `SNX_NVN_SHADOW=1` (off by default) routes NVN calls and, for the
functions in `source/src/nx/hle/nvn/shadow.c`, runs the game's driver and then notes what the call asked for. Two
chapter-route runs (`../desktop-launch-20261028/range-shadow-shadow-1`, `-2`): exit 0, 4,800 frames, production saves
unchanged. Inventory of run 2: `shadow-inventory-run2.txt`. Eight targeted tests pass; shadow.c compiles without
warnings under -Wall -Wextra.

## Matches B2recomp (SDK 13.3.2 against B2's SDK 3.6.0 assumptions)

- Texture formats: 29 builder and 10 view formats (RGBA8/sRGB, BC1-BC7, ASTC, depth, float). **All in B2's table.**
- Vertex attribute formats: 7. **All in B2's table.**
- Shader control sections: magic `0x98761234` on 100% of stages; machine-code header `0x12345678` at `data` on 99.1%;
  code length at control+0x6f8 equal to the header's own length field (e.g. 0x1d0 and 0x1d0); no NV assembly text
  (B2's Vulkan path uses machine code anyway). **Same layout as B2 reads.**
- No multisampling (samples 0 only), no sparse or virtual pools (pool flags 0x12/0x22/0x24/0x62/0xa2), window of 3
  textures at present interval 1, one command buffer per queue submit (~1 per frame).
- Sizes: 8 pools (2.6 GiB, largest 1.35 GiB); 79k buffers created (415 MiB); 4.5k 3D sizes and packaged layouts.

## Mismatches the port must handle

1. **Programs per stage.** Every `nvnProgramSetShaders` has one stage, and `nvnCommandBufferBindProgram` binds by stage:
   vertex 0x1 648k, fragment 0x2 690k, tessellation control 0x8 and evaluation 0x10 8.9k each, compute 0x20 22k.
   B2's `cmd_program` ignores the stage bits (the last bound program replaces the whole set). The front end must keep
   one program per stage and combine them per draw.
2. **Tessellation.** B2's Vulkan back end stops on any draw with a geometry or tessellation stage
   (`nvn_vk_draw.c:2613`) and its translator rejects those stages (`mx_translate.c:1511`). Our Maxwell-to-SPIR-V
   translator handles them: use ours behind B2's program interface (as the plan proposed).
3. **Headerless code.** 74 stages (0.9%) have no `0x12345678` header: the data starts with Maxwell code
   (`e22007f0 001fc000 ...`, at GPU addresses 0x4004xxxxx). B2 skips their machine code; the port reads code at
   data+0 when the header is absent.

## Not yet covered by the shadow

Command-buffer recording semantics (which state each command carries), texture and sampler pool layouts and handles,
uniform/storage binding patterns, queue fences and present. Next pass: these, then phase 3 (port) with the three
changes above as its first work items.

## Second pass: pools, handles, bindings, recording, fences, present (run `range-shadow-shadow-3`)

Exit 0, 4,800 frames, production saves unchanged. Inventory: `shadow-inventory-run3.txt`.

- **Texture handles: 12,877,213 from the driver, 100% equal to B2's `textureID | samplerID << 20`** (low 32 bits).
  Image handles: 12,578, 100% equal to B2's `id`. B2 answers these itself, and the game's shaders read them as
  bindless handles, so B2's encoding is the driver's. Ids up to texture 4,680, image 906, sampler 360; pools of
  32,768 texture and 512 sampler descriptors (two each).
- Frame: per frame one acquire (indices 0, 1, 2 in turn), one queue wait, one recording, one submit of one command
  buffer, one queue fence (ALL_GPU_COMMANDS_COMPLETE, no flags), one flush, one present; texture and sampler pools
  set once per recording. ~0.65 command-buffer fences per frame. Every submitted handle came from a recent
  EndRecording. 14 counted as submitted twice is likely a false positive (the ring matches handle values, which a
  later recording can repeat): check before relying on it.
- Bindings, most per stage seen: textures 16, uniform buffers 14, storage buffers 9, images 2. B2's draw state holds
  16 uniform buffers, 32 storage buffers and 32 textures per stage (`nvn_vk_draw.c:230`): enough.
- Barriers: flag sets 0x12, 0x21, 0x38, 0x20 (ordering plus texture/shader invalidation).

## Phase 2 result

Everything recorded so far matches B2recomp except the three mismatches above (per-stage program binding,
tessellation, headerless shader code). Not shadowed: the content of recorded commands (draw state, vertex/index
buffers, render targets, clears, copies); phase 3's unit tests and phase 4's title screen check those against the
driver directly.
