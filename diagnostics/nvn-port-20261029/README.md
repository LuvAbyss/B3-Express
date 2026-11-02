# NVN route, phase 3 started: design refined, host-to-driver calls proven (2026-10-29)

Plan: `../nvn-route-plan-20261029/PLAN.md` (phase 3 rewritten from this note). Nothing deployed; everything here is
off by default.

## Design change: keep Nintendo's driver for objects, replace only the GPU-work path

Reading B2recomp for this phase showed that B2 does not compute texture storage sizes or alignments itself: it builds
a fake builder and runs code lifted from Bayonetta 2's SDK 3.6 driver (`runtime/nvn_sdk.c`). Bayonetta 3's own
SDK 13.3.2 driver already runs, recompiled, in our runtime. So instead of porting B2's front end wholesale:

- **Nintendo's driver keeps every CPU-side object function**: builders, setters, getters, storage size and alignment,
  pools and their registration, handles, texture and pool initialisation. Exact SDK 13.3.2 semantics, no layouts to
  guess, no lifted 3.6 code that may differ.
- **Host handlers replace only what produces GPU work**: command-buffer recording (binds, state, draws, copies,
  clears, barriers), queue submit and fences, sync waits, window acquire and present. That is where the driver writes
  the Maxwell commands our GPU thread decodes today.
- **The host learns objects through the driver's own getters** (`nvn_call`), and takes B2recomp's command recording
  and Vulkan back end as the model for the GPU side, with phase 2's three fixes (programs per stage, tessellation
  through our translator, headerless code).

Why this is safe to switch: objects stay in the driver's layout, so host handlers for commands and the driver's
object functions never disagree about memory.

## Built

- `nvn_call(c, name, args, nargs, &ret)` (`source/src/nx/hle/nvn/nvn.c`): runs a driver function from a handler on
  the calling thread, restoring x0-x31, pc, NZCV, v0-v31, FPCR and FPSR afterwards. Routed functions are found by a
  name index; others through the driver's `nvnDeviceGetProcAddress`, with the name written below the guest SP and the
  call's SP below it, using the device the game first passed GetProcAddress.
- `tests/nvn_seam.c`: nvn_call by name, through GetProcAddress for a name the game never asked for (then by name),
  an unknown name, the name below SP and the call's SP below it, the callee's SP unchanged otherwise, and every
  register restored after a callee that changes x5, x30, pc, Z and FPSR. Eight targeted tests pass.
- `shadow.c` dual check (`SNX_NVN_SHADOW=1`): a host copy of each texture builder from its setters
  (SetDefaults, Flags, Target, Format, Size2D/3D, Levels, Samples); after `nvnTextureInitialize`, eight driver
  getters on the new texture plus `GetTextureAddress` and `GetStorageSize` through `nvn_call`, compared with the copy.
- Build `Bayo3_120.exe` here; module source snapshot `nvn-module-snapshot.patch`.

## Result (run `../desktop-launch-20261028/range-port-dual-1`, chapter route with movement)

Exit 0, 4,800 frames, production saves unchanged.

**5,089 textures read back; flags, target, format, width, height, depth, levels and samples equal to the builder
copy in 5,089 of 5,089 each.** GPU address and storage size non-zero for all. Getters not found: 0
(`GetTextureAddress` and others the game never requested resolved through the driver's GetProcAddress). 3 textures
had no builder copy (a builder copied by the game, not set up through the setters).

## Next in phase 3

1. Host command recording for the functions B3 calls (B2's `rec` model): binds, state objects read through driver
   getters, draws, clears, copies, barriers, in a host command list per NVN command buffer, still running the driver
   alongside and checking nothing (dual mode).
2. Programs per stage and shader translation through our Maxwell-to-SPIR-V translator.
3. Memory pools as Vulkan memory, textures and buffers as Vulkan objects, created from driver getters.
4. Then phase 4: switch the GPU-work functions to the host path behind `SNX_NVN_FRONTEND=2`.

## Step 1: host command lists in dual mode (2026-10-29)

`source/src/nx/hle/nvn/cmdlist.c` (`SNX_NVN_CMDLIST=1`, ahead of the shadow's handlers): between BeginRecording and
EndRecording every command of the 49 B3 records (`b3-called.txt`) goes into a host list with its operation, x1-x7, the
low words of v0-v3 and a copy of what it points at (state objects 4/8/0x18 bytes, binding arrays, clear and blend
colours, render-target and view arrays), sizes as B2recomp's `rec`/`rec_state`/`rec_ranges`. The driver still
records and runs everything. At QueueSubmitCommands each handle is matched with its list. Tests pass; no warnings.

Run `../desktop-launch-20261028/range-port-cmdlist-1` (street, 4,800 frames, exit 0, saves unchanged; FPS 21.72 /
21.92), `cmdlist-run1.txt`:

- **4,531 submitted handles, all 4,531 with a complete host list.** Unreadable pointers 0, data overflows 0, no
  command outside a recording.
- Per frame (one submit): ~503 draws and dispatches, ~1,600 commands (BindUniformBuffers ~1,000, vertex
  attribute/stream/buffer binds ~360 each, BindProgram ~320, BindTextures ~250, DrawElements ~330,
  MultiDrawElementsIndirectCount ~137, SetRenderTargets ~60, Barrier ~39). Largest list 7,315 commands; ~320 KiB of
  data copied per frame.
- Open check: by 151 s the lists held ~2.28 M draw and dispatch calls, the current renderer ~2.16 M executed
  (interpolated between its 600-present lines), ~5% fewer. Likely (not verified): MultiDrawElementsIndirectCount
  reads its count from a GPU buffer, which can be 0, and the renderer skips empty draws while the host counts calls.

Next: replay side. Programs per stage through our shader translator, then memory pools, textures and buffers as Vulkan
objects from the driver's getters, then executing a host list against them.

## Step 2: shaders through our translator (2026-10-29, first pass)

`SNX_NVN_CORPUS=dir` (with `SNX_NVN_SHADOW=1`): every `nvnProgramSetShaders` stage goes into a shader corpus
(`gpu/shader/corpus.c`) that `snx-shaderc` translates offline, keeping the translator off the game's threads. Framing
as B2recomp reads it: graphics = 0x50-byte program header at data+0x30 plus control+0x6f8 bytes of instructions, the
stage from the header's type; compute (control+0x714 = 5, or no header) = code at data with 80 zero bytes in front (as
the renderer frames compute code, `vk/compute.c:683`), work group from control+0x748..0x750 and shared memory from
control+0x754. The saved length is `shader_hash`'s end. Corpora stay local in this folder (game data).
`spirv-val` is not installed, so "translated" means the translator produced SPIR-V, not that it validated.

Runs `range-port-corpus-1..3` (chapter route, all exit 0, 4,800 frames, saves unchanged). Last capture: 7,895 stages
saved, 5,616 distinct.

| stage | distinct | translated offline | unsupported |
| --- | ---: | ---: | --- |
| vertex | 1,779 | 1,771 (99.5%) | 8 |
| tessellation control / evaluation | 3 / 6 | 3 / 4 | 2 |
| fragment | 3,755 | 2,381 (63%) | 893 all-zero opcode, 257 other opcode, 223 untracked surface handle, 1 ST |
| compute | 73 | 8 | mostly opcodes that are guest addresses (see below) |

Cross-check with the renderer's own corpus of the same run (`SNX_SHADER_CORPUS`, 241 programs translated live, the
rest came from the disk cache): **all 223 renderer graphics programs are byte-for-byte in the NVN capture**
(`trim_to_renderer.py`). The renderer's 241 translate offline 239. Of the 23 NVN fragment stages that failed but are
the same program as a renderer one, **21 translate when cut to the renderer's length**; the other 2 stop at opcode
`e370000000000000`, the translator gap the live renderer already logs (shader a543d510...). So the fragment failures
are the program's end not being found: `shader_hash` follows BRX jump tables through `g_shader_cb_read`, which the
renderer supplies at draw time and the capture does not, and the window then runs into the bytes after the program.

Where the jump tables are, from B2recomp (`nvn_vk_m4.c:110`, `nvn_vk_draw.c:2526`): the 8 words at control+0x6f0;
word 3 is the size and word 4 the offset from data of the program's own constant data, bound as constant buffer 1.
So the NVN path can resolve BRX from the program itself.

Compute: 16 of the renderer's 18 compute programs are inside NVN captures, 14 of them 0x100 bytes after data and 2 at
data. The entry offset is not yet known (likely the same control words); with it the window would start at the code.

Next for step 2: give the capture and `snx-shaderc` the program's constant buffer 1 (control+0x6f0 words 3 and 4) so
BRX ends resolve, find compute's entry offset, then recount. The remaining real translator gap seen so far is opcode
`e370000000000000`. After that, step 3: pools, textures and buffers as Vulkan objects.

## Step 2 done: every shader found, 99.75% translate (2026-10-29)

Two corrections to the capture (`shadow.c` `corpus_stage`) and one to the offline tool:

1. **Constant buffer 1.** Each stage's own constant data (size = word 3, offset = word 4 of control+0x6f0, as B2recomp
   binds it) is saved beside its corpus entry as `<key>_<stage>.cb1`; `tools/snx_shaderc.c` answers reads of
   constant buffer 1 from it while that entry translates. BRX jump tables resolve, so a program's end is found.
2. **Compute framing for SDK 13.3.2**, from the programs the renderer runs (18 of them): data starting with
   `0x12345679` is a 0x100-byte header, code from data+0x100 (its length, control+0x6f8, counted from there) and
   constant data at data + word 5 (= word 4 + 0x100). Without the header the code is at data and the constant data at
   data + word 4 (B2recomp's SDK 3.6 reading). All 14 headered programs the renderer ran start at +0x100, whatever
   words 4 and 5 hold; a word-difference rule tried first got the cases without constant data wrong.

Capture `range-port-corpus-7` (chapter route, exit 0, 4,800 frames, saves unchanged), `corpus-nvn-7`,
`shaderc-nvn-7-verbose.txt`:

| stage | distinct | translated offline |
| --- | ---: | ---: |
| vertex | 1,779 | 1,779 |
| tessellation control / evaluation | 3 / 6 | 3 / 4 |
| fragment | 3,755 | 3,743 |
| compute | 73 | 73 |
| **all** | **5,616** | **5,602 (99.75%)** |

The 14 left all stop at opcode `e370000000000000` (12 fragment, 2 tessellation evaluation): the translator gap the live
renderer already logs. Translation without `spirv-val` (not installed), so this is "produced SPIR-V".

**All 241 programs the renderer translated in the same run are byte-for-byte in the capture, from the capture's entry
on**: compute 18/18, vertex 85/85, tessellation 2/2 and 2/2, fragment 134/134.

Superseded captures 1-6 were deleted (regenerable, ~290 MB). Kept: `corpus-nvn-7`, `corpus-renderer`,
`corpus-trimmed` (the trim test), `shadow-snapshot.patch`.

Next (step 3): memory pools as Vulkan memory, textures and buffers as Vulkan objects, created from the driver's
getters, then replaying a host command list with programs per stage. For the live path, translation happens at bind or
draw time with constant buffer 1 taken from the program the same way.

## Step 3a: every resource a frame uses is known (2026-10-29)

`cmdlist.c` (`SNX_NVN_CMDLIST=1`) now keeps a resource registry beside the command lists, from the driver itself:
memory pools at `nvnMemoryPoolInitialize` (GPU address and size through `nvnMemoryPoolGetBufferAddress` and
`GetSize`), textures at `nvnTextureInitialize` (GPU address, storage size, format, width, height, target through the
texture getters), Finalize of both, and the ids `nvnTexturePoolRegisterTexture`/`RegisterImage` and
`nvnSamplerPoolRegisterSampler(Builder)` give out. At each submit every reference in the list is looked up: buffer
addresses (vertex, index, uniform and storage ranges, indirect, parameter, copy and clear addresses) in a live pool's
GPU range; texture handles (id = bits 0-19, sampler = bits 20-31) and image handles in the registered ids; render
targets (colour array and depth) among the live textures.

Run `../desktop-launch-20261028/range-port-resources-1` (street, exit 0, 4,800 frames, saves unchanged; this run
reached the lighter workload state, 27.2 / 27.7 FPS, not comparable), `resources-run1.txt`:

| reference | count | known | not known |
| --- | ---: | ---: | ---: |
| buffer address | 45,125,625 | 45,125,625 | 0 |
| render target | 314,871 | 314,871 | 0 |
| image handle | 11,880 | 11,880 | 0 |
| texture handle | 13,102,755 | 13,098,872 | 3,883 |

8 pools, 5,019 textures made (4,170 live at the end), driver getters failed 0. Every logged unknown texture handle is
the raw value 1 (from ~16 s, loading): likely a placeholder the game binds into unused slots. To confirm.

So the host knows, from the driver, every buffer, texture, image and target each frame uses. Next, step 3b: Vulkan
objects for them (pool memory as buffers over the guest's storage, textures as images from their getters, in the
renderer's device), then replaying one host list into them behind a switch, against the current renderer's frame.

## Step 3b, first part: memory pools as Vulkan buffers over the guest's memory (2026-10-29)

- `gpu/vk/device.c`: `VK_EXT_external_memory_host` is enabled when the device has it (it has no features), with
  `minImportedHostPointerAlignment` in `g_vk.host_import_align`. Enabling it changes nothing else the renderer does.
- `gpu/vk/nvn_res.c` / `nvn_res.h` (plain types, for hle): `vk_nvn_pool_import(cpu, size)` imports a pool's guest
  storage (`GP(cpu)`) as host-allocation memory, host-visible and coherent first, bound to one VkBuffer with transfer,
  uniform, storage, vertex, index and indirect usage. `vk_nvn_pool_check` copies bytes of it on the GPU (its own
  command pool and fence, submitted under `vk_lock`) and compares them with guest memory.
- `cmdlist.c` with `SNX_NVN_IMPORT=1`: at `nvnMemoryPoolInitialize` the pool's CPU storage comes from the driver's
  `nvnMemoryPoolMap`; it is imported and checked at its start and middle.

Runs (street, `SNX_NVN_CMDLIST=1`): `range-port-import-1` and `-2` with import, `range-port-noimport-1` without,
`range-port-off-ext-1` plain; all exit 0, saves unchanged. `import-run2.txt`:

- **All 8 pools imported (2.6 GiB: 1.35 GiB, 853 MiB, 266 MiB, 114 MiB, 43 MiB shader code, 42 MiB, 18 MiB,
  1 MiB) and the GPU's copy equal to guest memory at both places in every one**, in both import runs. Flags seen:
  0x22, 0x24, 0x12, 0xa2 (compressible), 0x62 (shader code).
- Frame rate, last two street intervals, normal workload state: import-2 22.50 / 22.53, no import 19.08 / 21.09,
  plain 18.06 / 18.49. The import does not slow the game. Import-1 ran slower in the street (14.4) and stopped short
  of 4,800 frames before its time ran out; not repeated in import-2, cause unknown (runs this session are noisier
  than earlier ones). Watch for it.
- References resolved in import-2: buffer addresses 60.6 M, all known; texture handles 99.97% (placeholder 1).

The buffers are created and verified but nothing draws with them yet. Next (step 3b, second part): textures as Vulkan
images from the driver's getters (format, size, target, GPU address in an imported pool), uploaded from their pool
storage, then step 3c: replaying one recorded frame into these objects behind a switch and comparing its picture with
the current renderer's.

## Step 3b, second part: textures through the renderer's own descriptors (2026-10-29)

Rather than a second texture implementation, the NVN path can use the renderer's: when the game registers a texture
or image, the driver writes its hardware descriptor (TIC, 32 bytes) into the texture pool, and the renderer builds
textures from exactly these (`vk/texture.c` `tex_from_tic`: format, block-linear layout, levels, ASTC). `cmdlist.c`
records each texture pool (`nvnTexturePoolInitialize`: its memory pool's storage through `nvnMemoryPoolMap`, plus the
offset) and, at every `RegisterTexture`/`RegisterImage`, compares the descriptor written for that id with the driver's
account of the texture (`GetTextureAddress`, width, height; 1D types read with height 1, as `tex_from_tic` does).

Run `../desktop-launch-20261028/range-port-tic-1` (chapter route, exit 0, 4,800 frames, saves unchanged),
`tic-run1.txt`: **15,834 descriptors checked, 15,804 equal to the driver's texture; pool or texture unknown: 0.** The
30 others are all single-layer 2D views (type 1) of 2D array textures (target 4) registered with a view: their
addresses are whole layers past the array's base (for one 400x320 array, descriptors 983 and 984 at +0x12c000 and
+0x258000, layers 1 and 2). Correct descriptors for views, not mismatches.

So a host path needs no texture code of its own: each bound handle's id gives the descriptor in the pool's memory,
which `vk_tex_from_tic` turns into the renderer's texture. Buffers come from the imported pools (part one).

Next, step 3c: replay. Execute one recorded frame's host list on the GPU thread against these (imported pool buffers,
`vk_tex_from_tic` textures, programs per stage through the translator with constant buffer 1, render targets from the
target textures' descriptors), behind a switch, and compare the picture with the current renderer's frame.

## Step 3c: the native draw path, in three parts (started 2026-10-29)

Replaying a frame needs a draw path of the NVN path's own (B2recomp's is `nvn_vk_draw.c`, ~4,800 lines): (1) the
pipeline state the frame binds, decoded; (2) Vulkan pipelines from it plus the translated stages, and descriptors for
buffers, textures and samplers; (3) one recorded frame drawn into its targets and compared with the current renderer's.

### Part 1: pipeline state through the driver's getters

`nvn_call_below` (`nvn.c`): nvn_call with SP lowered first, so a getter can read a state object copy and write its
outputs below the caller's SP without its frames (or a GetProcAddress lookup's) overwriting them. `cmdlist.c` at
submit decodes each distinct bound state object (FNV of its kept bytes) once: blend (target, funcs, equations), colour
(blend enable per target, logic op, alpha test), depth-stencil (test, write, func, stencil test and front func),
polygon (cull, front face, mode, offset enables), channel mask (per target), multisample (samples, alpha to coverage,
enable), vertex attributes (format, relative offset, stream) and streams (stride, divisor).

Run `../desktop-launch-20261028/range-port-state-1` (chapter route, exit 0, 4,800 frames, saves unchanged; 15-17 FPS
with this diagnostic's per-frame hashing), `state-run1.txt`: **every distinct state decoded, getters failed 0.**
Distinct objects: blend 8, colour 5, depth-stencil 9, polygon 3, channel mask 6, multisample 1 (off), vertex
attributes 35, vertex streams 11. The attribute formats (0x11, 0x16, 0x22, 0x25, 0x27, 0x29, 0x2e) are the seven the
phase 2 shadow saw at the setters; some streams have divisor 1 (instancing). The whole frame's fixed-function state
is a few dozen objects: pipelines can be cached by them.

Next, part 2: Vulkan pipelines from these values and the translated stages (programs per stage), in the translator's
descriptor layout (`shader.h` SNX_VKB_*), and the bindings: uniform/storage/vertex/index ranges in the imported pool
buffers, textures through `vk_tex_from_tic`, samplers through the sampler pool's descriptors.

### Part 2, first half: program stages loaded live as Vulkan modules (2026-10-30)

- `gpu/vk/nvn_res.c` `vk_nvn_shader`: one stage translated with the renderer's own `vk_translate_spirv` under
  `vk_lock` (the translator's state is shared, and the renderer translates under the same lock), its constant
  buffer 1 read from guest memory through a reader of its own, then `vkCreateShaderModule`. Module and `ShaderInfo`
  kept in a table (8,192) for the pipelines.
- `cmdlist.c` (`SNX_NVN_CMDLIST=1`): `nvnProgramSetShaders` records each program's stages with step 2's SDK 13.3.2
  framing; the first time a submitted list binds a program (`BindProgram` a[0]) its stages are translated and loaded.

Run `../desktop-launch-20261028/range-port-modules-1` (chapter route, exit 0, 4,800 frames, saves unchanged),
`modules-run1.txt`: **every bound program stage loaded as a module except two**: vertex 93/93, tessellation
control 2/2, evaluation 2/2, fragment 146/148, compute 16/16. The two are opcode `e370000000000000`, the translator
gap the current renderer shares. These are the stages the frames actually bind (programs created but never bound are
not translated).

Next, part 2 second half: VkPipelines from the decoded state (part 1) plus these modules, in the renderer's
descriptor set layouts (`g_vk_dslayout`, `shader_vk_set`), with render-target formats from the target textures,
counting pipelines made per frame. Then descriptors: ranges in the imported pool buffers, textures via
`vk_tex_from_tic`, samplers from the sampler pool's descriptors (TSC).

### Part 2, second half: a Vulkan pipeline for every draw (2026-10-30)

Mechanism (`cmdlist.c` "pipelines per draw", `gpu/vk/nvn_res.c` "pipelines"):

- At submit, `check_list` follows the state the lists set, command by command and across lists (the queue's state
  carries on): the program bound to each stage (`BindProgram` stage bits 1 vertex, 2 fragment, 4 geometry, 8 / 0x10
  tessellation, 0x20 compute; a stage the program has no shader for is off), each state object's decoded value
  (part 1's decode now returns it, cached per object), vertex attribute and stream arrays (the rest off), patch size,
  depth clamp, and the render targets: the format a view sets (`nvnTextureViewGetFormat`, read when
  `SetRenderTargets` is recorded, since views are often on the stack) or else the texture's from the driver.
  Blend values now keep 8 bits per field (blend funcs reach 0x64).
- At each draw the stages get the key `vk_shader_get` would give them: vertex, the integer attributes and clip
  (`SNX_SH_NOT_LAST` before tessellation); fragment, integer targets and unfed inputs (the last stage's outputs).
  Each key is its own translation (`stage_variant`, up to 6 per stage): 236 made.
- `vk_nvn_pipeline` turns an `NvnPipeDesc` into the renderer's `PipeKey` and calls its `vk_pipeline_get` (under
  `vk_lock`): the renderer's pipeline layout, set layouts, dynamic state and pipeline cache. NVN formats map to
  Vulkan as B2recomp's `comp_fmt` / `tex_vkfmt` / `vertex_vkfmt`, blend funcs and equations as its `blend_factor` /
  `blend_op`; attribute i reads location i, a binding per stream used, a constant binding for attributes left off.
  Compute: `vkCreateComputePipelines` per module in the same layout. Draws and frames (`nvnQueuePresentTexture`)
  are counted.

Run `../desktop-launch-20261028/range-port-pipe1` (chapter route, exit 0, saves unchanged), `pipe-run1.txt`:
2,197,713 of 2,208,828 draws had a pipeline (99.5%), every dispatch had one; 364 graphics and 17 compute pipelines,
**no creation failed**. All 11,115 draws without one bound one of the two fragment stages with opcode e370.

The fix (shader translator, shared with the current renderer): e370 is **SAM** and e380 **RAM** (set and restore
the active mask), which in all three corpus shaders bracket a run of SHFL. SPIR-V has no active mask; the shuffle
reads the invocations Vulkan runs, so both are no-ops, like IDE (`maxwell_opcodes.def`, `spirv_translate.c`). Inside
the brackets the shaders use **FSWZADD** with swizzles 0xcc and 0x77 and NDV set; the translator only had the four
derivative swizzles. Now any swizzle: each lane's mode, 2 bits by its place in the quad (subgroup lane & 3), gives
A * {1, -1, 1, 0} + B * {1, 1, -1, 1} (Eden's `EmitFSwizzleAdd`, GPL-2.0-or-later), with the subgroup lane input
declared; NDV is ignored as Ryujinx's `Fswzadd` ignores it (MIT). Offline (`shaderc-nvn-8-verbose.txt`): 5,614 of
5,616 corpus stages translate (was 5,602); the 2 left are tessellation evaluation stages with FSWZADD, refused there
by design (only fragment programs have quads), not bound on the route.

Run `../desktop-launch-20261028/range-port-pipe2` (same route, exit 0, saves unchanged, 19.4 / 21.3 FPS normal
state), `pipe-run2.txt`: **every draw (2,242,743) and every dispatch (23,284) has a pipeline, 0 failures**; every
bound stage loads (vertex 90, tess control 2, tess eval 2, fragment 142, compute 17). 360 graphics + 17 compute
pipelines, made in 71 of 4,527 frames (at most 56 in a frame, 1-9 in 62 of them), none after the scene loads.

Known approximations for the replay: tessellation evaluation is keyed with tessellation parameters 0 (where the
driver puts them for SDK 13.3.2 is not yet read), clip distances all enabled (the translator writes only those the
program writes), and the fragment key's storage-image bits are not set.

### Part 2c: every binding of every draw resolved (2026-10-30)

Mechanism (`cmdlist.c` `stage_bind` / `ds_bind` / `draw_descriptors`, `gpu/vk/nvn_res.c` "descriptors"):

- The tracked state also keeps, per NVN stage, the uniform and storage buffer ranges, texture and image handles
  (`BindUniformBuffers`/`BindStorageBuffers`/`BindTextures`/`BindImages`, (cmd, stage, first, count, array)), and the
  texture and sampler pools set (`SetTexturePool`, `SetSamplerPool`; sampler pools recorded at
  `nvnSamplerPoolInitialize` like texture pools). Imported pools carry their GPU address (`vk_nvn_pool_gpu`).
- Each draw's stages (the module of the draw's key) are resolved against what the translator found in them,
  `vk_nvn_stage_check`: constant buffer 1 the program's constants, 3 and up uniform binding n - 3 (a range in an
  imported pool, at the device's uniform alignment); 0 and 2 are made on the host. Storage buffers by where their
  address is in buffer 0: slot (offset - (0x110 + 0x100 * hardware stage), compute 0x310) / 16. Textures by handle:
  bound ones from constant buffer 2 word 8 + 2 * unit; the handle's TIC (bits 0-19) from the texture pool and TSC
  (bits 20-31) from the sampler pool, `vk_tex_from_tic`, then the view and sampler the renderer's draws use
  (`vk_tex_bind_view`, `vk_tex_bind_sampler`), cached per handle per frame. The fragment key now carries the integer
  and depth-compared colour textures (`vk_nvn_tex_key`, as `fs_get_keyed`).
- A slot table logs where the modules' handles and addresses actually were: the check of B2recomp's layout (SDK 3.6)
  against SDK 13.3.2.

Run `../desktop-launch-20261028/range-port-desc1` (chapter route, exit 0, saves unchanged), `desc-run1.txt`:
2,118,007 of 2,156,070 draws and dispatches fully resolved (98.2%). Constant buffers 0, 1, 2 and all storage buffers
(1.49 M) resolved; 6.78 M textures, 48 not. **The layout holds for SDK 13.3.2**: fragment storage buffer addresses at
buffer 0 bytes 0x510-0x590 (0x110 + 0x100 * 4), compute 0x310-0x380, texture handles at buffer 2 words 8-38 (units
0-15). It also showed what B2 never handled: **image handles at buffer 2 word 72 + 2 * unit** (fragment word 74,
compute 72), right after the 32 texture units. Not resolved: images (not bound yet) 8,839; uniform slots a program
declares with nothing bound 29,222; 48 textures.

Fixes: images through the texture pool (`RegisterImage` ids) from word 72 + 2 * unit, as storage-capable textures with
a 2D view (the renderer's fragment storage image rules); a uniform slot with nothing bound reads zeros, as B2recomp
binds it; the 48 textures (logged: shaders declaring a cube sampler whose slot holds a 2D screen-sized target, during
loading) get the declared type's null texture, as the renderer's `stage_set` does.

Run `../desktop-launch-20261028/range-port-desc2` (chapter route, exit 0, saves unchanged, 20.5 / 20.3 FPS normal
state with this per-draw checking), `desc-run2.txt`: **2,075,785 of 2,075,798 draws and dispatches fully resolved**;
images 9,226 of 9,226; uniform buffers 11.94 M plus 34,982 zero slots; storage buffers 1.31 M; textures 6,498,908, the
same 52 not (since bound as the null texture: that build is the replay's).

### Part 3: replaying one frame (2026-10-30, in progress)

Switch: `SNX_NVN_REPLAY=<frame>` (frames counted by `nvnQueuePresentTexture`) with `SNX_NVN_CMDLIST=1
SNX_NVN_IMPORT=1`; `SNX_NVN_REPLAY_OUT=<path without .ppm>`. Debug: `SNX_NVN_REPLAY_DEBUG` bits 1 no culling, 2 no
depth test, 4 no scissor, 8 no alpha test; `SNX_NVN_CLIP=<mask>` clip distances (default 0).

Mechanism (`nvn_res.c` "replay of one frame", `cmdlist.c` `replay_draw` / `ds_dynamic` / `present`):

- From the frame's first submitted list to its present, every draw, clear, dispatch, buffer copy and clear is also
  recorded into a command buffer of the replay's own. Render targets are images of its own per target texture (GPU
  address; cleared at first use, kept in GENERAL), and a texture of the same address sampled later in the frame is
  that image. Pipelines, textures, samplers and null textures are the renderer's; vertex, index, indirect and
  read-only storage buffers are the imported pools; uniform data and constant buffers 0 (storage buffer addresses,
  rounded to the device's alignment) and 2 (handles) are built in a 256 MiB staging buffer. A storage range the
  frame writes (compute, copies, clears, written storage buffers) is copied once and used by every later binding in
  it, so guest memory never changes. Descriptor pools grow by 256 sets.
- Dynamic state from the commands and objects: viewport (upper-left origin: `y + h, -h` as B2recomp; lower-left:
  positive, winding swapped), scissor, depth range, cull and front face (NVN CW 0 / CCW 1), depth and stencil (state
  decode now also returns back-face function and both faces' ops), stencil references and masks, polygon offset,
  blend colour, line width; push constants: GL depth off for `nvnDeviceSetDepthMode` 1, the colour state's alpha test.
- Device features added (device.c): `multiDrawIndirect`, `drawIndirectFirstInstance`, `drawIndirectCount`.
- At the present the replay is submitted after the renderer's commands so far (which upload the textures it samples),
  waited for, and every target read back (`<out>-tNN.ppm`; depth as grey, float targets tone mapped with NaN/Inf
  counts; the presented one also as `<out>.ppm`). A renderer hook (`vk_capture_next_present`) saves the renderer's
  own present of the same buffer, but the window texture's CPU address isn't found yet (logged 0), so it doesn't fire.

The game sets **upper-left origin and near-is-zero depth** (`nvnDeviceSetWindowOriginMode 1`, `SetDepthMode 1`), as
B2recomp assumes.

Runs `range-port-replay1` to `-replay10`: the HUD and the depth pre-pass came out right, the forward lighting pass
(370 draws into the 1440x816 B10G11R11 target) almost empty. Runs `-replay11` to `-replay32` found and fixed, in order:

1. **Descriptor pools used up**: they now grow by 256 sets.
2. **Forward pass failing its depth EQUAL test**: its vertex shaders take Maxwell's instance id as InstanceIndex
   minus the base instance (push constant 0, as the renderer passes register 0x50E). The replay passed 0, so
   instanced draws read the wrong per-instance transforms and missed the pre-pass depth by a little. Now pc[0] is the
   draw's base instance.
3. **Multi-draw indirect (GPU-culled) draws**: the base instance comes from each indirect record, so the translator
   declares BaseInstance (with DrawParameters) for the NVN path's vertex stages (`g_shader_instance_base_builtin`
   when the device has shaderDrawParameters), and the indirect arguments and counts come from the replay's copies of
   ranges this frame's compute wrote.
4. **Magenta HDR**: layered targets (array textures rendered one layer at a time) are now images with layers, and
   their views pick the layer `SetRenderTargets` names (`NvnTarget.layers` / `.layer`, from `nvnTextureGetDepth`
   and the view).
5. **Black post-processing chain**: target 12 (R32F) is written by fragment storage-image stores, which were skipped.
   They now store into the replay's image of that address (storage usage where the format allows it).
6. **Rotated colours**: textures of this frame's targets are viewed as `vk_tex_bind_view` views them: the TIC
   swizzle in memory order (R and B swapped for BGRA formats), depth as R, R, R, 1, the TIC's format, sRGB when the
   TIC and sampler ask for it.

The renderer's window-texture capture uses "the next present" (`vk_capture_next_present(~0)`): the window
textures' pool is not routed, so their CPU address is unknown (logged 0), and the next present is the same frame.

Run `-replay32`: all 1,095 draws, 44 clears and 4 dispatches replayed, none left out; the picture against the
renderer's: mean largest-channel difference 0.70/255, 0.49% of pixels more than 16 apart (particles and sparks).

Run `-replay33` (2026-10-30, after removing the one-off debug logs; `SNX_NVN_CMDLIST=1 SNX_NVN_IMPORT=1
SNX_NVN_REPLAY=4000`, chapter route, exit 0, saves unchanged): **1,062 of 1,062 draws, 44 of 44 clears, 4 of 4
dispatches, 15 written ranges copied, 49 targets, nothing left out.** Against the renderer's picture of the same
frame (`tools/ppm_diff.py`): **mean largest-channel difference 0.89/255, 0.61% of pixels more than 16 apart.**
`replay/replay-f4000-r33-host.png` (host path) and `replay-f4000-r33-renderer.png` (current renderer). The draw
count differs from run 32 because the route lands in one of two workload states.

**Part 3 is done: one whole frame of B3 draws correctly through the NVN host path.** Step 3c is complete, and so is
phase 3 (dual-mode recording, shaders, resources, pipelines, descriptors and a matching frame).

Known approximations left for phase 4: tessellation evaluation is keyed with tessellation parameters 0; clip
distances are off (`SNX_NVN_CLIP`); non-indexed draws ignore instances and base instance (`vkCmdDraw(count, 1,
first, 0)`); depth bounds test off; `nvnCommandBufferDrawTexture`, transform feedback and 8-bit indices are not replayed
(not seen in frame 4000).

Evidence files removed to save space: the replay's PPMs and the run folders' periodic screenshots (`screen_NNNN.ppm`,
2.7 GB over all old runs; logs and summaries kept).
